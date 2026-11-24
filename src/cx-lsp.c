/**
 * @file cx-lsp.c
 * @brief Pure-C Language Server Protocol (LSP) proxy for `.cx` files over `clangd`.
 *
 * Architecture:
 *  - Calls `cx_transpile()` in-process (<1 ms) with `emit_lines = 0, recover = 1`
 *    so `clangd` operates on a deterministic shadow `.cx.c` document whose exact
 *    byte coordinates map 1-to-1 with `.cx` via `cx_span`.
 *  - Serves HTML/native tag and `component` prop completions directly when the
 *    cursor is inside a JSX tag/attribute name outside `{...}`.
 *  - Forwards all C semantics (`hover`, `completion` inside `{}`/closures,
 *    `definition`, `references`, `signatureHelp`, `publishDiagnostics`) to
 *    unmodified `clangd`, remapping URIs and ranges `.cx <-> .c`.
 *  - Zero external runtime dependencies beyond libc, POSIX `poll`/`fork`/`pipe`,
 *    and `cx_transpile.o`.
 */

#define _POSIX_C_SOURCE 200809L

#include "cx_lsp_internal.h"

#define JSMN_STATIC
#include "jsmn.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Dynamic Byte Buffer ──────────────────────────────────────────────── */

void lbuf_free(lsp_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

int lbuf_reserve(lsp_buf *b, size_t extra)
{
	size_t need = b->len + extra + 1;
	if (need <= b->cap)
		return 0;
	size_t ncap = b->cap ? b->cap * 2 : 512;
	while (ncap < need)
		ncap *= 2;
	char *p = (char *)realloc(b->data, ncap);
	if (!p)
		return -1;
	b->data = p;
	b->cap = ncap;
	return 0;
}

int lbuf_append(lsp_buf *b, const char *s, size_t n)
{
	if (n == 0)
		return 0;
	if (lbuf_reserve(b, n) != 0)
		return -1;
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

int lbuf_str(lsp_buf *b, const char *s)
{
	return lbuf_append(b, s, strlen(s));
}

int lbuf_ch(lsp_buf *b, char c)
{
	if (lbuf_reserve(b, 1) != 0)
		return -1;
	b->data[b->len++] = c;
	b->data[b->len] = '\0';
	return 0;
}

int lbuf_printf(lsp_buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int need = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (need < 0) {
		va_end(ap2);
		return -1;
	}
	if (lbuf_reserve(b, (size_t)need) != 0) {
		va_end(ap2);
		return -1;
	}
	vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
	va_end(ap2);
	b->len += (size_t)need;
	return 0;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = (char *)malloc(n + 1);
	if (!p)
		return NULL;
	if (n > 0)
		memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

/* ── Minimal Owned JSON DOM (backed by jsmn.h) ────────────────────────── */

jval *jval_new(jkind k)
{
	jval *v = (jval *)calloc(1, sizeof(*v));
	if (v)
		v->kind = k;
	return v;
}

jval *jval_new_null(void)
{
	return jval_new(J_NULL);
}

jval *jval_new_bool(int b)
{
	jval *v = jval_new(J_BOOL);
	if (v)
		v->bval = b ? 1 : 0;
	return v;
}

jval *jval_new_int(long long n)
{
	char tmp[32];
	int len = snprintf(tmp, sizeof(tmp), "%lld", n);
	jval *v = jval_new(J_NUM);
	if (v && len > 0) {
		v->str = xstrndup(tmp, (size_t)len);
		v->slen = (size_t)len;
	}
	return v;
}

jval *jval_new_num_raw(const char *s, size_t n)
{
	jval *v = jval_new(J_NUM);
	if (v) {
		v->str = xstrndup(s, n);
		v->slen = n;
	}
	return v;
}

jval *jval_new_strn(const char *s, size_t n)
{
	jval *v = jval_new(J_STR);
	if (v) {
		v->str = xstrndup(s ? s : "", n);
		v->slen = n;
	}
	return v;
}

jval *jval_new_str(const char *s)
{
	return jval_new_strn(s ? s : "", s ? strlen(s) : 0);
}

void jval_free(jval *v)
{
	if (!v)
		return;
	free(v->str);
	if (v->kind == J_ARR) {
		for (size_t i = 0; i < v->arr.len; i++)
			jval_free(v->arr.items[i]);
		free(v->arr.items);
	} else if (v->kind == J_OBJ) {
		for (size_t i = 0; i < v->obj.len; i++) {
			free(v->obj.keys[i]);
			jval_free(v->obj.vals[i]);
		}
		free(v->obj.keys);
		free(v->obj.vals);
	}
	free(v);
}

int jval_push(jval *arr, jval *item)
{
	if (!arr || arr->kind != J_ARR || !item)
		return -1;
	if (arr->arr.len >= arr->arr.cap) {
		size_t ncap = arr->arr.cap ? arr->arr.cap * 2 : 8;
		jval **p = (jval **)realloc(arr->arr.items, ncap * sizeof(*p));
		if (!p)
			return -1;
		arr->arr.items = p;
		arr->arr.cap = ncap;
	}
	arr->arr.items[arr->arr.len++] = item;
	return 0;
}

jval *jval_get(const jval *obj, const char *key)
{
	if (!obj || obj->kind != J_OBJ || !key)
		return NULL;
	for (size_t i = 0; i < obj->obj.len; i++) {
		if (strcmp(obj->obj.keys[i], key) == 0)
			return obj->obj.vals[i];
	}
	return NULL;
}

int jval_set(jval *obj, const char *key, jval *val)
{
	if (!obj || obj->kind != J_OBJ || !key || !val)
		return -1;
	for (size_t i = 0; i < obj->obj.len; i++) {
		if (strcmp(obj->obj.keys[i], key) == 0) {
			jval_free(obj->obj.vals[i]);
			obj->obj.vals[i] = val;
			return 0;
		}
	}
	if (obj->obj.len >= obj->obj.cap) {
		size_t ncap = obj->obj.cap ? obj->obj.cap * 2 : 8;
		char **nk = (char **)realloc(obj->obj.keys, ncap * sizeof(*nk));
		jval **nv = (jval **)realloc(obj->obj.vals, ncap * sizeof(*nv));
		if (!nk || !nv) {
			if (nk)
				obj->obj.keys = nk;
			if (nv)
				obj->obj.vals = nv;
			return -1;
		}
		obj->obj.keys = nk;
		obj->obj.vals = nv;
		obj->obj.cap = ncap;
	}
	obj->obj.keys[obj->obj.len] = xstrndup(key, strlen(key));
	obj->obj.vals[obj->obj.len] = val;
	obj->obj.len++;
	return 0;
}

void jval_del(jval *obj, const char *key)
{
	if (!obj || obj->kind != J_OBJ || !key)
		return;
	for (size_t i = 0; i < obj->obj.len; i++) {
		if (strcmp(obj->obj.keys[i], key) == 0) {
			free(obj->obj.keys[i]);
			jval_free(obj->obj.vals[i]);
			for (size_t j = i + 1; j < obj->obj.len; j++) {
				obj->obj.keys[j - 1] = obj->obj.keys[j];
				obj->obj.vals[j - 1] = obj->obj.vals[j];
			}
			obj->obj.len--;
			return;
		}
	}
}

long long jval_as_int(const jval *v, long long def)
{
	if (!v || v->kind != J_NUM || !v->str)
		return def;
	return strtoll(v->str, NULL, 10);
}

const char *jval_as_str(const jval *v)
{
	if (!v || v->kind != J_STR)
		return NULL;
	return v->str;
}

jval *jval_clone(const jval *v)
{
	if (!v)
		return NULL;
	switch (v->kind) {
	case J_NULL: return jval_new_null();
	case J_BOOL: return jval_new_bool(v->bval);
	case J_NUM:  return jval_new_num_raw(v->str ? v->str : "0", v->slen);
	case J_STR:  return jval_new_strn(v->str ? v->str : "", v->slen);
	case J_ARR: {
		jval *a = jval_new(J_ARR);
		for (size_t i = 0; i < v->arr.len; i++)
			jval_push(a, jval_clone(v->arr.items[i]));
		return a;
	}
	case J_OBJ: {
		jval *o = jval_new(J_OBJ);
		for (size_t i = 0; i < v->obj.len; i++)
			jval_set(o, v->obj.keys[i], jval_clone(v->obj.vals[i]));
		return o;
	}
	}
	return NULL;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void append_utf8_cp(lsp_buf *b, uint32_t cp)
{
	if (cp <= 0x7F) {
		lbuf_ch(b, (char)cp);
	} else if (cp <= 0x7FF) {
		lbuf_ch(b, (char)(0xC0 | (cp >> 6)));
		lbuf_ch(b, (char)(0x80 | (cp & 0x3F)));
	} else if (cp <= 0xFFFF) {
		lbuf_ch(b, (char)(0xE0 | (cp >> 12)));
		lbuf_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		lbuf_ch(b, (char)(0x80 | (cp & 0x3F)));
	} else if (cp <= 0x10FFFF) {
		lbuf_ch(b, (char)(0xF0 | (cp >> 18)));
		lbuf_ch(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
		lbuf_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
		lbuf_ch(b, (char)(0x80 | (cp & 0x3F)));
	}
}

static char *json_unescape(const char *s, size_t n, size_t *out_len)
{
	lsp_buf b = { 0 };
	lbuf_reserve(&b, n);
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\\' && i + 1 < n) {
			i++;
			switch (s[i]) {
			case '"':  lbuf_ch(&b, '"'); break;
			case '\\': lbuf_ch(&b, '\\'); break;
			case '/':  lbuf_ch(&b, '/'); break;
			case 'b':  lbuf_ch(&b, '\b'); break;
			case 'f':  lbuf_ch(&b, '\f'); break;
			case 'n':  lbuf_ch(&b, '\n'); break;
			case 'r':  lbuf_ch(&b, '\r'); break;
			case 't':  lbuf_ch(&b, '\t'); break;
			case 'u':
				if (i + 4 < n) {
					int h0 = hex_val(s[i + 1]), h1 = hex_val(s[i + 2]);
					int h2 = hex_val(s[i + 3]), h3 = hex_val(s[i + 4]);
					if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
						uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
						i += 4;
						/* UTF-16 surrogate pair */
						if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n &&
						    s[i + 1] == '\\' && s[i + 2] == 'u')
						{
							int l0 = hex_val(s[i + 3]), l1 = hex_val(s[i + 4]);
							int l2 = hex_val(s[i + 5]), l3 = hex_val(s[i + 6]);
							if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
								uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
								if (lo >= 0xDC00 && lo <= 0xDFFF) {
									cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
									i += 6;
								}
							}
						}
						append_utf8_cp(&b, cp);
						break;
					}
				}
				lbuf_ch(&b, 'u');
				break;
			default:
				lbuf_ch(&b, s[i]);
				break;
			}
		} else {
			lbuf_ch(&b, s[i]);
		}
	}
	if (!b.data) {
		b.data = xstrndup("", 0);
		b.len = 0;
	}
	if (out_len)
		*out_len = b.len;
	return b.data;
}

static jval *jval_from_tokens(
        const char *js, const jsmntok_t *toks, int ntoks, int *idx)
{
	if (*idx >= ntoks)
		return NULL;
	const jsmntok_t *t = &toks[(*idx)++];
	const char *s = js + t->start;
	size_t n = (size_t)(t->end - t->start);

	switch (t->type) {
	case JSMN_STRING: {
		size_t ulen = 0;
		char *u = json_unescape(s, n, &ulen);
		jval *v = jval_new(J_STR);
		if (v) {
			v->str = u;
			v->slen = ulen;
		} else {
			free(u);
		}
		return v;
	}
	case JSMN_PRIMITIVE:
		if (n == 4 && memcmp(s, "true", 4) == 0)
			return jval_new_bool(1);
		if (n == 5 && memcmp(s, "false", 5) == 0)
			return jval_new_bool(0);
		if (n == 4 && memcmp(s, "null", 4) == 0)
			return jval_new_null();
		return jval_new_num_raw(s, n);
	case JSMN_ARRAY: {
		jval *a = jval_new(J_ARR);
		for (int i = 0; i < t->size; i++) {
			jval *el = jval_from_tokens(js, toks, ntoks, idx);
			if (el)
				jval_push(a, el);
		}
		return a;
	}
	case JSMN_OBJECT: {
		jval *o = jval_new(J_OBJ);
		for (int i = 0; i < t->size; i++) {
			if (*idx >= ntoks)
				break;
			const jsmntok_t *kt = &toks[(*idx)++];
			size_t klen = 0;
			char *key = json_unescape(js + kt->start,
			                          (size_t)(kt->end - kt->start), &klen);
			jval *val = jval_from_tokens(js, toks, ntoks, idx);
			if (key && val)
				jval_set(o, key, val);
			else
				jval_free(val);
			free(key);
		}
		return o;
	}
	default:
		return jval_new_null();
	}
}

jval *jval_parse(const char *js, size_t len)
{
	int cap = 256;
	jsmntok_t *toks = NULL;
	int r = 0;
	for (;;) {
		jsmntok_t *nt = (jsmntok_t *)realloc(toks, (size_t)cap * sizeof(*toks));
		if (!nt) {
			free(toks);
			return NULL;
		}
		toks = nt;
		jsmn_parser p;
		jsmn_init(&p);
		r = jsmn_parse(&p, js, len, toks, (unsigned int)cap);
		if (r == JSMN_ERROR_NOMEM) {
			cap *= 2;
			continue;
		}
		break;
	}
	if (r <= 0) {
		free(toks);
		return NULL;
	}
	int idx = 0;
	jval *root = jval_from_tokens(js, toks, r, &idx);
	free(toks);
	return root;
}

static void json_escape_append(lsp_buf *b, const char *s, size_t n)
{
	lbuf_ch(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"':  lbuf_str(b, "\\\""); break;
		case '\\': lbuf_str(b, "\\\\"); break;
		case '\b': lbuf_str(b, "\\b"); break;
		case '\f': lbuf_str(b, "\\f"); break;
		case '\n': lbuf_str(b, "\\n"); break;
		case '\r': lbuf_str(b, "\\r"); break;
		case '\t': lbuf_str(b, "\\t"); break;
		default:
			if (c < 0x20) {
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
				lbuf_str(b, esc);
			} else {
				lbuf_ch(b, (char)c);
			}
			break;
		}
	}
	lbuf_ch(b, '"');
}

void jval_serialize(const jval *v, lsp_buf *b)
{
	if (!v) {
		lbuf_str(b, "null");
		return;
	}
	switch (v->kind) {
	case J_NULL:
		lbuf_str(b, "null");
		break;
	case J_BOOL:
		lbuf_str(b, v->bval ? "true" : "false");
		break;
	case J_NUM:
		lbuf_append(b, v->str ? v->str : "0", v->str ? v->slen : 1);
		break;
	case J_STR:
		json_escape_append(b, v->str ? v->str : "", v->slen);
		break;
	case J_ARR:
		lbuf_ch(b, '[');
		for (size_t i = 0; i < v->arr.len; i++) {
			if (i > 0)
				lbuf_ch(b, ',');
			jval_serialize(v->arr.items[i], b);
		}
		lbuf_ch(b, ']');
		break;
	case J_OBJ:
		lbuf_ch(b, '{');
		for (size_t i = 0; i < v->obj.len; i++) {
			if (i > 0)
				lbuf_ch(b, ',');
			json_escape_append(b, v->obj.keys[i], strlen(v->obj.keys[i]));
			lbuf_ch(b, ':');
			jval_serialize(v->obj.vals[i], b);
		}
		lbuf_ch(b, '}');
		break;
	}
}

/* ── LSP Framed Stream Transport ──────────────────────────────────────── */

typedef struct {
	int    fd;
	char  *buf;
	size_t len;
	size_t cap;
	int    eof;
} lsp_stream;

static void stream_free(lsp_stream *st)
{
	free(st->buf);
	st->buf = NULL;
	st->len = 0;
	st->cap = 0;
}

static int stream_read_once(lsp_stream *st)
{
	if (st->fd < 0 || st->eof)
		return 0;
	if (st->len + 4096 > st->cap) {
		size_t ncap = st->cap ? st->cap * 2 : 8192;
		while (ncap < st->len + 4096)
			ncap *= 2;
		char *p = (char *)realloc(st->buf, ncap);
		if (!p)
			return -1;
		st->buf = p;
		st->cap = ncap;
	}
	ssize_t r = read(st->fd, st->buf + st->len, st->cap - st->len);
	if (r == 0) {
		st->eof = 1;
		return 0;
	}
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0;
		st->eof = 1;
		return -1;
	}
	st->len += (size_t)r;
	return 1;
}

static int ci_starts_with(const char *s, size_t n, const char *pfx)
{
	size_t plen = strlen(pfx);
	if (n < plen)
		return 0;
	for (size_t i = 0; i < plen; i++) {
		if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i]))
			return 0;
	}
	return 1;
}

/* Pop one complete LSP JSON body from stream; returns 1=got msg, 0=need more, -1=err */
static int stream_pop_msg(lsp_stream *st, char **out_body, size_t *out_len)
{
	/* Find "\r\n\r\n" header terminator */
	size_t hdr_end = 0;
	int found = 0;
	for (size_t i = 0; i + 3 < st->len; i++) {
		if (st->buf[i] == '\r' && st->buf[i + 1] == '\n' &&
		    st->buf[i + 2] == '\r' && st->buf[i + 3] == '\n')
		{
			hdr_end = i + 4;
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;

	/* Parse Content-Length header */
	long content_len = -1;
	size_t p = 0;
	while (p < hdr_end) {
		size_t ls = p;
		while (p < hdr_end && st->buf[p] != '\n')
			p++;
		size_t le = p;
		if (p < hdr_end)
			p++;
		if (le > ls && st->buf[le - 1] == '\r')
			le--;
		if (ci_starts_with(st->buf + ls, le - ls, "content-length:")) {
			const char *v = st->buf + ls + 15;
			while (v < st->buf + le && (*v == ' ' || *v == '\t'))
				v++;
			content_len = strtol(v, NULL, 10);
		}
	}
	if (content_len < 0)
		return -1;

	size_t total = hdr_end + (size_t)content_len;
	if (st->len < total)
		return 0;

	char *body = xstrndup(st->buf + hdr_end, (size_t)content_len);
	if (!body)
		return -1;

	size_t rem = st->len - total;
	if (rem > 0)
		memmove(st->buf, st->buf + total, rem);
	st->len = rem;

	*out_body = body;
	*out_len = (size_t)content_len;
	return 1;
}

static int write_all(int fd, const char *data, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)w;
	}
	return 0;
}

int send_lsp_json(int fd, const jval *msg)
{
	if (fd < 0 || !msg)
		return -1;
	lsp_buf body = { 0 };
	jval_serialize(msg, &body);
	char hdr[64];
	int hlen = snprintf(hdr, sizeof(hdr), "Content-Length: %zu\r\n\r\n", body.len);
	int rc = 0;
	if (write_all(fd, hdr, (size_t)hlen) != 0 ||
	    write_all(fd, body.data ? body.data : "", body.len) != 0)
	{
		rc = -1;
	}
	lbuf_free(&body);
	return rc;
}

/* ── Line/Column <-> Byte Offset Index (UTF-16 LSP columns) ───────────── */

void line_index_free(line_index *li)
{
	free(li->offs);
	li->offs = NULL;
	li->count = 0;
}

void line_index_build(line_index *li, const char *s, size_t len)
{
	line_index_free(li);
	size_t cap = 64;
	li->offs = (uint32_t *)malloc(cap * sizeof(uint32_t));
	if (!li->offs)
		return;
	li->offs[0] = 0;
	li->count = 1;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '\n') {
			if (li->count >= cap) {
				cap *= 2;
				uint32_t *p = (uint32_t *)realloc(li->offs, cap * sizeof(uint32_t));
				if (!p)
					break;
				li->offs = p;
			}
			li->offs[li->count++] = (uint32_t)(i + 1);
		}
	}
}

uint32_t line_index_pos_to_off(
        const line_index *li, const char *s, size_t len,
        long long line, long long utf16_col)
{
	if (!li || li->count == 0)
		return 0;
	if (line < 0)
		return 0;
	if ((size_t)line >= li->count)
		return (uint32_t)len;

	uint32_t start = li->offs[line];
	uint32_t end = ((size_t)line + 1 < li->count) ? li->offs[line + 1] : (uint32_t)len;
	if (end > start && s[end - 1] == '\n')
		end--;
	if (end > start && s[end - 1] == '\r')
		end--;

	uint32_t p = start;
	long long col = 0;
	while (p < end && col < utf16_col) {
		unsigned char c = (unsigned char)s[p];
		if (c < 0x80) {
			p += 1;
			col += 1;
		} else if ((c & 0xE0) == 0xC0) {
			p += 2;
			col += 1;
		} else if ((c & 0xF0) == 0xE0) {
			p += 3;
			col += 1;
		} else if ((c & 0xF8) == 0xF0) {
			p += 4;
			col += 2; /* surrogate pair in UTF-16 */
		} else {
			p += 1;
			col += 1;
		}
	}
	if (p > end)
		p = end;
	return p;
}

void line_index_off_to_pos(
        const line_index *li, const char *s, size_t len, uint32_t off,
        long long *out_line, long long *out_col)
{
	if (!li || li->count == 0) {
		*out_line = 0;
		*out_col = 0;
		return;
	}
	if (off > (uint32_t)len)
		off = (uint32_t)len;

	/* Binary search for largest line whose start <= off */
	size_t lo = 0, hi = li->count;
	while (lo + 1 < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (li->offs[mid] <= off)
			lo = mid;
		else
			hi = mid;
	}

	uint32_t p = li->offs[lo];
	long long col = 0;
	while (p < off) {
		unsigned char c = (unsigned char)s[p];
		if (c < 0x80) {
			p += 1;
			col += 1;
		} else if ((c & 0xE0) == 0xC0) {
			p += 2;
			col += 1;
		} else if ((c & 0xF0) == 0xE0) {
			p += 3;
			col += 1;
		} else if ((c & 0xF8) == 0xF0) {
			p += 4;
			col += 2;
		} else {
			p += 1;
			col += 1;
		}
	}
	*out_line = (long long)lo;
	*out_col = col;
}

jval *make_lsp_pos(long long line, long long col)
{
	jval *p = jval_new(J_OBJ);
	jval_set(p, "line", jval_new_int(line));
	jval_set(p, "character", jval_new_int(col));
	return p;
}

jval *make_lsp_range(long long sl, long long sc, long long el, long long ec)
{
	jval *r = jval_new(J_OBJ);
	jval_set(r, "start", make_lsp_pos(sl, sc));
	jval_set(r, "end", make_lsp_pos(el, ec));
	return r;
}

jval *cx_slice_to_range(const line_index *li, const char *src, size_t len, cx_slice sl)
{
	long long sln = 0, sc = 0, eln = 0, ec = 0;
	line_index_off_to_pos(li, src, len, sl.off, &sln, &sc);
	line_index_off_to_pos(li, src, len, sl.off + sl.len, &eln, &ec);
	return make_lsp_range(sln, sc, eln, ec);
}

/* ── URI, Path & Open Document Table ──────────────────────────────────── */

int is_cx_uri(const char *uri)
{
	if (!uri)
		return 0;
	size_t n = strlen(uri);
	return n >= 3 && strcmp(uri + n - 3, ".cx") == 0;
}

int is_cxh_uri(const char *uri)
{
	if (!uri)
		return 0;
	size_t n = strlen(uri);
	return n >= 4 && strcmp(uri + n - 4, ".cxh") == 0;
}

int is_cx_or_cxh_path(const char *path)
{
	return is_cx_uri(path) || is_cxh_uri(path);
}

static int is_tracked_uri(const char *uri)
{
	return is_cx_uri(uri) || is_cxh_uri(uri);
}

static char *cx_uri_to_c_uri(const char *cx_uri)
{
	size_t n = strlen(cx_uri);
	char *out = (char *)malloc(n + 3);
	if (!out)
		return NULL;
	memcpy(out, cx_uri, n);
	out[n] = '.';
	out[n + 1] = 'c';
	out[n + 2] = '\0';
	return out;
}

char *uri_to_path(const char *uri)
{
	const char *p = uri;
	if (!p)
		return xstrndup("", 0);
	if (strncmp(p, "file://", 7) == 0)
		p += 7;
	lsp_buf b = { 0 };
	while (*p) {
		if (p[0] == '%' && p[1] && p[2]) {
			int hi = hex_val(p[1]), lo = hex_val(p[2]);
			if (hi >= 0 && lo >= 0) {
				lbuf_ch(&b, (char)((hi << 4) | lo));
				p += 3;
				continue;
			}
		}
		lbuf_ch(&b, *p++);
	}
	if (!b.data)
		return xstrndup("", 0);
	return b.data;
}

char *path_to_uri(const char *path)
{
	if (!path)
		return xstrndup("file://", 7);
	lsp_buf b = { 0 };
	lbuf_str(&b, "file://");
	for (const char *p = path; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c <= 0x20 || c == '%' || c == '#' || c == '?' || c >= 0x7F) {
			lbuf_printf(&b, "%%%02X", (unsigned)c);
		} else {
			lbuf_ch(&b, (char)c);
		}
	}
	return b.data ? b.data : xstrndup("file://", 7);
}

static void doc_free(cx_doc *d)
{
	free(d->cx_uri);
	free(d->c_uri);
	free(d->cx_path);
	free(d->cx_text);
	cx_result_free(&d->trans);
	line_index_free(&d->cx_lines);
	line_index_free(&d->c_lines);
	free(d);
}

static void doc_retranspile(lsp_server *srv, cx_doc *d)
{
	cx_result_free(&d->trans);
	cx_options opt;
	memset(&opt, 0, sizeof(opt));
	opt.filename = d->cx_path ? d->cx_path : "<input>.cx";
	opt.emit_lines = 0;  /* exact shadow .c coordinates for clangd */
	opt.recover = 1;     /* tolerate mid-keystroke incomplete edits */
	opt.build_index = 1; /* flat POD metadata for LSP intelligence */
	cx_transpile(&opt, d->cx_text ? d->cx_text : "", d->cx_len, &d->trans);
	line_index_build(&d->cx_lines, d->cx_text ? d->cx_text : "", d->cx_len);
	line_index_build(&d->c_lines, d->trans.c_code ? d->trans.c_code : "", d->trans.c_len);
	ws_index_resolve_includes(srv, d);
	ws_index_update_doc(srv, d);
}

static void doc_apply_changes(cx_doc *d, const jval *changes)
{
	if (!changes || changes->kind != J_ARR)
		return;
	for (size_t i = 0; i < changes->arr.len; i++) {
		const jval *ch = changes->arr.items[i];
		const jval *text_v = jval_get(ch, "text");
		const char *new_txt = jval_as_str(text_v);
		if (!new_txt)
			continue;
		size_t new_len = text_v->slen;
		const jval *range = jval_get(ch, "range");
		if (!range || range->kind != J_OBJ) {
			/* Full document replacement */
			free(d->cx_text);
			d->cx_text = xstrndup(new_txt, new_len);
			d->cx_len = new_len;
			line_index_build(&d->cx_lines, d->cx_text, d->cx_len);
		} else {
			/* Incremental range replacement */
			const jval *st = jval_get(range, "start");
			const jval *en = jval_get(range, "end");
			if (!st || !en)
				continue;
			uint32_t s_off = line_index_pos_to_off(
			        &d->cx_lines, d->cx_text, d->cx_len,
			        jval_as_int(jval_get(st, "line"), 0),
			        jval_as_int(jval_get(st, "character"), 0));
			uint32_t e_off = line_index_pos_to_off(
			        &d->cx_lines, d->cx_text, d->cx_len,
			        jval_as_int(jval_get(en, "line"), 0),
			        jval_as_int(jval_get(en, "character"), 0));
			if (e_off < s_off)
				e_off = s_off;
			size_t next_len = d->cx_len - (e_off - s_off) + new_len;
			char *buf = (char *)malloc(next_len + 1);
			if (!buf)
				continue;
			memcpy(buf, d->cx_text, s_off);
			memcpy(buf + s_off, new_txt, new_len);
			memcpy(buf + s_off + new_len, d->cx_text + e_off, d->cx_len - e_off);
			buf[next_len] = '\0';
			free(d->cx_text);
			d->cx_text = buf;
			d->cx_len = next_len;
			line_index_build(&d->cx_lines, d->cx_text, d->cx_len);
		}
	}
}

cx_doc *find_doc_by_cx_uri(const lsp_server *srv, const char *uri)
{
	if (!srv || !uri)
		return NULL;
	for (size_t i = 0; i < srv->doc_count; i++) {
		if (strcmp(srv->docs[i]->cx_uri, uri) == 0)
			return srv->docs[i];
	}
	return NULL;
}

cx_doc *find_doc_by_c_uri(const lsp_server *srv, const char *uri)
{
	if (!srv || !uri)
		return NULL;
	for (size_t i = 0; i < srv->doc_count; i++) {
		if (strcmp(srv->docs[i]->c_uri, uri) == 0)
			return srv->docs[i];
	}
	return NULL;
}

cx_doc *find_doc_by_path(const lsp_server *srv, const char *path)
{
	if (!srv || !path)
		return NULL;
	for (size_t i = 0; i < srv->doc_count; i++) {
		if (srv->docs[i]->cx_path && strcmp(srv->docs[i]->cx_path, path) == 0)
			return srv->docs[i];
	}
	return NULL;
}

static cx_doc *upsert_doc(lsp_server *srv, const char *cx_uri)
{
	cx_doc *d = find_doc_by_cx_uri(srv, cx_uri);
	if (d)
		return d;
	if (srv->doc_count >= srv->doc_cap) {
		size_t ncap = srv->doc_cap ? srv->doc_cap * 2 : 8;
		cx_doc **nd = (cx_doc **)realloc(srv->docs, ncap * sizeof(*nd));
		if (!nd)
			return NULL;
		srv->docs = nd;
		srv->doc_cap = ncap;
	}
	d = (cx_doc *)calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->cx_uri = xstrndup(cx_uri, strlen(cx_uri));
	d->c_uri = cx_uri_to_c_uri(cx_uri);
	d->cx_path = uri_to_path(cx_uri);
	srv->docs[srv->doc_count++] = d;
	return d;
}

static void remove_doc(lsp_server *srv, const char *cx_uri)
{
	for (size_t i = 0; i < srv->doc_count; i++) {
		if (strcmp(srv->docs[i]->cx_uri, cx_uri) == 0) {
			doc_free(srv->docs[i]);
			srv->docs[i] = srv->docs[--srv->doc_count];
			return;
		}
	}
}

/* ── Coordinate Mapping (.cx <-> .c) ──────────────────────────────────── */

int map_cx_pos_to_c(
        const cx_doc *d, long long cx_line, long long cx_col,
        long long *out_c_line, long long *out_c_col)
{
	uint32_t cx_off = line_index_pos_to_off(
	        &d->cx_lines, d->cx_text, d->cx_len, cx_line, cx_col);
	uint32_t c_off = 0;
	uint8_t kind = 0;
	if (!cx_map_cx_to_c(&d->trans, cx_off, &c_off, &kind)) {
		/* Fallback: nearest preceding span on the same .cx line */
		uint32_t line_start = ((size_t)cx_line < d->cx_lines.count)
		                      ? d->cx_lines.offs[cx_line] : 0;
		const cx_span *best = NULL;
		for (size_t i = 0; i < d->trans.span_count; i++) {
			const cx_span *s = &d->trans.spans[i];
			uint32_t send = s->cx_off + s->cx_len;
			if (send >= line_start && send <= cx_off) {
				if (!best || send > best->cx_off + best->cx_len)
					best = s;
			}
		}
		if (!best)
			return 0;
		c_off = best->c_off + best->c_len;
	}
	line_index_off_to_pos(&d->c_lines, d->trans.c_code, d->trans.c_len,
	                      c_off, out_c_line, out_c_col);
	return 1;
}

int map_c_pos_to_cx(
        const cx_doc *d, long long c_line, long long c_col,
        long long *out_cx_line, long long *out_cx_col)
{
	uint32_t c_off = line_index_pos_to_off(
	        &d->c_lines, d->trans.c_code, d->trans.c_len, c_line, c_col);
	uint32_t cx_off = 0;
	uint8_t kind = 0;
	if (!cx_map_c_to_cx(&d->trans, c_off, &cx_off, &kind)) {
		/* Fallback: nearest span on the same generated C line, else nearest preceding */
		uint32_t line_start = ((size_t)c_line < d->c_lines.count)
		                      ? d->c_lines.offs[c_line] : 0;
		uint32_t line_end = ((size_t)c_line + 1 < d->c_lines.count)
		                    ? d->c_lines.offs[c_line + 1] : (uint32_t)d->trans.c_len;
		const cx_span *best_line = NULL;
		uint32_t best_dist = UINT32_MAX;
		const cx_span *best_prev = NULL;

		for (size_t i = 0; i < d->trans.span_count; i++) {
			const cx_span *s = &d->trans.spans[i];
			uint32_t send = s->c_off + s->c_len;
			if (s->c_off < line_end && send >= line_start) {
				uint32_t dist = (c_off < s->c_off) ? (s->c_off - c_off)
				                : (c_off > send ? c_off - send : 0);
				if (dist < best_dist) {
					best_dist = dist;
					best_line = s;
				}
			}
			if (s->c_off <= c_off) {
				if (!best_prev || s->c_off > best_prev->c_off)
					best_prev = s;
			}
		}
		const cx_span *pick = best_line ? best_line : best_prev;
		if (!pick)
			return 0;
		cx_off = (c_off >= pick->c_off + pick->c_len)
		         ? (pick->cx_off + pick->cx_len) : pick->cx_off;
	}
	line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len,
	                      cx_off, out_cx_line, out_cx_col);
	return 1;
}

int cx_c_range_is_mapped(const cx_doc *d, const jval *range)
{
	if (!d || !range || range->kind != J_OBJ || !d->trans.c_code)
		return 0;
	const jval *st = jval_get(range, "start");
	const jval *en = jval_get(range, "end");
	if (!st || !en)
		return 0;
	uint32_t s_off = line_index_pos_to_off(
	        &d->c_lines, d->trans.c_code, d->trans.c_len,
	        jval_as_int(jval_get(st, "line"), 0),
	        jval_as_int(jval_get(st, "character"), 0));
	uint32_t e_off = line_index_pos_to_off(
	        &d->c_lines, d->trans.c_code, d->trans.c_len,
	        jval_as_int(jval_get(en, "line"), 0),
	        jval_as_int(jval_get(en, "character"), 0));
	if (e_off < s_off)
		return 0;

	/* Require exact containment in a 1:1 copied user-code span (never synthetic boilerplate) */
	for (size_t i = 0; i < d->trans.span_count; i++) {
		const cx_span *s = &d->trans.spans[i];
		if (s->c_len == 0 || s->c_len != s->cx_len)
			continue;
		if (s_off >= s->c_off && e_off <= s->c_off + s->c_len)
			return 1;
	}
	return 0;
}

static void remap_pos_obj_c_to_cx(const cx_doc *d, jval *pos)
{
	if (!d || !pos || pos->kind != J_OBJ)
		return;
	long long cl = jval_as_int(jval_get(pos, "line"), 0);
	long long cc = jval_as_int(jval_get(pos, "character"), 0);
	long long xl = 0, xc = 0;
	if (map_c_pos_to_cx(d, cl, cc, &xl, &xc)) {
		jval_set(pos, "line", jval_new_int(xl));
		jval_set(pos, "character", jval_new_int(xc));
	}
}

static void remap_range_obj_c_to_cx(const cx_doc *d, jval *range)
{
	if (!d || !range || range->kind != J_OBJ)
		return;
	remap_pos_obj_c_to_cx(d, jval_get(range, "start"));
	remap_pos_obj_c_to_cx(d, jval_get(range, "end"));
}

static int remap_range_obj_cx_to_c(const cx_doc *d, jval *range)
{
	if (!d || !range || range->kind != J_OBJ)
		return 0;
	jval *rs = jval_get(range, "start");
	jval *re = jval_get(range, "end");
	long long sl = 0, sc = 0, el = 0, ec = 0;
	int ok = 0;
	if (rs && map_cx_pos_to_c(d, jval_as_int(jval_get(rs, "line"), 0),
	                          jval_as_int(jval_get(rs, "character"), 0), &sl, &sc))
	{
		jval_set(rs, "line", jval_new_int(sl));
		jval_set(rs, "character", jval_new_int(sc));
		ok = 1;
	}
	if (re && map_cx_pos_to_c(d, jval_as_int(jval_get(re, "line"), 0),
	                          jval_as_int(jval_get(re, "character"), 0), &el, &ec))
	{
		jval_set(re, "line", jval_new_int(el));
		jval_set(re, "character", jval_new_int(ec));
	}
	return ok;
}

/* ── Pending Request Table ────────────────────────────────────────────── */

static char *id_to_key(const jval *id)
{
	if (!id)
		return NULL;
	lsp_buf b = { 0 };
	jval_serialize(id, &b);
	return b.data;
}

void track_req_ex(
        lsp_server *srv, const jval *id, req_kind kind,
        const char *cx_uri, jval *extra)
{
	char *key = id_to_key(id);
	if (!key) {
		jval_free(extra);
		return;
	}
	if (srv->req_count >= srv->req_cap) {
		size_t ncap = srv->req_cap ? srv->req_cap * 2 : 16;
		pending_req *nr = (pending_req *)realloc(srv->reqs, ncap * sizeof(*nr));
		if (!nr) {
			free(key);
			jval_free(extra);
			return;
		}
		srv->reqs = nr;
		srv->req_cap = ncap;
	}
	pending_req *r = &srv->reqs[srv->req_count++];
	r->id_key = key;
	r->kind = kind;
	r->cx_uri = cx_uri ? xstrndup(cx_uri, strlen(cx_uri)) : NULL;
	r->extra = extra;
}

static int pop_req(
        lsp_server *srv, const jval *id,
        req_kind *out_kind, char **out_uri, jval **out_extra)
{
	char *key = id_to_key(id);
	if (!key)
		return 0;
	for (size_t i = 0; i < srv->req_count; i++) {
		if (strcmp(srv->reqs[i].id_key, key) == 0) {
			*out_kind = srv->reqs[i].kind;
			*out_uri = srv->reqs[i].cx_uri;
			*out_extra = srv->reqs[i].extra;
			free(srv->reqs[i].id_key);
			srv->reqs[i] = srv->reqs[--srv->req_count];
			free(key);
			return 1;
		}
	}
	free(key);
	return 0;
}

/* ── Recursive `.c -> .cx` JSON Rewriter ──────────────────────────────── */

void remap_json_c_to_cx(const lsp_server *srv, jval *v, const cx_doc *active_doc)
{
	if (!v)
		return;
	if (v->kind == J_ARR) {
		for (size_t i = 0; i < v->arr.len; i++)
			remap_json_c_to_cx(srv, v->arr.items[i], active_doc);
		return;
	}
	if (v->kind != J_OBJ)
		return;

	const cx_doc *scope_doc = active_doc;

	/* 1. Rewrite `uri` (Location, TextDocumentIdentifier, etc.) */
	jval *uri_v = jval_get(v, "uri");
	if (uri_v && uri_v->kind == J_STR) {
		cx_doc *ud = find_doc_by_c_uri(srv, uri_v->str);
		if (ud) {
			jval_set(v, "uri", jval_new_str(ud->cx_uri));
			scope_doc = ud;
		} else {
			/* URI belongs to a non-.cx file (e.g. a `.h` header): do not remap its range! */
			scope_doc = NULL;
		}
	}

	/* 2. Rewrite `targetUri` (LocationLink) */
	jval *turi_v = jval_get(v, "targetUri");
	if (turi_v && turi_v->kind == J_STR) {
		cx_doc *td = find_doc_by_c_uri(srv, turi_v->str);
		if (td) {
			jval_set(v, "targetUri", jval_new_str(td->cx_uri));
			remap_range_obj_c_to_cx(td, jval_get(v, "targetRange"));
			remap_range_obj_c_to_cx(td, jval_get(v, "targetSelectionRange"));
		}
		if (active_doc)
			remap_range_obj_c_to_cx(active_doc, jval_get(v, "originSelectionRange"));
	}

	/* 3. Rewrite `range` / `selectionRange` in the active/resolved document */
	if (scope_doc) {
		remap_range_obj_c_to_cx(scope_doc, jval_get(v, "range"));
		remap_range_obj_c_to_cx(scope_doc, jval_get(v, "selectionRange"));
	}

	/* 4. Rewrite WorkspaceEdit `changes` map keys (`c_uri` -> `cx_uri`) */
	jval *changes_v = jval_get(v, "changes");
	if (changes_v && changes_v->kind == J_OBJ) {
		for (size_t i = 0; i < changes_v->obj.len; i++) {
			cx_doc *cd = find_doc_by_c_uri(srv, changes_v->obj.keys[i]);
			if (cd) {
				free(changes_v->obj.keys[i]);
				changes_v->obj.keys[i] = xstrndup(cd->cx_uri, strlen(cd->cx_uri));
				remap_json_c_to_cx(srv, changes_v->obj.vals[i], cd);
			}
		}
	}

	/* 5. Recurse into remaining fields */
	for (size_t i = 0; i < v->obj.len; i++) {
		const char *k = v->obj.keys[i];
		if (strcmp(k, "range") == 0 || strcmp(k, "selectionRange") == 0 ||
		    strcmp(k, "targetRange") == 0 || strcmp(k, "targetSelectionRange") == 0 ||
		    strcmp(k, "originSelectionRange") == 0 || strcmp(k, "changes") == 0)
		{
			continue;
		}
		remap_json_c_to_cx(srv, v->obj.vals[i], scope_doc);
	}
}

static void publish_cx_only_diags(lsp_server *srv, const cx_doc *d)
{
	jval *diags = jval_new(J_ARR);
	cx_feat_process_diags(srv, d, diags);
	jval *params = jval_new(J_OBJ);
	jval_set(params, "uri", jval_new_str(d->cx_uri));
	jval_set(params, "diagnostics", diags);
	jval *note = jval_new(J_OBJ);
	jval_set(note, "jsonrpc", jval_new_str("2.0"));
	jval_set(note, "method", jval_new_str("textDocument/publishDiagnostics"));
	jval_set(note, "params", params);
	send_lsp_json(srv->client_out, note);
	jval_free(note);
}

void send_result_to_client(lsp_server *srv, const jval *id, jval *result)
{
	jval *resp = jval_new(J_OBJ);
	jval_set(resp, "jsonrpc", jval_new_str("2.0"));
	jval_set(resp, "id", jval_clone(id));
	jval_set(resp, "result", result ? result : jval_new_null());
	send_lsp_json(srv->client_out, resp);
	jval_free(resp);
}

static void apply_cx_capabilities(jval *caps)
{
	if (!caps || caps->kind != J_OBJ)
		return;

	jval *comp = jval_get(caps, "completionProvider");
	if (!comp || comp->kind != J_OBJ) {
		comp = jval_new(J_OBJ);
		jval_set(caps, "completionProvider", comp);
	}
	jval *trigs = jval_get(comp, "triggerCharacters");
	if (!trigs || trigs->kind != J_ARR) {
		trigs = jval_new(J_ARR);
		jval_set(comp, "triggerCharacters", trigs);
	}
	int has_lt = 0;
	for (size_t i = 0; i < trigs->arr.len; i++) {
		const char *s = jval_as_str(trigs->arr.items[i]);
		if (s && strcmp(s, "<") == 0)
			has_lt = 1;
	}
	if (!has_lt)
		jval_push(trigs, jval_new_str("<"));

	/* Strip capabilities whose wire formats or edits operate in shadow-C
	 * coordinates without range remapping (e.g., delta-encoded semanticTokens
	 * and clang-format). Tree-sitter handles .cx syntax highlighting cleanly. */
	jval_del(caps, "semanticTokensProvider");
	jval_del(caps, "documentFormattingProvider");
	jval_del(caps, "documentRangeFormattingProvider");
	jval_del(caps, "documentOnTypeFormattingProvider");
	jval_del(caps, "inlayHintProvider");

	/* Advertise first-class .cx providers */
	jval_set(caps, "hoverProvider", jval_new_bool(1));
	jval_set(caps, "definitionProvider", jval_new_bool(1));
	jval_set(caps, "documentSymbolProvider", jval_new_bool(1));
	jval_set(caps, "foldingRangeProvider", jval_new_bool(1));
	jval_set(caps, "linkedEditingRangeProvider", jval_new_bool(1));
	jval_set(caps, "codeActionProvider", jval_new_bool(1));
	jval *ren = jval_new(J_OBJ);
	jval_set(ren, "prepareProvider", jval_new_bool(1));
	jval_set(caps, "renameProvider", ren);
}

static char *extract_root_path(const jval *params)
{
	if (!params || params->kind != J_OBJ)
		return NULL;
	const char *ruri = jval_as_str(jval_get(params, "rootUri"));
	if (ruri && ruri[0])
		return uri_to_path(ruri);
	const char *rpath = jval_as_str(jval_get(params, "rootPath"));
	if (rpath && rpath[0])
		return xstrndup(rpath, strlen(rpath));
	const jval *wfs = jval_get(params, "workspaceFolders");
	if (wfs && wfs->kind == J_ARR && wfs->arr.len > 0) {
		const char *wfuri = jval_as_str(jval_get(wfs->arr.items[0], "uri"));
		if (wfuri && wfuri[0])
			return uri_to_path(wfuri);
	}
	return NULL;
}

/* ── Message Handlers ─────────────────────────────────────────────────── */

static void handle_client_msg(lsp_server *srv, jval *msg)
{
	const char *method = jval_as_str(jval_get(msg, "method"));
	jval *id = jval_get(msg, "id");
	jval *params = jval_get(msg, "params");

	if (!method) {
		/* Client response to a server->client request */
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "initialize") == 0) {
		free(srv->root_path);
		srv->root_path = extract_root_path(params);
		if (srv->root_path)
			ws_index_scan_root(srv, srv->root_path);

		if (srv->no_clangd) {
			jval *caps = jval_new(J_OBJ);
			jval_set(caps, "textDocumentSync", jval_new_int(2));
			jval *trigs = jval_new(J_ARR);
			jval_push(trigs, jval_new_str("<"));
			jval_push(trigs, jval_new_str("."));
			jval_push(trigs, jval_new_str(">"));
			jval *comp = jval_new(J_OBJ);
			jval_set(comp, "triggerCharacters", trigs);
			jval_set(caps, "completionProvider", comp);
			apply_cx_capabilities(caps);
			jval *res = jval_new(J_OBJ);
			jval_set(res, "capabilities", caps);
			send_result_to_client(srv, id, res);
			return;
		}
		if (params && params->kind == J_OBJ)
			inject_fallback_flags(params, srv->root_path);
		if (id)
			track_req_ex(srv, id, REQ_INITIALIZE, NULL, NULL);
		send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "shutdown") == 0) {
		srv->shutting_down = 1;
		if (srv->no_clangd) {
			send_result_to_client(srv, id, jval_new_null());
			return;
		}
		send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "exit") == 0) {
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		srv->shutting_down = 2;
		return;
	}

	if (strcmp(method, "workspace/didChangeWatchedFiles") == 0 && params) {
		const jval *changes = jval_get(params, "changes");
		if (changes && changes->kind == J_ARR) {
			for (size_t i = 0; i < changes->arr.len; i++) {
				const char *uri = jval_as_str(jval_get(changes->arr.items[i], "uri"));
				if (is_tracked_uri(uri)) {
					char *path = uri_to_path(uri);
					if (path) {
						ws_index_index_path(srv, path);
						free(path);
					}
				}
			}
		}
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "textDocument/didOpen") == 0 && params) {
		jval *td = jval_get(params, "textDocument");
		const char *uri = td ? jval_as_str(jval_get(td, "uri")) : NULL;
		if (is_tracked_uri(uri)) {
			cx_doc *d = upsert_doc(srv, uri);
			if (d) {
				jval *tv = jval_get(td, "text");
				free(d->cx_text);
				d->cx_text = xstrndup(tv && tv->str ? tv->str : "", tv ? tv->slen : 0);
				d->cx_len = tv ? tv->slen : 0;
				d->version = (int)jval_as_int(jval_get(td, "version"), 1);
				doc_retranspile(srv, d);

				jval_set(td, "uri", jval_new_str(d->c_uri));
				jval_set(td, "languageId", jval_new_str("c"));
				jval_set(td, "text", jval_new_strn(
				        d->trans.c_code ? d->trans.c_code : "", d->trans.c_len));

				if (srv->no_clangd) {
					publish_cx_only_diags(srv, d);
					return;
				}
			}
		}
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "textDocument/didChange") == 0 && params) {
		jval *td = jval_get(params, "textDocument");
		const char *uri = td ? jval_as_str(jval_get(td, "uri")) : NULL;
		cx_doc *d = find_doc_by_cx_uri(srv, uri);
		if (d) {
			d->version = (int)jval_as_int(jval_get(td, "version"), d->version + 1);
			doc_apply_changes(d, jval_get(params, "contentChanges"));
			doc_retranspile(srv, d);

			jval_set(td, "uri", jval_new_str(d->c_uri));
			jval *full_ch = jval_new(J_OBJ);
			jval_set(full_ch, "text", jval_new_strn(
			        d->trans.c_code ? d->trans.c_code : "", d->trans.c_len));
			jval *arr = jval_new(J_ARR);
			jval_push(arr, full_ch);
			jval_set(params, "contentChanges", arr);

			if (srv->no_clangd) {
				publish_cx_only_diags(srv, d);
				return;
			}
		}
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "textDocument/didSave") == 0 && params) {
		jval *td = jval_get(params, "textDocument");
		const char *uri = td ? jval_as_str(jval_get(td, "uri")) : NULL;
		cx_doc *d = find_doc_by_cx_uri(srv, uri);
		if (d) {
			ws_index_update_doc(srv, d);
			jval_set(td, "uri", jval_new_str(d->c_uri));
			if (jval_get(params, "text")) {
				jval_set(params, "text", jval_new_strn(
				        d->trans.c_code ? d->trans.c_code : "", d->trans.c_len));
			}
		}
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		return;
	}

	if (strcmp(method, "textDocument/didClose") == 0 && params) {
		jval *td = jval_get(params, "textDocument");
		const char *uri = td ? jval_as_str(jval_get(td, "uri")) : NULL;
		cx_doc *d = find_doc_by_cx_uri(srv, uri);
		if (d) {
			jval_set(td, "uri", jval_new_str(d->c_uri));
			if (!srv->no_clangd)
				send_lsp_json(srv->clangd_in, msg);
			remove_doc(srv, uri);
			return;
		}
		if (!srv->no_clangd)
			send_lsp_json(srv->clangd_in, msg);
		return;
	}

	/* Document-scoped requests (`completion`, `hover`, `definition`, `rename`, etc.) */
	if (params && strncmp(method, "textDocument/", 13) == 0) {
		jval *td = jval_get(params, "textDocument");
		const char *uri = td ? jval_as_str(jval_get(td, "uri")) : NULL;
		cx_doc *d = find_doc_by_cx_uri(srv, uri);
		if (d) {
			jval *pos = jval_get(params, "position");
			long long xl = pos ? jval_as_int(jval_get(pos, "line"), 0) : 0;
			long long xc = pos ? jval_as_int(jval_get(pos, "character"), 0) : 0;

			req_kind rkind = REQ_DOC_REMAP;
			jval *extra = NULL;

			/* 1. Direct `.cx`-only providers (never need clangd) */
			if (strcmp(method, "textDocument/foldingRange") == 0 && id) {
				send_result_to_client(srv, id, cx_feat_folding_ranges(d));
				return;
			}
			if (strcmp(method, "textDocument/linkedEditingRange") == 0 && pos && id) {
				send_result_to_client(srv, id, cx_feat_linked_editing(d, xl, xc));
				return;
			}

			/* 2. Native `.cx` interceptors with clangd fallback */
			if (strcmp(method, "textDocument/completion") == 0 && pos && id) {
				jval *res = cx_feat_completion(srv, d, xl, xc);
				if (res) {
					send_result_to_client(srv, id, res);
					return;
				}
			} else if (strcmp(method, "textDocument/hover") == 0 && pos && id) {
				jval *res = cx_feat_hover(srv, d, xl, xc);
				if (res) {
					send_result_to_client(srv, id, res);
					return;
				}
				rkind = REQ_HOVER_SANITIZE;
			} else if (strcmp(method, "textDocument/definition") == 0 && pos && id) {
				jval *res = cx_feat_definition(srv, d, xl, xc);
				if (res) {
					send_result_to_client(srv, id, res);
					return;
				}
				rkind = REQ_DEF_REMAP;
			} else if (strcmp(method, "textDocument/documentSymbol") == 0 && id) {
				jval *cx_syms = cx_feat_document_symbols(d);
				if (srv->no_clangd) {
					send_result_to_client(srv, id, cx_syms);
					return;
				}
				rkind = REQ_DOC_SYMBOL_MERGE;
				extra = cx_syms;
			} else if (strcmp(method, "textDocument/prepareRename") == 0 && pos && id) {
				jval *res = cx_feat_prepare_rename(srv, d, xl, xc);
				if (res) {
					send_result_to_client(srv, id, res);
					return;
				}
			} else if (strcmp(method, "textDocument/rename") == 0 && pos && id) {
				const char *new_name = jval_as_str(jval_get(params, "newName"));
				int merge_clangd = 0;
				jval *ws_edit = cx_feat_rename(srv, d, xl, xc, new_name, &merge_clangd);
				if (ws_edit && (!merge_clangd || srv->no_clangd)) {
					send_result_to_client(srv, id, ws_edit);
					return;
				}
				if (ws_edit && merge_clangd) {
					rkind = REQ_PROP_RENAME_MERGE;
					extra = ws_edit;
				} else {
					rkind = REQ_RENAME_FILTER;
				}
			} else if (strcmp(method, "textDocument/codeAction") == 0 && id) {
				const jval *orig_range = jval_get(params, "range");
				const jval *ctx = jval_get(params, "context");
				const jval *ctx_diags = ctx ? jval_get(ctx, "diagnostics") : NULL;
				jval *cx_acts = cx_feat_code_actions(srv, d, orig_range, ctx_diags);
				if (srv->no_clangd) {
					send_result_to_client(srv, id, cx_acts);
					return;
				}
				rkind = REQ_CODEACTION_MERGE;
				extra = cx_acts;
			}

			if (srv->no_clangd) {
				jval_free(extra);
				if (id)
					send_result_to_client(srv, id, jval_new_null());
				return;
			}

			/* Map cursor position `.cx -> .c` */
			if (pos && pos->kind == J_OBJ) {
				long long cl = 0, cc = 0;
				if (!map_cx_pos_to_c(d, xl, xc, &cl, &cc)) {
					if (id) {
						send_result_to_client(srv, id, extra ? extra : jval_new_null());
						extra = NULL;
					}
					jval_free(extra);
					return;
				}
				jval_set(pos, "line", jval_new_int(cl));
				jval_set(pos, "character", jval_new_int(cc));
			}

			/* Map request range `.cx -> .c` if present (e.g. codeAction) */
			jval *range = jval_get(params, "range");
			if (range && range->kind == J_OBJ) {
				if (!remap_range_obj_cx_to_c(d, range) && rkind == REQ_CODEACTION_MERGE) {
					if (id) {
						send_result_to_client(srv, id, extra ? extra : jval_new(J_ARR));
						extra = NULL;
					}
					jval_free(extra);
					return;
				}
			}

			/* Also map context diagnostic ranges for codeAction */
			if (rkind == REQ_CODEACTION_MERGE) {
				jval *ctx = jval_get(params, "context");
				jval *cd = ctx ? jval_get(ctx, "diagnostics") : NULL;
				if (cd && cd->kind == J_ARR) {
					for (size_t i = 0; i < cd->arr.len; i++)
						remap_range_obj_cx_to_c(d, jval_get(cd->arr.items[i], "range"));
				}
			}

			jval_set(td, "uri", jval_new_str(d->c_uri));
			if (id) {
				track_req_ex(srv, id, rkind, d->cx_uri, extra);
				extra = NULL;
			}
			jval_free(extra);
			send_lsp_json(srv->clangd_in, msg);
			return;
		}
	}

	/* Default pass-through (non-.cx files, workspace methods, $/cancelRequest, etc.) */
	if (srv->no_clangd) {
		if (id)
			send_result_to_client(srv, id, jval_new_null());
		return;
	}
	send_lsp_json(srv->clangd_in, msg);
}

static void handle_clangd_msg(lsp_server *srv, jval *msg)
{
	const char *method = jval_as_str(jval_get(msg, "method"));
	jval *id = jval_get(msg, "id");

	/* Server notification: publishDiagnostics */
	if (method && strcmp(method, "textDocument/publishDiagnostics") == 0) {
		jval *params = jval_get(msg, "params");
		const char *uri = params ? jval_as_str(jval_get(params, "uri")) : NULL;
		cx_doc *d = find_doc_by_c_uri(srv, uri);
		if (d) {
			jval_set(params, "uri", jval_new_str(d->cx_uri));
			jval *diags = jval_get(params, "diagnostics");
			if (!diags || diags->kind != J_ARR) {
				diags = jval_new(J_ARR);
				jval_set(params, "diagnostics", diags);
			}
			remap_json_c_to_cx(srv, diags, d);
			cx_feat_process_diags(srv, d, diags);
		}
		send_lsp_json(srv->client_out, msg);
		return;
	}

	/* Response to a tracked client request */
	if (!method && id) {
		req_kind kind = REQ_OTHER;
		char *cx_uri = NULL;
		jval *extra = NULL;
		if (pop_req(srv, id, &kind, &cx_uri, &extra)) {
			jval *res = jval_get(msg, "result");
			cx_doc *d = cx_uri ? find_doc_by_cx_uri(srv, cx_uri) : NULL;

			if (kind == REQ_INITIALIZE && res && res->kind == J_OBJ) {
				apply_cx_capabilities(jval_get(res, "capabilities"));
			} else if (kind == REQ_DOC_REMAP && res) {
				remap_json_c_to_cx(srv, res, d);
			} else if (kind == REQ_HOVER_SANITIZE && res) {
				remap_json_c_to_cx(srv, res, d);
				cx_feat_sanitize_hover(res);
			} else if (kind == REQ_DEF_REMAP && res) {
				remap_json_c_to_cx(srv, res, d);
				cx_feat_remap_def_result(srv, d, res);
			} else if (kind == REQ_DOC_SYMBOL_MERGE) {
				if (res)
					remap_json_c_to_cx(srv, res, d);
				jval *merged = cx_feat_merge_doc_symbols(d, extra, res);
				extra = NULL;
				jval_set(msg, "result", merged);
				jval_del(msg, "error");
			} else if (kind == REQ_RENAME_FILTER && res) {
				cx_feat_filter_clangd_rename(srv, res);
			} else if (kind == REQ_PROP_RENAME_MERGE) {
				if (res && res->kind == J_OBJ) {
					cx_feat_filter_clangd_rename(srv, res);
					cx_feat_merge_workspace_edits(extra, res);
				}
				jval_set(msg, "result", extra ? extra : jval_new_null());
				jval_del(msg, "error");
				extra = NULL;
			} else if (kind == REQ_CODEACTION_MERGE) {
				jval *merged = cx_feat_merge_code_actions(srv, extra, res);
				extra = NULL;
				jval_set(msg, "result", merged);
				jval_del(msg, "error");
			}

			jval_free(extra);
			free(cx_uri);
		}
		send_lsp_json(srv->client_out, msg);
		return;
	}

	/* Any other server notification/request */
	send_lsp_json(srv->client_out, msg);
}

/* ── Child `clangd` Spawn & Main Poll Loop ────────────────────────────── */

static int spawn_clangd(
        lsp_server *srv, const char *clangd_bin,
        char *const *extra_args, int extra_count)
{
	int to_child[2], from_child[2];
	if (pipe(to_child) != 0)
		return -1;
	if (pipe(from_child) != 0) {
		close(to_child[0]);
		close(to_child[1]);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(to_child[0]);
		close(to_child[1]);
		close(from_child[0]);
		close(from_child[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(to_child[0], STDIN_FILENO);
		dup2(from_child[1], STDOUT_FILENO);
		close(to_child[0]);
		close(to_child[1]);
		close(from_child[0]);
		close(from_child[1]);

		char **argv = (char **)calloc((size_t)(extra_count + 3), sizeof(char *));
		if (!argv)
			_exit(127);
		int k = 0;
		argv[k++] = (char *)clangd_bin;
		if (extra_count == 0)
			argv[k++] = (char *)"--log=error";
		for (int i = 0; i < extra_count; i++)
			argv[k++] = extra_args[i];
		argv[k] = NULL;

		execvp(clangd_bin, argv);
		_exit(127);
	}

	close(to_child[0]);
	close(from_child[1]);
	srv->clangd_in = to_child[1];
	srv->clangd_out = from_child[0];
	srv->clangd_pid = pid;
	return 0;
}

static void usage(FILE *fp)
{
	fprintf(fp,
	        "usage: cx-lsp [--clangd <path>] [--no-clangd] [-- <clangd args...>]\n"
	        "  Thin LSP stdio proxy over clangd for .cx files.\n");
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	const char *clangd_bin = getenv("CX_LSP_CLANGD");
	if (!clangd_bin || !clangd_bin[0])
		clangd_bin = "clangd";

	int no_clangd = 0;
	char **extra_args = (char **)calloc((size_t)argc + 1, sizeof(char *));
	int extra_count = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			free(extra_args);
			return 0;
		} else if (strcmp(argv[i], "--no-clangd") == 0) {
			no_clangd = 1;
		} else if (strcmp(argv[i], "--clangd") == 0 && i + 1 < argc) {
			clangd_bin = argv[++i];
		} else if (strcmp(argv[i], "--") == 0) {
			for (int j = i + 1; j < argc; j++)
				extra_args[extra_count++] = argv[j];
			break;
		} else {
			/* Pass through unrecognized flags directly to clangd (e.g. --compile-commands-dir=...) */
			extra_args[extra_count++] = argv[i];
		}
	}

	lsp_server srv;
	memset(&srv, 0, sizeof(srv));
	srv.client_in = STDIN_FILENO;
	srv.client_out = STDOUT_FILENO;
	srv.clangd_in = -1;
	srv.clangd_out = -1;
	srv.no_clangd = no_clangd;

	if (!no_clangd) {
		if (spawn_clangd(&srv, clangd_bin, extra_args, extra_count) != 0) {
			fprintf(stderr, "cx-lsp: failed to spawn '%s'\n", clangd_bin);
			free(extra_args);
			return 1;
		}
	}
	free(extra_args);

	lsp_stream c_in = { 0 };
	c_in.fd = srv.client_in;
	lsp_stream d_out = { 0 };
	d_out.fd = srv.clangd_out;

	while (srv.shutting_down < 2) {
		/* Drain any already-buffered messages first */
		int progressed = 0;
		char *body = NULL;
		size_t blen = 0;

		while (stream_pop_msg(&c_in, &body, &blen) == 1) {
			jval *msg = jval_parse(body, blen);
			free(body);
			if (msg) {
				handle_client_msg(&srv, msg);
				jval_free(msg);
			}
			progressed = 1;
			if (srv.shutting_down >= 2)
				break;
		}
		if (srv.shutting_down >= 2)
			break;

		if (!srv.no_clangd) {
			while (stream_pop_msg(&d_out, &body, &blen) == 1) {
				jval *msg = jval_parse(body, blen);
				free(body);
				if (msg) {
					handle_clangd_msg(&srv, msg);
					jval_free(msg);
				}
				progressed = 1;
			}
		}
		if (progressed)
			continue;

		if (c_in.eof)
			break;
		if (!srv.no_clangd && d_out.eof && srv.shutting_down)
			break;

		struct pollfd pfds[2];
		nfds_t nfds = 0;
		pfds[nfds].fd = c_in.fd;
		pfds[nfds].events = POLLIN;
		pfds[nfds].revents = 0;
		nfds++;

		if (!srv.no_clangd && d_out.fd >= 0 && !d_out.eof) {
			pfds[nfds].fd = d_out.fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		int pr = poll(pfds, nfds, -1);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
			stream_read_once(&c_in);
		if (nfds > 1 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)))
			stream_read_once(&d_out);
	}

	/* Clean shutdown */
	if (srv.clangd_in >= 0)
		close(srv.clangd_in);
	if (srv.clangd_out >= 0)
		close(srv.clangd_out);
	if (srv.clangd_pid > 0) {
		int status = 0;
		/* Give clangd a moment to exit on its own after stdin close, then reap */
		for (int t = 0; t < 20; t++) {
			pid_t w = waitpid(srv.clangd_pid, &status, WNOHANG);
			if (w == srv.clangd_pid) {
				srv.clangd_pid = 0;
				break;
			}
			struct pollfd dummy = { .fd = -1, .events = 0, .revents = 0 };
			poll(&dummy, 0, 25);
		}
		if (srv.clangd_pid > 0) {
			kill(srv.clangd_pid, SIGTERM);
			waitpid(srv.clangd_pid, &status, 0);
		}
	}

	stream_free(&c_in);
	stream_free(&d_out);
	for (size_t i = 0; i < srv.doc_count; i++)
		doc_free(srv.docs[i]);
	free(srv.docs);
	for (size_t i = 0; i < srv.req_count; i++) {
		free(srv.reqs[i].id_key);
		free(srv.reqs[i].cx_uri);
		jval_free(srv.reqs[i].extra);
	}
	free(srv.reqs);
	ws_index_free(&srv.ws);
	free(srv.root_path);
	return 0;
}

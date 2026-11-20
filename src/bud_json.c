#include "bud_internal.h"

/* ── JSON field extraction helpers (jsmn-backed, length-aware) ── */

#define JSMN_PARENT_LINKS
#define JSMN_STATIC
#include "jsmn.h"

#define BUD_JSON_MAX_TOKS 512

static int bud__jsoneq(const char *js, const jsmntok_t *tok, const char *s)
{
	size_t slen = strlen(s);
	return tok->type == JSMN_STRING &&
	       (size_t)(tok->end - tok->start) == slen &&
	       strncmp(js + tok->start, s, slen) == 0;
}

static int bud__hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Unescape a jsmn string slice (without outer quotes) into out. */
static void
bud__unescape_slice(const char *src, size_t slen, char *out, size_t out_sz)
{
	size_t i = 0;
	size_t o = 0;

	if (!out || out_sz == 0)
		return;
	out[0] = '\0';
	for (i = 0; i < slen && o + 1 < out_sz; i++) {
		char c = src[i];
		if (c == '\\' && i + 1 < slen) {
			char e = src[i + 1];
			switch (e) {
			case '"':
				out[o++] = '"';
				i++;
				break;
			case '\\':
				out[o++] = '\\';
				i++;
				break;
			case '/':
				out[o++] = '/';
				i++;
				break;
			case 'b':
				out[o++] = '\b';
				i++;
				break;
			case 'f':
				out[o++] = '\f';
				i++;
				break;
			case 'n':
				out[o++] = '\n';
				i++;
				break;
			case 'r':
				out[o++] = '\r';
				i++;
				break;
			case 't':
				out[o++] = '\t';
				i++;
				break;
			case 'u': {
				if (i + 5 < slen) {
					int h0 = bud__hex_val(src[i + 2]);
					int h1 = bud__hex_val(src[i + 3]);
					int h2 = bud__hex_val(src[i + 4]);
					int h3 = bud__hex_val(src[i + 5]);
					if (h0 >= 0 && h1 >= 0 && h2 >= 0 &&
					    h3 >= 0)
					{
						unsigned cp =
						        (unsigned)((h0 << 12) |
						                   (h1 << 8) |
						                   (h2 << 4) |
						                   h3);
						if (cp < 0x80) {
							out[o++] = (char)cp;
						} else if (
						        cp < 0x800 &&
						        o + 2 < out_sz)
						{
							out[o++] = (char)(0xC0 |
							                  (cp >>
							                   6));
							out[o++] =
							        (char)(0x80 |
							               (cp &
							                0x3F));
						} else if (o + 3 < out_sz) {
							out[o++] = (char)(0xE0 |
							                  (cp >>
							                   12));
							out[o++] =
							        (char)(0x80 |
							               ((cp >>
							                 6) &
							                0x3F));
							out[o++] =
							        (char)(0x80 |
							               (cp &
							                0x3F));
						}
						i += 5;
						break;
					}
				}
				out[o++] = '\\';
				break;
			}
			default:
				out[o++] = e;
				i++;
				break;
			}
		} else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
}

static int bud__parse_tokens(
        const char *js, size_t len, jsmntok_t *stack_buf, unsigned stack_n,
        jsmntok_t **out_toks, unsigned *out_ntoks, jsmntok_t **heap_toks)
{
	jsmn_parser p;
	int r;

	*heap_toks = NULL;
	jsmn_init(&p);
	r = jsmn_parse(&p, js, len, stack_buf, stack_n);
	if (r != JSMN_ERROR_NOMEM) {
		*out_toks = stack_buf;
		*out_ntoks = r >= 0 ? (unsigned)r : 0;
		return r;
	}
	{
		unsigned cap = stack_n * 2;
		while (cap <= 65536) {
			jsmntok_t *heap =
			        (jsmntok_t *)malloc(cap * sizeof(jsmntok_t));
			if (!heap)
				return JSMN_ERROR_NOMEM;
			jsmn_init(&p);
			r = jsmn_parse(&p, js, len, heap, cap);
			if (r != JSMN_ERROR_NOMEM) {
				*out_toks = heap;
				*out_ntoks = r >= 0 ? (unsigned)r : 0;
				*heap_toks = heap;
				return r;
			}
			free(heap);
			cap *= 2;
		}
	}
	return JSMN_ERROR_NOMEM;
}

static int bud__find_key(
        const char *js, jsmntok_t *toks, unsigned ntoks, const char *key,
        int *val_idx)
{
	unsigned i;

	if (ntoks == 0 || toks[0].type != JSMN_OBJECT)
		return 0;
	i = 1;
	while (i + 1 < ntoks) {
		jsmntok_t *kt = &toks[i];
		jsmntok_t *vt;
		unsigned next;

		if (kt->type != JSMN_STRING) {
			i++;
			continue;
		}
		vt = &toks[i + 1];
		if (bud__jsoneq(js, kt, key)) {
			*val_idx = (int)(i + 1);
			return 1;
		}
		next = i + 2;
		while (next < ntoks && toks[next].start != -1 &&
		       toks[next].start < vt->end)
			next++;
		i = next;
	}
	return 0;
}

void bud_json_str_len(
        const char *json, size_t len, const char *key, char *out,
        size_t out_size)
{
	jsmntok_t stack[BUD_JSON_MAX_TOKS];
	jsmntok_t *toks = NULL;
	jsmntok_t *heap = NULL;
	unsigned ntoks = 0;
	int r;
	int vidx;

	if (!out || out_size == 0)
		return;
	out[0] = '\0';
	if (!json || !key)
		return;
	if (len == 0)
		len = strlen(json);
	if (len == 0)
		return;
	r = bud__parse_tokens(
	        json, len, stack, BUD_JSON_MAX_TOKS, &toks, &ntoks, &heap);
	if (r < 0)
		goto out;
	if (!bud__find_key(json, toks, ntoks, key, &vidx))
		goto out;
	{
		jsmntok_t *vt = &toks[vidx];
		if (vt->type == JSMN_STRING) {
			bud__unescape_slice(
			        json + vt->start, (size_t)(vt->end - vt->start),
			        out, out_size);
		} else if (vt->type == JSMN_PRIMITIVE) {
			size_t slen = (size_t)(vt->end - vt->start);
			size_t n = slen < out_size - 1 ? slen : out_size - 1;
			memcpy(out, json + vt->start, n);
			out[n] = '\0';
		} else if (vt->type == JSMN_OBJECT || vt->type == JSMN_ARRAY) {
			size_t slen = (size_t)(vt->end - vt->start);
			size_t n = slen < out_size - 1 ? slen : out_size - 1;
			memcpy(out, json + vt->start, n);
			out[n] = '\0';
		}
	}
out:
	if (heap)
		free(heap);
}

void bud_json_str(const char *json, const char *key, char *out, size_t out_size)
{
	size_t len = json ? strlen(json) : 0;
	bud_json_str_len(json, len, key, out, out_size);
}

int bud_json_int_len(
        const char *json, size_t len, const char *key, int default_val)
{
	jsmntok_t stack[BUD_JSON_MAX_TOKS];
	jsmntok_t *toks = NULL;
	jsmntok_t *heap = NULL;
	unsigned ntoks = 0;
	int r;
	int vidx;

	if (!json || !key)
		return default_val;
	if (len == 0)
		len = strlen(json);
	if (len == 0)
		return default_val;
	r = bud__parse_tokens(
	        json, len, stack, BUD_JSON_MAX_TOKS, &toks, &ntoks, &heap);
	if (r < 0) {
		if (heap)
			free(heap);
		return default_val;
	}
	if (!bud__find_key(json, toks, ntoks, key, &vidx)) {
		if (heap)
			free(heap);
		return default_val;
	}
	{
		jsmntok_t *vt = &toks[vidx];
		const char *s = json + vt->start;
		size_t slen = (size_t)(vt->end - vt->start);
		char buf[64];
		size_t n = slen < sizeof(buf) - 1 ? slen : sizeof(buf) - 1;
		char *end = NULL;
		long v;
		memcpy(buf, s, n);
		buf[n] = '\0';
		if (vt->type == JSMN_STRING) {
			char *p = buf;
			while (*p && (*p < '0' || *p > '9') && *p != '-')
				p++;
			if (!*p) {
				if (heap)
					free(heap);
				return default_val;
			}
			v = strtol(p, &end, 10);
		} else {
			v = strtol(buf, &end, 10);
			if (end == buf) {
				if (heap)
					free(heap);
				return default_val;
			}
		}
		if (heap)
			free(heap);
		return (int)v;
	}
}

int bud_json_int(const char *json, const char *key, int default_val)
{
	size_t len = json ? strlen(json) : 0;
	return bud_json_int_len(json, len, key, default_val);
}

void bud_json_data_len(const char *json, size_t len, char *out, size_t out_size)
{
	bud_json_str_len(json, len, "data", out, out_size);
}

void bud_json_data(const char *json, char *out, size_t out_size)
{
	size_t len = json ? strlen(json) : 0;
	bud_json_data_len(json, len, out, out_size);
}

int bud_json_array_for_each_len(
        const char *json, size_t len,
        void (*fn)(const char *elem, size_t elen, void *user), void *user)
{
	jsmntok_t stack[BUD_JSON_MAX_TOKS];
	jsmntok_t *toks = NULL;
	jsmntok_t *heap = NULL;
	unsigned ntoks = 0;
	int r;
	unsigned idx;
	int count = 0;

	if (!json || !fn)
		return -1;
	if (len == 0)
		len = strlen(json);
	r = bud__parse_tokens(
	        json, len, stack, BUD_JSON_MAX_TOKS, &toks, &ntoks, &heap);
	if (r < 0) {
		if (heap)
			free(heap);
		return -1;
	}
	if (ntoks == 0 || toks[0].type != JSMN_ARRAY) {
		if (heap)
			free(heap);
		return -1;
	}
	idx = 1;
	for (int i = 0; i < toks[0].size; i++) {
		unsigned next;
		const char *elem;
		size_t elen;

		if (idx >= ntoks)
			break;
		elem = json + toks[idx].start;
		elen = (size_t)(toks[idx].end - toks[idx].start);
		if (toks[idx].type == JSMN_STRING) {
			if (toks[idx].start > 0 &&
			    json[toks[idx].start - 1] == '"' &&
			    (size_t)toks[idx].end < len &&
			    json[toks[idx].end] == '"')
			{
				elem = json + toks[idx].start - 1;
				elen += 2;
			}
		} else if (
		        toks[idx].type == JSMN_OBJECT ||
		        toks[idx].type == JSMN_ARRAY)
		{
			elem = json + toks[idx].start;
			elen = (size_t)(toks[idx].end - toks[idx].start);
		}
		fn(elem, elen, user);
		count++;
		next = idx + 1;
		while (next < ntoks && toks[next].start != -1 &&
		       toks[next].start < toks[idx].end)
			next++;
		idx = next;
	}
	if (heap)
		free(heap);
	return count;
}

int bud_json_array_for_each(
        const char *json, void (*fn)(const char *elem, size_t len, void *user),
        void *user)
{
	size_t len = json ? strlen(json) : 0;
	return bud_json_array_for_each_len(json, len, fn, user);
}

int bud_json_array_for_each_key_len(
        const char *json, size_t len, const char *key,
        void (*fn)(const char *elem, size_t elen, void *user), void *user)
{
	jsmntok_t stack[BUD_JSON_MAX_TOKS];
	jsmntok_t *toks = NULL;
	jsmntok_t *heap = NULL;
	unsigned ntoks = 0;
	int r;
	int vidx;
	unsigned idx;
	int n;

	if (!json || !key || !fn)
		return -1;
	if (len == 0)
		len = strlen(json);
	r = bud__parse_tokens(
	        json, len, stack, BUD_JSON_MAX_TOKS, &toks, &ntoks, &heap);
	if (r < 0) {
		if (heap)
			free(heap);
		return -1;
	}
	if (!bud__find_key(json, toks, ntoks, key, &vidx)) {
		if (heap)
			free(heap);
		return -1;
	}
	if (toks[vidx].type != JSMN_ARRAY) {
		if (heap)
			free(heap);
		return -1;
	}
	n = toks[vidx].size;
	idx = (unsigned)(vidx + 1);
	for (int i = 0; i < n; i++) {
		unsigned next;
		const char *elem;
		size_t elen;

		if (idx >= ntoks)
			break;
		elem = json + toks[idx].start;
		elen = (size_t)(toks[idx].end - toks[idx].start);
		if (toks[idx].type == JSMN_STRING) {
			if (toks[idx].start > 0 &&
			    json[toks[idx].start - 1] == '"' &&
			    (size_t)toks[idx].end < len &&
			    json[toks[idx].end] == '"')
			{
				elem = json + toks[idx].start - 1;
				elen += 2;
			}
		} else if (
		        toks[idx].type == JSMN_OBJECT ||
		        toks[idx].type == JSMN_ARRAY)
		{
			elem = json + toks[idx].start;
			elen = (size_t)(toks[idx].end - toks[idx].start);
		}
		fn(elem, elen, user);
		next = idx + 1;
		while (next < ntoks && toks[next].start != -1 &&
		       toks[next].start < toks[idx].end)
			next++;
		idx = next;
	}
	if (heap)
		free(heap);
	return n;
}

/* ── Table-driven state ── */

void bud_state_apply_stride_len(
        void *state, const void *fields, size_t field_stride, const char *json,
        size_t len)
{
	jsmntok_t stack[BUD_JSON_MAX_TOKS];
	jsmntok_t *toks = NULL;
	jsmntok_t *heap = NULL;
	unsigned ntoks = 0;
	int r;

	if (!state || !fields || !json)
		return;
	if (field_stride < sizeof(bud_field_desc_t))
		field_stride = sizeof(bud_field_desc_t);
	if ((ssize_t)len <= 0)
		len = strlen(json);
	if (len == 0)
		return;
	r = bud__parse_tokens(
	        json, len, stack, BUD_JSON_MAX_TOKS, &toks, &ntoks, &heap);
	if (r < 0) {
		if (heap)
			free(heap);
		return;
	}
	if (ntoks == 0 || toks[0].type != JSMN_OBJECT) {
		if (heap)
			free(heap);
		return;
	}
	for (const char *p = (const char *)fields;; p += field_stride) {
		const bud_field_desc_t *f = (const bud_field_desc_t *)p;
		if (!f->key)
			break;
		int vidx;
		if (f->kind == 1 || f->kind == 5)
			continue;
		if (!bud__find_key(json, toks, ntoks, f->key, &vidx))
			continue;
		if (f->is_int) {
			int *dest = (int *)((char *)state + f->offset);
			jsmntok_t *vt = &toks[vidx];
			const char *s = json + vt->start;
			size_t slen = (size_t)(vt->end - vt->start);
			char buf[64];
			size_t n =
			        slen < sizeof(buf) - 1 ? slen : sizeof(buf) - 1;
			char *end = NULL;
			long v;
			memcpy(buf, s, n);
			buf[n] = '\0';
			if (vt->type == JSMN_STRING) {
				char *pstr = buf;
				while (*pstr && (*pstr < '0' || *pstr > '9') &&
				       *pstr != '-')
					pstr++;
				if (!*pstr)
					continue;
				v = strtol(pstr, &end, 10);
			} else {
				v = strtol(buf, &end, 10);
				if (end == buf)
					continue;
			}
			*dest = (int)v;
		} else if (f->size > 0) {
			char *dest = (char *)state + f->offset;
			jsmntok_t *vt = &toks[vidx];
			if (vt->type == JSMN_STRING) {
				bud__unescape_slice(
				        json + vt->start,
				        (size_t)(vt->end - vt->start), dest,
				        f->size);
			} else if (vt->type == JSMN_PRIMITIVE) {
				size_t slen = (size_t)(vt->end - vt->start);
				size_t n =
				        slen < f->size - 1 ? slen : f->size - 1;
				memcpy(dest, json + vt->start, n);
				dest[n] = '\0';
			} else if (
			        vt->type == JSMN_OBJECT ||
			        vt->type == JSMN_ARRAY)
			{
				size_t slen = (size_t)(vt->end - vt->start);
				size_t n =
				        slen < f->size - 1 ? slen : f->size - 1;
				memcpy(dest, json + vt->start, n);
				dest[n] = '\0';
			}
		}
	}
	if (heap)
		free(heap);
}

void bud_state_apply_stride(
        void *state, const void *fields, size_t field_stride, const char *json)
{
	size_t len = json ? strlen(json) : 0;
	bud_state_apply_stride_len(state, fields, field_stride, json, len);
}

void bud_state_apply_len(
        void *state, const bud_field_desc_t *fields, const char *json,
        size_t len)
{
	bud_state_apply_stride_len(
	        state, fields, sizeof(bud_field_desc_t), json, len);
}

void bud_state_apply(
        void *state, const bud_field_desc_t *fields, const char *json)
{
	size_t len = json ? strlen(json) : 0;
	bud_state_apply_len(state, fields, json, len);
}

struct apply_array_ctx {
	char *array_out;
	size_t elem_size;
	int count;
	int max_elems;
	const void *schema;
	size_t schema_stride;
};

static void apply_array_cb(const char *elem, size_t len, void *user)
{
	struct apply_array_ctx *ctx = (struct apply_array_ctx *)user;
	char *dest;

	if (ctx->count >= ctx->max_elems)
		return;
	dest = ctx->array_out + (ctx->count * ctx->elem_size);
	memset(dest, 0, ctx->elem_size);
	bud_state_apply_stride_len(
	        dest, ctx->schema, ctx->schema_stride, elem, len);
	ctx->count++;
}

void bud_state_apply_array_stride_len(
        const char *json, size_t len, const char *key, void *array_out,
        size_t elem_size, int *count_out, int max_elems, const void *schema,
        size_t schema_stride)
{
	jsmntok_t stack[BUD_JSON_MAX_TOKS];
	jsmntok_t *toks = NULL;
	jsmntok_t *heap = NULL;
	unsigned ntoks = 0;
	int r;
	int vidx;
	struct apply_array_ctx ctx;

	if (!json || !key || !array_out || !schema)
		return;
	if (schema_stride < sizeof(bud_field_desc_t))
		schema_stride = sizeof(bud_field_desc_t);
	if ((ssize_t)len <= 0)
		len = strlen(json);
	ctx.array_out = (char *)array_out;
	ctx.elem_size = elem_size;
	ctx.count = 0;
	ctx.max_elems = max_elems;
	ctx.schema = schema;
	ctx.schema_stride = schema_stride;
	r = bud__parse_tokens(
	        json, len, stack, BUD_JSON_MAX_TOKS, &toks, &ntoks, &heap);
	if (r < 0)
		goto done;
	if (!bud__find_key(json, toks, ntoks, key, &vidx))
		goto done;
	{
		jsmntok_t *vt = &toks[vidx];
		const char *arr_json;
		size_t arr_len;
		unsigned idx;
		int n;

		if (vt->type != JSMN_ARRAY)
			goto done;
		arr_json = json + vt->start;
		arr_len = (size_t)(vt->end - vt->start);
		n = vt->size;
		idx = (unsigned)(vidx + 1);
		for (int i = 0; i < n; i++) {
			unsigned next;
			const char *elem;
			size_t elen;

			if (idx >= ntoks)
				break;
			elem = json + toks[idx].start;
			elen = (size_t)(toks[idx].end - toks[idx].start);
			if (toks[idx].type == JSMN_OBJECT ||
			    toks[idx].type == JSMN_ARRAY)
			{
				elem = json + toks[idx].start;
				elen = (size_t)(toks[idx].end -
				                toks[idx].start);
			} else if (toks[idx].type == JSMN_STRING) {
				if (toks[idx].start > 0 &&
				    json[toks[idx].start - 1] == '"' &&
				    (size_t)toks[idx].end < len &&
				    json[toks[idx].end] == '"')
				{
					elem = json + toks[idx].start - 1;
					elen += 2;
				}
			}
			(void)arr_json;
			(void)arr_len;
			apply_array_cb(elem, elen, &ctx);
			next = idx + 1;
			while (next < ntoks && toks[next].start != -1 &&
			       toks[next].start < toks[idx].end)
				next++;
			idx = next;
		}
	}
done:
	if (heap)
		free(heap);
	if (count_out)
		*count_out = ctx.count;
}

void bud_state_apply_array_len(
        const char *json, size_t len, const char *key, void *array_out,
        size_t elem_size, int *count_out, int max_elems,
        const bud_field_desc_t *schema)
{
	bud_state_apply_array_stride_len(
	        json, len, key, array_out, elem_size, count_out, max_elems,
	        schema, sizeof(bud_field_desc_t));
}

void bud_state_apply_array(
        const char *json, const char *key, void *array_out, size_t elem_size,
        int *count_out, int max_elems, const bud_field_desc_t *schema)
{
	size_t len = json ? strlen(json) : 0;
	bud_state_apply_array_len(
	        json, len, key, array_out, elem_size, count_out, max_elems,
	        schema);
}

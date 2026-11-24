/**
 * @file cx_transpile.c
 * @brief Single-pass pure-C `.cx` to C11 transpiler targeting `bud_reconcile.h`.
 *
 * Architecture:
 *  - Two-buffer top-level model (`hoist` + `body`) flushed to `out` at
 *    brace_depth == 0 statement/function/directive boundaries.
 *  - Zero C typechecker: relies on C11 `_Generic` (`bud_child_any`,
 *    `bud_set_attr_any`, `bud_set_key_any`) and explicit/component-param
 *    typed closure capture lists.
 *  - Emits `#line` directives and exact `cx_span` source maps for `cx-lsp`.
 */

#include <bud/cx.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Dynamic String Buffer ────────────────────────────────────────────── */

typedef struct {
	char  *data;
	size_t len;
	size_t cap;
	int    oom;
} cx_buf;

static void buf_free(cx_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static void buf_reset(cx_buf *b)
{
	b->len = 0;
	if (b->data)
		b->data[0] = '\0';
}

static int buf_reserve(cx_buf *b, size_t extra)
{
	if (b->oom)
		return -1;
	size_t need = b->len + extra + 1;
	if (need <= b->cap)
		return 0;
	size_t ncap = b->cap ? b->cap * 2 : 256;
	while (ncap < need)
		ncap *= 2;
	char *p = (char *)realloc(b->data, ncap);
	if (!p) {
		b->oom = 1;
		return -1;
	}
	b->data = p;
	b->cap = ncap;
	return 0;
}

static void buf_append(cx_buf *b, const char *s, size_t n)
{
	if (n == 0 || buf_reserve(b, n) != 0)
		return;
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void buf_str(cx_buf *b, const char *s)
{
	if (s)
		buf_append(b, s, strlen(s));
}

static void buf_ch(cx_buf *b, char c)
{
	if (buf_reserve(b, 1) != 0)
		return;
	b->data[b->len++] = c;
	b->data[b->len] = '\0';
}

static void buf_printf(cx_buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int need = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (need <= 0) {
		va_end(ap2);
		return;
	}
	if (buf_reserve(b, (size_t)need) != 0) {
		va_end(ap2);
		return;
	}
	vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
	va_end(ap2);
	b->len += (size_t)need;
}

static void buf_escaped_cstr(cx_buf *b, const char *s, size_t n)
{
	buf_ch(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '\\': buf_str(b, "\\\\"); break;
		case '"':  buf_str(b, "\\\""); break;
		case '\n': buf_str(b, "\\n");  break;
		case '\r': buf_str(b, "\\r");  break;
		case '\t': buf_str(b, "\\t");  break;
		default:
			if (c < 0x20)
				buf_printf(b, "\\x%02x", (unsigned)c);
			else
				buf_ch(b, (char)c);
			break;
		}
	}
	buf_ch(b, '"');
}

/* ── Span & Diagnostic Lists ──────────────────────────────────────────── */

typedef struct {
	cx_span *items;
	size_t   count;
	size_t   cap;
	int      oom;
} cx_span_list;

static void span_list_free(cx_span_list *l)
{
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->cap = 0;
}

static void span_push(
        cx_span_list *l, uint32_t cx_off, uint32_t cx_len,
        uint32_t c_off, uint32_t c_len, uint8_t kind)
{
	if (l->oom || cx_len == 0)
		return;
	if (l->count >= l->cap) {
		size_t ncap = l->cap ? l->cap * 2 : 64;
		cx_span *p = (cx_span *)realloc(l->items, ncap * sizeof(*p));
		if (!p) {
			l->oom = 1;
			return;
		}
		l->items = p;
		l->cap = ncap;
	}
	cx_span *sp = &l->items[l->count++];
	sp->cx_off = cx_off;
	sp->cx_len = cx_len;
	sp->c_off = c_off;
	sp->c_len = c_len;
	sp->kind = kind;
	sp->_pad[0] = sp->_pad[1] = sp->_pad[2] = 0;
}

typedef struct {
	cx_diag *items;
	size_t   count;
	size_t   cap;
	int      oom;
} cx_diag_list;

typedef struct {
	char    *decl;
	char    *name;
	uint32_t decl_off;
	uint32_t decl_len;
} cx_param;

/* ── Parser State ─────────────────────────────────────────────────────── */

typedef struct {
	const char *src;
	size_t      len;
	size_t      pos;
	uint32_t    line;
	uint32_t    col;

	const char *filename;
	int         emit_lines;
	int         recover;

	cx_buf       out;
	cx_buf       hoist;
	cx_buf       body;
	cx_span_list out_spans;
	cx_span_list hoist_spans;
	cx_span_list body_spans;
	cx_diag_list diags;

	int      brace_depth;
	uint32_t next_node_id;
	uint32_t next_closure_id;

	/* Parameters of currently enclosing `component Foo(...)` for shorthand captures */
	cx_param *comp_params;
	size_t    comp_param_count;
	int       comp_brace_depth;
} cx_state;

/* ── Lexer Primitives ─────────────────────────────────────────────────── */

static int is_ident_start(char c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static int is_attr_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':';
}

static char peek_at(const cx_state *st, size_t off)
{
	return (st->pos + off < st->len) ? st->src[st->pos + off] : '\0';
}

static char advance(cx_state *st)
{
	if (st->pos >= st->len)
		return '\0';
	char c = st->src[st->pos++];
	if (c == '\n') {
		st->line++;
		st->col = 1;
	} else {
		st->col++;
	}
	return c;
}

static void add_diag(cx_state *st, uint32_t line, uint32_t col, uint32_t off, const char *fmt, ...)
{
	if (st->diags.oom)
		return;
	if (st->diags.count >= st->diags.cap) {
		size_t ncap = st->diags.cap ? st->diags.cap * 2 : 8;
		cx_diag *p = (cx_diag *)realloc(st->diags.items, ncap * sizeof(*p));
		if (!p) {
			st->diags.oom = 1;
			return;
		}
		st->diags.items = p;
		st->diags.cap = ncap;
	}
	cx_diag *d = &st->diags.items[st->diags.count++];
	d->line = line;
	d->col = col;
	d->off = off;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(d->msg, sizeof(d->msg), fmt, ap);
	va_end(ap);
}

static void emit_line_directive(cx_state *st, cx_buf *dst, uint32_t line)
{
	if (!st->emit_lines || !st->filename)
		return;
	if (dst->len > 0 && dst->data[dst->len - 1] != '\n')
		buf_ch(dst, '\n');
	buf_str(dst, "#line ");
	buf_printf(dst, "%u ", (unsigned)line);
	buf_escaped_cstr(dst, st->filename, strlen(st->filename));
	buf_ch(dst, '\n');
}

static void clear_comp_params(cx_state *st)
{
	for (size_t i = 0; i < st->comp_param_count; i++) {
		free(st->comp_params[i].decl);
		free(st->comp_params[i].name);
	}
	free(st->comp_params);
	st->comp_params = NULL;
	st->comp_param_count = 0;
	st->comp_brace_depth = -1;
}

static void flush_top_level(cx_state *st)
{
	if (st->hoist.len > 0) {
		if (st->out.len > 0 && st->out.data[st->out.len - 1] != '\n' &&
		    st->hoist.data[0] == '#')
		{
			buf_ch(&st->out, '\n');
		}
		uint32_t base = (uint32_t)st->out.len;
		buf_append(&st->out, st->hoist.data, st->hoist.len);
		for (size_t i = 0; i < st->hoist_spans.count; i++) {
			cx_span sp = st->hoist_spans.items[i];
			span_push(&st->out_spans, sp.cx_off, sp.cx_len,
			          sp.c_off + base, sp.c_len, sp.kind);
		}
		buf_reset(&st->hoist);
		st->hoist_spans.count = 0;
	}
	if (st->body.len > 0) {
		if (st->out.len > 0 && st->out.data[st->out.len - 1] != '\n' &&
		    st->body.data[0] == '#')
		{
			buf_ch(&st->out, '\n');
		}
		uint32_t base = (uint32_t)st->out.len;
		buf_append(&st->out, st->body.data, st->body.len);
		for (size_t i = 0; i < st->body_spans.count; i++) {
			cx_span sp = st->body_spans.items[i];
			span_push(&st->out_spans, sp.cx_off, sp.cx_len,
			          sp.c_off + base, sp.c_len, sp.kind);
		}
		buf_reset(&st->body);
		st->body_spans.count = 0;
	}
}

/* ── String / Comment / Preprocessor Verbatim Scanners ────────────────── */

static void scan_string(cx_state *st, cx_buf *dst, char quote)
{
	buf_ch(dst, advance(st));
	while (st->pos < st->len) {
		char c = advance(st);
		buf_ch(dst, c);
		if (c == '\\' && st->pos < st->len) {
			buf_ch(dst, advance(st));
		} else if (c == quote) {
			return;
		} else if (c == '\n') {
			return;
		}
	}
}

static void scan_line_comment(cx_state *st, cx_buf *dst)
{
	while (st->pos < st->len) {
		char c = advance(st);
		buf_ch(dst, c);
		if (c == '\n')
			return;
	}
}

static void scan_block_comment(cx_state *st, cx_buf *dst)
{
	buf_ch(dst, advance(st)); /* / */
	buf_ch(dst, advance(st)); /* * */
	while (st->pos < st->len) {
		char c = advance(st);
		buf_ch(dst, c);
		if (c == '*' && peek_at(st, 0) == '/') {
			buf_ch(dst, advance(st));
			return;
		}
	}
}

static void skip_ws_and_comments(cx_state *st)
{
	while (st->pos < st->len) {
		char c = peek_at(st, 0);
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			advance(st);
		} else if (c == '/' && peek_at(st, 1) == '/') {
			while (st->pos < st->len && advance(st) != '\n')
				;
		} else if (c == '/' && peek_at(st, 1) == '*') {
			advance(st);
			advance(st);
			while (st->pos < st->len) {
				if (advance(st) == '*' && peek_at(st, 0) == '/') {
					advance(st);
					break;
				}
			}
		} else {
			break;
		}
	}
}

static void scan_preprocessor_line(cx_state *st, cx_buf *dst, cx_span_list *spans)
{
	uint32_t cx_start = (uint32_t)st->pos;
	uint32_t c_start = (uint32_t)dst->len;
	while (st->pos < st->len) {
		char c = advance(st);
		buf_ch(dst, c);
		if (c == '\\' && peek_at(st, 0) == '\n') {
			buf_ch(dst, advance(st));
		} else if (c == '\\' && peek_at(st, 0) == '\r' && peek_at(st, 1) == '\n') {
			buf_ch(dst, advance(st));
			buf_ch(dst, advance(st));
		} else if (c == '\n') {
			break;
		}
	}
	span_push(spans, cx_start, (uint32_t)(st->pos - cx_start),
	          c_start, (uint32_t)(dst->len - c_start), CX_SPAN_PASSTHROUGH);
}

/* ── Parameter & Declarator Helpers ───────────────────────────────────── */

static char *cx_strndup(const char *s, size_t n)
{
	char *p = (char *)malloc(n + 1);
	if (!p)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static void trim_slice(const char **s, size_t *n)
{
	while (*n > 0 && isspace((unsigned char)(*s)[0])) {
		(*s)++;
		(*n)--;
	}
	while (*n > 0 && isspace((unsigned char)(*s)[*n - 1])) {
		(*n)--;
	}
}

/* Extract identifier name from a C declarator like `const char *const *songs`,
 * `int arr[4]`, or `void (*cb)(int)`. Returns heap string or NULL. */
static char *extract_decl_name(const char *s, size_t n)
{
	trim_slice(&s, &n);
	if (n == 0)
		return NULL;

	/* Check for function pointer declarator `(*name)(...)` */
	for (size_t i = 0; i + 2 < n; i++) {
		if (s[i] == '(') {
			size_t j = i + 1;
			while (j < n && isspace((unsigned char)s[j]))
				j++;
			if (j < n && s[j] == '*') {
				j++;
				while (j < n && (s[j] == '*' || isspace((unsigned char)s[j])))
					j++;
				/* Skip qualifiers like const/restrict inside (*const name) */
				const char *last_id = NULL;
				size_t last_len = 0;
				while (j < n && s[j] != ')') {
					if (is_ident_start(s[j])) {
						const char *id = s + j;
						while (j < n && is_ident_char(s[j]))
							j++;
						last_id = id;
						last_len = (size_t)((s + j) - id);
					} else {
						j++;
					}
				}
				if (last_id && last_len > 0)
					return cx_strndup(last_id, last_len);
			}
		}
	}

	/* Strip trailing array dimensions `[...]` */
	size_t end = n;
	while (end > 0) {
		while (end > 0 && isspace((unsigned char)s[end - 1]))
			end--;
		if (end > 0 && s[end - 1] == ']') {
			int depth = 1;
			end--;
			while (end > 0 && depth > 0) {
				if (s[end - 1] == ']')
					depth++;
				else if (s[end - 1] == '[')
					depth--;
				end--;
			}
		} else {
			break;
		}
	}

	/* Walk backwards over final identifier */
	size_t id_end = end;
	while (end > 0 && is_ident_char(s[end - 1]))
		end--;
	if (end < id_end && is_ident_start(s[end]))
		return cx_strndup(s + end, id_end - end);

	return NULL;
}

/* Parse comma-separated parameter list slice into cx_param array. */
static size_t parse_param_slice(
        const char *base_src, const char *s, size_t n, cx_param **out_params)
{
	*out_params = NULL;
	trim_slice(&s, &n);
	if (n == 0 || (n == 4 && memcmp(s, "void", 4) == 0))
		return 0;

	size_t cap = 8, count = 0;
	cx_param *arr = (cx_param *)calloc(cap, sizeof(*arr));
	if (!arr)
		return 0;

	size_t start = 0;
	int pdepth = 0, bdepth = 0, cdepth = 0;
	for (size_t i = 0; i <= n; i++) {
		char c = (i < n) ? s[i] : ',';
		if (c == '(') pdepth++;
		else if (c == ')' && pdepth > 0) pdepth--;
		else if (c == '[') bdepth++;
		else if (c == ']' && bdepth > 0) bdepth--;
		else if (c == '{') cdepth++;
		else if (c == '}' && cdepth > 0) cdepth--;

		if (c == ',' && pdepth == 0 && bdepth == 0 && cdepth == 0) {
			const char *seg = s + start;
			size_t slen = i - start;
			trim_slice(&seg, &slen);
			if (slen > 0) {
				char *name = extract_decl_name(seg, slen);
				if (name) {
					if (count >= cap) {
						cap *= 2;
						cx_param *np = (cx_param *)realloc(arr, cap * sizeof(*np));
						if (!np) {
							free(name);
							break;
						}
						arr = np;
					}
					arr[count].decl = cx_strndup(seg, slen);
					arr[count].name = name;
					arr[count].decl_off = base_src ? (uint32_t)(seg - base_src) : 0;
					arr[count].decl_len = (uint32_t)slen;
					count++;
				}
			}
			start = i + 1;
		}
	}

	*out_params = arr;
	return count;
}

static const char *lookup_comp_param_decl(const cx_state *st, const char *name, size_t nlen)
{
	for (size_t i = 0; i < st->comp_param_count; i++) {
		if (strlen(st->comp_params[i].name) == nlen &&
		    memcmp(st->comp_params[i].name, name, nlen) == 0)
		{
			return st->comp_params[i].decl;
		}
	}
	return NULL;
}

/* Forward declarations for mutual recursion between C exprs and JSX */
static void parse_jsx_expr(cx_state *st, cx_buf *dst, cx_span_list *spans);
static void transpile_c_until(
        cx_state *st, cx_buf *dst, cx_span_list *spans,
        char stop_ch, int initial_allow_jsx, uint8_t passthrough_kind);

/* ── Portable C99 Closure Lifting ─────────────────────────────────────── */

typedef struct {
	char *decl;
	char *name;
	char *init_expr;
} cx_capture;

/* Scan balanced delimiter (`(...)`, `[...]`, `{...}`) returning slice inside. */
static int scan_balanced_raw(
        cx_state *st, char open_ch, char close_ch,
        size_t *out_inner_start, size_t *out_inner_end)
{
	uint32_t start_line = st->line, start_col = st->col;
	uint32_t start_off = (uint32_t)st->pos;
	if (peek_at(st, 0) != open_ch)
		return 0;
	advance(st);
	*out_inner_start = st->pos;
	int depth = 1;
	while (st->pos < st->len && depth > 0) {
		char c = peek_at(st, 0);
		if (c == '"' || c == '\'') {
			char q = advance(st);
			while (st->pos < st->len) {
				char sc = advance(st);
				if (sc == '\\' && st->pos < st->len)
					advance(st);
				else if (sc == q || sc == '\n')
					break;
			}
		} else if (c == '/' && peek_at(st, 1) == '/') {
			while (st->pos < st->len && advance(st) != '\n')
				;
		} else if (c == '/' && peek_at(st, 1) == '*') {
			advance(st);
			advance(st);
			while (st->pos < st->len) {
				if (advance(st) == '*' && peek_at(st, 0) == '/') {
					advance(st);
					break;
				}
			}
		} else if (c == open_ch) {
			depth++;
			advance(st);
		} else if (c == close_ch) {
			depth--;
			if (depth == 0) {
				*out_inner_end = st->pos;
				advance(st);
				return 1;
			}
			advance(st);
		} else {
			advance(st);
		}
	}
	*out_inner_end = st->pos;
	add_diag(st, start_line, start_col, start_off,
	         "unterminated '%c' ... '%c'", open_ch, close_ch);
	return 0;
}

static size_t parse_capture_list(
        cx_state *st, const char *s, size_t n, uint32_t line, uint32_t col,
        uint32_t off, cx_capture **out_caps)
{
	*out_caps = NULL;
	trim_slice(&s, &n);
	if (n == 0)
		return 0;

	size_t cap = 4, count = 0;
	cx_capture *arr = (cx_capture *)calloc(cap, sizeof(*arr));
	if (!arr)
		return 0;

	size_t start = 0;
	int pdepth = 0, bdepth = 0, cdepth = 0;
	for (size_t i = 0; i <= n; i++) {
		char c = (i < n) ? s[i] : ',';
		if (c == '(') pdepth++;
		else if (c == ')' && pdepth > 0) pdepth--;
		else if (c == '[') bdepth++;
		else if (c == ']' && bdepth > 0) bdepth--;
		else if (c == '{') cdepth++;
		else if (c == '}' && cdepth > 0) cdepth--;

		if (c == ',' && pdepth == 0 && bdepth == 0 && cdepth == 0) {
			const char *seg = s + start;
			size_t slen = i - start;
			trim_slice(&seg, &slen);
			if (slen > 0) {
				/* Find top-level '=' for init capture `int id = song->id` */
				size_t eq_pos = slen;
				int ep = 0, eb = 0;
				for (size_t k = 0; k < slen; k++) {
					if (seg[k] == '(') ep++;
					else if (seg[k] == ')' && ep > 0) ep--;
					else if (seg[k] == '[') eb++;
					else if (seg[k] == ']' && eb > 0) eb--;
					else if (seg[k] == '=' && ep == 0 && eb == 0 &&
					         (k + 1 >= slen || seg[k + 1] != '=') &&
					         (k == 0 || (seg[k - 1] != '!' && seg[k - 1] != '<' &&
					                     seg[k - 1] != '>' && seg[k - 1] != '=')))
					{
						eq_pos = k;
						break;
					}
				}

				const char *lhs = seg;
				size_t lhs_len = eq_pos;
				trim_slice(&lhs, &lhs_len);

				const char *rhs = (eq_pos < slen) ? (seg + eq_pos + 1) : NULL;
				size_t rhs_len = (eq_pos < slen) ? (slen - eq_pos - 1) : 0;
				if (rhs)
					trim_slice(&rhs, &rhs_len);

				/* Check if lhs is a bare identifier (component-param shorthand) */
				int bare_ident = (lhs_len > 0 && is_ident_start(lhs[0]));
				for (size_t k = 1; k < lhs_len && bare_ident; k++) {
					if (!is_ident_char(lhs[k]))
						bare_ident = 0;
				}

				char *decl = NULL;
				char *name = NULL;
				char *init_expr = NULL;

				if (bare_ident) {
					const char *pdecl = lookup_comp_param_decl(st, lhs, lhs_len);
					if (!pdecl) {
						add_diag(st, line, col, off,
						         "capture '%.*s' requires an explicit type (not a component parameter)",
						         (int)lhs_len, lhs);
					} else {
						decl = cx_strndup(pdecl, strlen(pdecl));
						name = cx_strndup(lhs, lhs_len);
						init_expr = rhs ? cx_strndup(rhs, rhs_len)
						                : cx_strndup(lhs, lhs_len);
					}
				} else {
					name = extract_decl_name(lhs, lhs_len);
					if (!name) {
						add_diag(st, line, col, off,
						         "cannot parse capture declaration '%.*s'",
						         (int)lhs_len, lhs);
					} else {
						decl = cx_strndup(lhs, lhs_len);
						init_expr = rhs ? cx_strndup(rhs, rhs_len)
						                : cx_strndup(name, strlen(name));
					}
				}

				if (decl && name && init_expr) {
					if (count >= cap) {
						cap *= 2;
						cx_capture *np = (cx_capture *)realloc(arr, cap * sizeof(*np));
						if (!np) {
							free(decl);
							free(name);
							free(init_expr);
							break;
						}
						arr = np;
					}
					arr[count].decl = decl;
					arr[count].name = name;
					arr[count].init_expr = init_expr;
					count++;
				} else {
					free(decl);
					free(name);
					free(init_expr);
				}
			}
			start = i + 1;
		}
	}

	*out_caps = arr;
	return count;
}

/* Parse `[captures](bud_event *e) { body }` and emit bud_bind_ctx call into dst. */
static void parse_and_lift_closure(
        cx_state *st, cx_buf *dst, const char *el_var, const char *ev_name)
{
	uint32_t cl_line = st->line, cl_col = st->col, cl_off = (uint32_t)st->pos;
	size_t cap_s = 0, cap_e = 0;
	if (!scan_balanced_raw(st, '[', ']', &cap_s, &cap_e))
		return;

	cx_capture *caps = NULL;
	size_t cap_count = parse_capture_list(
	        st, st->src + cap_s, cap_e - cap_s, cl_line, cl_col, cl_off, &caps);

	skip_ws_and_comments(st);

	/* Optional parameter list `(bud_event *e)` or `()` */
	char *ev_param_decl = NULL;
	char *ev_param_name = NULL;
	if (peek_at(st, 0) == '(') {
		size_t ps = 0, pe = 0;
		if (scan_balanced_raw(st, '(', ')', &ps, &pe)) {
			const char *pstr = st->src + ps;
			size_t plen = pe - ps;
			trim_slice(&pstr, &plen);
			if (plen > 0 && !(plen == 4 && memcmp(pstr, "void", 4) == 0)) {
				ev_param_decl = cx_strndup(pstr, plen);
				ev_param_name = extract_decl_name(pstr, plen);
			}
		}
	}
	if (!ev_param_decl || !ev_param_name) {
		free(ev_param_decl);
		free(ev_param_name);
		ev_param_decl = cx_strndup("bud_event *__cx_ev", 18);
		ev_param_name = cx_strndup("__cx_ev", 7);
	}

	skip_ws_and_comments(st);
	if (peek_at(st, 0) != '{') {
		add_diag(st, st->line, st->col, (uint32_t)st->pos,
		         "expected '{' to start closure body");
		goto cleanup;
	}

	uint32_t cid = ++st->next_closure_id;
	uint32_t body_line = st->line;

	/* Emit hoisted struct + trampoline function into st->hoist */
	emit_line_directive(st, &st->hoist, cl_line);
	if (cap_count > 0) {
		buf_printf(&st->hoist, "typedef struct __cx_cap_%u {\n", cid);
		for (size_t i = 0; i < cap_count; i++)
			buf_printf(&st->hoist, "\t%s;\n", caps[i].decl);
		buf_printf(&st->hoist, "} __cx_cap_%u;\n", cid);
	}

	buf_printf(&st->hoist, "static int __cx_fn_%u(%s) {\n", cid, ev_param_decl);
	if (cap_count > 0) {
		buf_printf(&st->hoist, "\t__cx_cap_%u *__cx_c = (__cx_cap_%u *)%s->user;\n",
		           cid, cid, ev_param_name);
		for (size_t i = 0; i < cap_count; i++) {
			buf_printf(&st->hoist, "\t%s = __cx_c->%s; (void)%s;\n",
			           caps[i].decl, caps[i].name, caps[i].name);
		}
	}
	buf_printf(&st->hoist, "\t(void)%s;\n", ev_param_name);
	emit_line_directive(st, &st->hoist, body_line);

	/* Consume '{' and transpile closure body into st->hoist */
	advance(st);
	buf_str(&st->hoist, "{\n");
	transpile_c_until(st, &st->hoist, &st->hoist_spans, '}', 0, CX_SPAN_CLOSURE);
	if (peek_at(st, 0) == '}')
		advance(st);
	buf_str(&st->hoist, "\n}\n\treturn 0;\n}\n");

	/* Emit bud_bind_ctx at the call site */
	if (cap_count > 0) {
		buf_printf(dst, "\tbud_bind_ctx(%s, \"%s\", 1, __cx_fn_%u, &(__cx_cap_%u){ ",
		           el_var, ev_name, cid, cid);
		for (size_t i = 0; i < cap_count; i++) {
			if (i > 0)
				buf_str(dst, ", ");
			buf_printf(dst, ".%s = (%s)", caps[i].name, caps[i].init_expr);
		}
		buf_printf(dst, " }, sizeof(__cx_cap_%u));\n", cid);
	} else {
		buf_printf(dst, "\tbud_bind_ctx(%s, \"%s\", 1, __cx_fn_%u, NULL, 0);\n",
		           el_var, ev_name, cid);
	}

cleanup:
	free(ev_param_decl);
	free(ev_param_name);
	for (size_t i = 0; i < cap_count; i++) {
		free(caps[i].decl);
		free(caps[i].name);
		free(caps[i].init_expr);
	}
	free(caps);
}

/* ── JSX Text Normalization ───────────────────────────────────────────── */

static void normalize_jsx_text(const char *raw, size_t len, cx_buf *out)
{
	int has_nl = 0;
	for (size_t i = 0; i < len; i++) {
		if (raw[i] == '\n') {
			has_nl = 1;
			break;
		}
	}
	if (!has_nl) {
		buf_append(out, raw, len);
		return;
	}

	/* Multiline JSX text: trim each line, skip blank lines, join with single space */
	size_t i = 0;
	int emitted_any = 0;
	while (i < len) {
		size_t line_start = i;
		while (i < len && raw[i] != '\n')
			i++;
		size_t line_end = i;
		if (i < len && raw[i] == '\n')
			i++;

		const char *s = raw + line_start;
		size_t slen = line_end - line_start;
		if (slen > 0 && s[slen - 1] == '\r')
			slen--;
		trim_slice(&s, &slen);
		if (slen > 0) {
			if (emitted_any)
				buf_ch(out, ' ');
			buf_append(out, s, slen);
			emitted_any = 1;
		}
	}
}

/* ── JSX Control Flow Inside `{for ...}` / `{if ...}` ─────────────────── */

static int match_keyword_ahead(const cx_state *st, const char *kw)
{
	size_t klen = strlen(kw);
	if (st->pos + klen > st->len)
		return 0;
	if (memcmp(st->src + st->pos, kw, klen) != 0)
		return 0;
	if (st->pos + klen < st->len && is_ident_char(st->src[st->pos + klen]))
		return 0;
	return 1;
}

static void parse_jsx_stmt_body(
        cx_state *st, cx_buf *dst, cx_span_list *spans, const char *parent_var)
{
	skip_ws_and_comments(st);

	if (peek_at(st, 0) == '<') {
		buf_printf(dst, " { bud_append_opt(%s, ", parent_var);
		parse_jsx_expr(st, dst, spans);
		buf_str(dst, "); }\n");
		return;
	}

	if (match_keyword_ahead(st, "if")) {
		uint32_t k_off = (uint32_t)st->pos;
		uint32_t c_off = (uint32_t)dst->len;
		st->pos += 2;
		st->col += 2;
		skip_ws_and_comments(st);
		size_t cs = 0, ce = 0;
		if (scan_balanced_raw(st, '(', ')', &cs, &ce)) {
			buf_str(dst, " if (");
			buf_append(dst, st->src + cs, ce - cs);
			buf_str(dst, ")");
			span_push(spans, k_off, (uint32_t)(st->pos - k_off),
			          c_off, (uint32_t)(dst->len - c_off), CX_SPAN_LOOP);
			parse_jsx_stmt_body(st, dst, spans, parent_var);
			skip_ws_and_comments(st);
			if (match_keyword_ahead(st, "else")) {
				st->pos += 4;
				st->col += 4;
				buf_str(dst, " else");
				parse_jsx_stmt_body(st, dst, spans, parent_var);
			}
		}
		return;
	}

	if (peek_at(st, 0) == '{') {
		/* Block body inside {for (...) { const S *x = &arr[i]; <Row s={x} />; }} */
		advance(st);
		buf_str(dst, " {\n");
		while (st->pos < st->len) {
			skip_ws_and_comments(st);
			if (peek_at(st, 0) == '}') {
				advance(st);
				break;
			}
			if (peek_at(st, 0) == '<' &&
			    (peek_at(st, 1) == '>' || is_ident_start(peek_at(st, 1))))
			{
				buf_printf(dst, "\tbud_append_opt(%s, ", parent_var);
				parse_jsx_expr(st, dst, spans);
				buf_str(dst, ");\n");
				skip_ws_and_comments(st);
				if (peek_at(st, 0) == ';')
					advance(st);
				continue;
			}
			/* Emit one C statement up to ';' or '}' */
			transpile_c_until(st, dst, spans, ';', 0, CX_SPAN_PASSTHROUGH);
			if (peek_at(st, 0) == ';') {
				buf_ch(dst, advance(st));
				buf_ch(dst, '\n');
			} else if (peek_at(st, 0) == '}') {
				advance(st);
				break;
			}
		}
		buf_str(dst, "}\n");
		return;
	}

	/* Fallback: expression ending before '}' */
	buf_printf(dst, " { bud_append_opt(%s, bud_child_any((", parent_var);
	transpile_c_until(st, dst, spans, '}', 1, CX_SPAN_EXPR);
	buf_str(dst, "))); }\n");
}

/* ── JSX Element / Fragment / Component Parser ────────────────────────── */

typedef struct {
	char    *name;
	uint32_t name_off;
	uint32_t name_len;
	int      is_string;
	int      is_bool_bare;
	int      is_closure;
	cx_buf   val_code;
	cx_span_list val_spans;
} cx_attr;

static void attr_free(cx_attr *a)
{
	free(a->name);
	buf_free(&a->val_code);
	span_list_free(&a->val_spans);
}

static void parse_jsx_children(
        cx_state *st, cx_buf *dst, cx_span_list *spans,
        const char *parent_var, const char *closing_tag)
{
	while (st->pos < st->len) {
		char c = peek_at(st, 0);

		/* Closing tag `</tag>` or `</>` */
		if (c == '<' && peek_at(st, 1) == '/') {
			uint32_t cl_line = st->line, cl_col = st->col;
			uint32_t cl_off = (uint32_t)st->pos;
			advance(st); /* < */
			advance(st); /* / */
			skip_ws_and_comments(st);
			size_t tstart = st->pos;
			while (st->pos < st->len && is_attr_char(peek_at(st, 0)))
				advance(st);
			size_t tlen = st->pos - tstart;
			skip_ws_and_comments(st);
			if (peek_at(st, 0) == '>')
				advance(st);
			else
				add_diag(st, cl_line, cl_col, cl_off, "expected '>' in closing tag");

			if (closing_tag) {
				if (strlen(closing_tag) != tlen ||
				    memcmp(closing_tag, st->src + tstart, tlen) != 0)
				{
					add_diag(st, cl_line, cl_col, cl_off,
					         "mismatched closing tag '</%.*s>', expected '</%s>'",
					         (int)tlen, st->src + tstart, closing_tag);
				}
			}
			return;
		}

		/* Nested JSX child */
		if (c == '<' && (peek_at(st, 1) == '>' || is_ident_start(peek_at(st, 1)))) {
			buf_printf(dst, "\tbud_append_opt(%s, ", parent_var);
			parse_jsx_expr(st, dst, spans);
			buf_str(dst, ");\n");
			continue;
		}

		/* `{ ... }` hole: comment, for-loop, if-branch, or C expression */
		if (c == '{') {
			uint32_t h_line = st->line, h_col = st->col, h_off = (uint32_t)st->pos;
			advance(st); /* { */
			skip_ws_and_comments(st);

			if (peek_at(st, 0) == '}') {
				/* Empty or comment-only JSX expression hole */
				advance(st);
				continue;
			}

			if (match_keyword_ahead(st, "for")) {
				uint32_t f_off = (uint32_t)st->pos;
				uint32_t c_off = (uint32_t)dst->len;
				st->pos += 3;
				st->col += 3;
				skip_ws_and_comments(st);
				size_t hs = 0, he = 0;
				if (scan_balanced_raw(st, '(', ')', &hs, &he)) {
					buf_str(dst, "\tfor (");
					buf_append(dst, st->src + hs, he - hs);
					buf_str(dst, ")");
					span_push(spans, f_off, (uint32_t)(st->pos - f_off),
					          c_off, (uint32_t)(dst->len - c_off), CX_SPAN_LOOP);
					parse_jsx_stmt_body(st, dst, spans, parent_var);
				}
				skip_ws_and_comments(st);
				if (peek_at(st, 0) == '}')
					advance(st);
				else
					add_diag(st, h_line, h_col, h_off, "expected '}' after '{for ...}'");
				continue;
			}

			if (match_keyword_ahead(st, "if")) {
				uint32_t i_off = (uint32_t)st->pos;
				uint32_t c_off = (uint32_t)dst->len;
				st->pos += 2;
				st->col += 2;
				skip_ws_and_comments(st);
				size_t cs = 0, ce = 0;
				if (scan_balanced_raw(st, '(', ')', &cs, &ce)) {
					buf_str(dst, "\tif (");
					buf_append(dst, st->src + cs, ce - cs);
					buf_str(dst, ")");
					span_push(spans, i_off, (uint32_t)(st->pos - i_off),
					          c_off, (uint32_t)(dst->len - c_off), CX_SPAN_LOOP);
					parse_jsx_stmt_body(st, dst, spans, parent_var);
					skip_ws_and_comments(st);
					if (match_keyword_ahead(st, "else")) {
						st->pos += 4;
						st->col += 4;
						buf_str(dst, "\telse");
						parse_jsx_stmt_body(st, dst, spans, parent_var);
					}
				}
				skip_ws_and_comments(st);
				if (peek_at(st, 0) == '}')
					advance(st);
				else
					add_diag(st, h_line, h_col, h_off, "expected '}' after '{if ...}'");
				continue;
			}

			/* Regular expression `{expr}` */
			buf_printf(dst, "\tbud_append_opt(%s, bud_child_any((", parent_var);
			transpile_c_until(st, dst, spans, '}', 1, CX_SPAN_EXPR);
			buf_str(dst, ")));\n");
			if (peek_at(st, 0) == '}')
				advance(st);
			else
				add_diag(st, h_line, h_col, h_off, "unterminated '{' in JSX child");
			continue;
		}

		/* Literal JSX text run until '<' or '{' */
		size_t tstart = st->pos;
		while (st->pos < st->len && peek_at(st, 0) != '<' && peek_at(st, 0) != '{')
			advance(st);
		if (st->pos > tstart) {
			cx_buf norm = { 0 };
			normalize_jsx_text(st->src + tstart, st->pos - tstart, &norm);
			if (norm.len > 0) {
				buf_printf(dst, "\tbud_append_opt(%s, bud_text(", parent_var);
				buf_escaped_cstr(dst, norm.data, norm.len);
				buf_str(dst, "));\n");
			}
			buf_free(&norm);
		}
	}

	add_diag(st, st->line, st->col, (uint32_t)st->pos,
	         "unterminated JSX element '<%s>'", closing_tag ? closing_tag : "");
}

static void parse_jsx_expr(cx_state *st, cx_buf *dst, cx_span_list *spans)
{
	uint32_t start_line = st->line, start_col = st->col;
	uint32_t start_off = (uint32_t)st->pos;

	advance(st); /* consume '<' */

	/* Fragment `<> ... </>` */
	if (peek_at(st, 0) == '>') {
		advance(st);
		uint32_t nid = ++st->next_node_id;
		char var[32];
		snprintf(var, sizeof(var), "__cx_n%u", nid);

		emit_line_directive(st, dst, start_line);
		buf_printf(dst, "__extension__ ({\n\tbud_node *%s = bud_fragment();\n", var);
		parse_jsx_children(st, dst, spans, var, "");
		buf_printf(dst, "\t%s;\n})", var);
		return;
	}

	/* Read tag name */
	uint32_t tag_off = (uint32_t)st->pos;
	while (st->pos < st->len && is_attr_char(peek_at(st, 0)))
		advance(st);
	size_t tag_len = st->pos - tag_off;
	if (tag_len == 0) {
		add_diag(st, start_line, start_col, start_off, "expected tag name after '<'");
		buf_str(dst, "NULL");
		return;
	}
	char *tag = cx_strndup(st->src + tag_off, tag_len);
	int is_component = isupper((unsigned char)tag[0]);

	uint32_t nid = ++st->next_node_id;
	char var[32];
	snprintf(var, sizeof(var), "__cx_n%u", nid);

	emit_line_directive(st, dst, start_line);
	buf_str(dst, "__extension__ ({\n");

	/* Parse attributes */
	cx_attr *attrs = NULL;
	size_t attr_count = 0, attr_cap = 0;
	int self_closing = 0;
	int header_closed = 0;

	/* For HTML elements, emit element creation immediately so closures & attrs stream cleanly */
	if (!is_component) {
		buf_printf(dst, "\tbud_node *%s = bud_element(", var);
		uint32_t tag_c_off = (uint32_t)dst->len + 1;
		buf_escaped_cstr(dst, tag, tag_len);
		span_push(spans, tag_off, (uint32_t)tag_len,
		          tag_c_off, (uint32_t)tag_len, CX_SPAN_TAG);
		buf_str(dst, ");\n");
	}

	while (st->pos < st->len) {
		skip_ws_and_comments(st);
		char c = peek_at(st, 0);
		if (c == '/' && peek_at(st, 1) == '>') {
			advance(st);
			advance(st);
			self_closing = 1;
			header_closed = 1;
			break;
		}
		if (c == '>') {
			advance(st);
			header_closed = 1;
			break;
		}
		if (!is_ident_start(c)) {
			add_diag(st, st->line, st->col, (uint32_t)st->pos,
			         "unexpected character '%c' in <%s> attribute list", c, tag);
			if (st->recover)
				break;
			advance(st);
			continue;
		}

		uint32_t an_off = (uint32_t)st->pos;
		while (st->pos < st->len && is_attr_char(peek_at(st, 0)))
			advance(st);
		size_t an_len = st->pos - an_off;
		char *aname = cx_strndup(st->src + an_off, an_len);

		skip_ws_and_comments(st);

		if (!is_component) {
			/* Direct emission on HTML element */
			if (peek_at(st, 0) != '=') {
				/* Bare boolean attribute: `<button disabled>` */
				uint32_t ac = (uint32_t)dst->len;
				buf_printf(dst, "\tbud_set_attr(%s, ", var);
				buf_escaped_cstr(dst, aname, an_len);
				buf_str(dst, ", \"\");\n");
				span_push(spans, an_off, (uint32_t)an_len, ac, (uint32_t)an_len, CX_SPAN_ATTR);
				free(aname);
				continue;
			}
			advance(st); /* '=' */
			skip_ws_and_comments(st);

			int is_key = (an_len == 3 && memcmp(aname, "key", 3) == 0);
			int is_event = (an_len >= 3 && aname[0] == 'o' && aname[1] == 'n' &&
			                isupper((unsigned char)aname[2]));

			if (peek_at(st, 0) == '"') {
				size_t vs = st->pos + 1;
				advance(st);
				while (st->pos < st->len && peek_at(st, 0) != '"') {
					if (peek_at(st, 0) == '\\' && st->pos + 1 < st->len)
						advance(st);
					advance(st);
				}
				size_t ve = st->pos;
				if (peek_at(st, 0) == '"')
					advance(st);

				if (is_key) {
					buf_printf(dst, "\tbud_set_key(%s, \"%.*s\");\n",
					           var, (int)(ve - vs), st->src + vs);
				} else {
					buf_printf(dst, "\tbud_set_attr(%s, ", var);
					uint32_t ac = (uint32_t)dst->len + 1;
					buf_escaped_cstr(dst, aname, an_len);
					span_push(spans, an_off, (uint32_t)an_len, ac, (uint32_t)an_len, CX_SPAN_ATTR);
					buf_printf(dst, ", \"%.*s\");\n", (int)(ve - vs), st->src + vs);
				}
			} else if (peek_at(st, 0) == '{') {
				uint32_t bline = st->line, bcol = st->col, boff = (uint32_t)st->pos;
				advance(st); /* '{' */
				skip_ws_and_comments(st);

				if (is_event) {
					char ev_lower[64];
					size_t elen = an_len - 2;
					if (elen >= sizeof(ev_lower))
						elen = sizeof(ev_lower) - 1;
					for (size_t k = 0; k < elen; k++)
						ev_lower[k] = (char)tolower((unsigned char)aname[2 + k]);
					ev_lower[elen] = '\0';

					if (peek_at(st, 0) == '[') {
						parse_and_lift_closure(st, dst, var, ev_lower);
						skip_ws_and_comments(st);
					} else {
						buf_printf(dst, "\tbud_bind_ctx(%s, \"%s\", 1, (", var, ev_lower);
						transpile_c_until(st, dst, spans, '}', 1, CX_SPAN_EXPR);
						buf_str(dst, "), NULL, 0);\n");
					}
				} else if (is_key) {
					buf_printf(dst, "\tbud_set_key_any(%s, (", var);
					transpile_c_until(st, dst, spans, '}', 1, CX_SPAN_EXPR);
					buf_str(dst, "));\n");
				} else {
					buf_printf(dst, "\tbud_set_attr_any(%s, ", var);
					uint32_t ac = (uint32_t)dst->len + 1;
					buf_escaped_cstr(dst, aname, an_len);
					span_push(spans, an_off, (uint32_t)an_len, ac, (uint32_t)an_len, CX_SPAN_ATTR);
					buf_str(dst, ", (");
					transpile_c_until(st, dst, spans, '}', 1, CX_SPAN_EXPR);
					buf_str(dst, "));\n");
				}

				if (peek_at(st, 0) == '}')
					advance(st);
				else
					add_diag(st, bline, bcol, boff, "unterminated '{' in attribute '%s'", aname);
			} else {
				add_diag(st, st->line, st->col, (uint32_t)st->pos,
				         "expected '\"' or '{' after '%s='", aname);
			}
			free(aname);
		} else {
			/* Collect props for uppercase Component */
			if (attr_count >= attr_cap) {
				size_t ncap = attr_cap ? attr_cap * 2 : 8;
				cx_attr *na = (cx_attr *)realloc(attrs, ncap * sizeof(*na));
				if (!na) {
					free(aname);
					break;
				}
				attrs = na;
				attr_cap = ncap;
			}
			cx_attr *a = &attrs[attr_count++];
			memset(a, 0, sizeof(*a));
			a->name = aname;
			a->name_off = an_off;
			a->name_len = (uint32_t)an_len;

			if (peek_at(st, 0) != '=') {
				a->is_bool_bare = 1;
				continue;
			}
			advance(st); /* '=' */
			skip_ws_and_comments(st);

			if (peek_at(st, 0) == '"') {
				a->is_string = 1;
				scan_string(st, &a->val_code, '"');
			} else if (peek_at(st, 0) == '{') {
				uint32_t bline = st->line, bcol = st->col, boff = (uint32_t)st->pos;
				advance(st);
				transpile_c_until(st, &a->val_code, &a->val_spans, '}', 1, CX_SPAN_EXPR);
				if (peek_at(st, 0) == '}')
					advance(st);
				else
					add_diag(st, bline, bcol, boff, "unterminated '{' in prop '%s'", aname);
			} else {
				add_diag(st, st->line, st->col, (uint32_t)st->pos,
				         "expected '\"' or '{' after prop '%s='", aname);
			}
		}
	}

	if (!header_closed)
		add_diag(st, start_line, start_col, start_off, "unterminated opening tag '<%s'", tag);

	if (is_component) {
		/* If component has children, build fragment first */
		char ch_var[32] = { 0 };
		int has_children = (!self_closing && header_closed);
		if (has_children) {
			uint32_t ch_id = ++st->next_node_id;
			snprintf(ch_var, sizeof(ch_var), "__cx_ch%u", ch_id);
			buf_printf(dst, "\tbud_node *%s = bud_fragment();\n", ch_var);
			parse_jsx_children(st, dst, spans, ch_var, tag);
		}

		/* Emit bud_component_vnode(Tag_render, NULL, &(Tag_props){ ... }, sizeof(Tag_props)) */
		buf_printf(dst, "\tbud_node *%s = bud_component_vnode(", var);
		uint32_t comp_c_off = (uint32_t)dst->len;
		buf_printf(dst, "%s_render, NULL, &(%s_props){ ", tag, tag);
		span_push(spans, tag_off, (uint32_t)tag_len,
		          comp_c_off, (uint32_t)tag_len, CX_SPAN_COMPONENT);

		int emitted_prop = 0;
		const cx_attr *key_attr = NULL;
		for (size_t i = 0; i < attr_count; i++) {
			cx_attr *a = &attrs[i];
			if (strcmp(a->name, "key") == 0) {
				key_attr = a;
				continue;
			}
			if (emitted_prop)
				buf_str(dst, ", ");
			buf_ch(dst, '.');
			uint32_t ac = (uint32_t)dst->len;
			buf_str(dst, a->name);
			span_push(spans, a->name_off, a->name_len, ac, a->name_len, CX_SPAN_ATTR);
			buf_str(dst, " = ");
			if (a->is_bool_bare) {
				buf_str(dst, "1");
			} else if (a->is_string) {
				buf_append(dst, a->val_code.data, a->val_code.len);
			} else {
				buf_ch(dst, '(');
				uint32_t vbase = (uint32_t)dst->len;
				buf_append(dst, a->val_code.data, a->val_code.len);
				for (size_t s = 0; s < a->val_spans.count; s++) {
					cx_span sp = a->val_spans.items[s];
					span_push(spans, sp.cx_off, sp.cx_len,
					          sp.c_off + vbase, sp.c_len, sp.kind);
				}
				buf_ch(dst, ')');
			}
			emitted_prop = 1;
		}
		if (has_children) {
			if (emitted_prop)
				buf_str(dst, ", ");
			buf_printf(dst, ".children = %s", ch_var);
			emitted_prop = 1;
		}
		if (!emitted_prop)
			buf_str(dst, "0");

		buf_printf(dst, " }, sizeof(%s_props));\n", tag);

		if (key_attr) {
			if (key_attr->is_string) {
				buf_printf(dst, "\tbud_set_key(%s, ", var);
				buf_append(dst, key_attr->val_code.data, key_attr->val_code.len);
				buf_str(dst, ");\n");
			} else if (!key_attr->is_bool_bare) {
				buf_printf(dst, "\tbud_set_key_any(%s, (", var);
				uint32_t vbase = (uint32_t)dst->len;
				buf_append(dst, key_attr->val_code.data, key_attr->val_code.len);
				for (size_t s = 0; s < key_attr->val_spans.count; s++) {
					cx_span sp = key_attr->val_spans.items[s];
					span_push(spans, sp.cx_off, sp.cx_len,
					          sp.c_off + vbase, sp.c_len, sp.kind);
				}
				buf_str(dst, "));\n");
			}
		}

		for (size_t i = 0; i < attr_count; i++)
			attr_free(&attrs[i]);
		free(attrs);
	} else {
		if (!self_closing && header_closed)
			parse_jsx_children(st, dst, spans, var, tag);
	}

	buf_printf(dst, "\t%s;\n})", var);
	free(tag);
}

/* ── Top-Level `component Name(...) { ... }` Parser ───────────────────── */

/* Peek ahead from current pos (after optional `static`) to see if `component <Ident>(` follows. */
static int peek_component_decl(const cx_state *st, int *out_is_static, size_t *out_after_kw)
{
	size_t p = st->pos;
	*out_is_static = 0;

	/* Optional `static` keyword */
	if (p + 6 <= st->len && memcmp(st->src + p, "static", 6) == 0 &&
	    (p + 6 == st->len || !is_ident_char(st->src[p + 6])))
	{
		size_t q = p + 6;
		while (q < st->len && isspace((unsigned char)st->src[q]))
			q++;
		if (q + 9 <= st->len && memcmp(st->src + q, "component", 9) == 0 &&
		    (q + 9 == st->len || !is_ident_char(st->src[q + 9])))
		{
			*out_is_static = 1;
			p = q;
		} else {
			return 0;
		}
	} else if (p + 9 <= st->len && memcmp(st->src + p, "component", 9) == 0 &&
	           (p + 9 == st->len || !is_ident_char(st->src[p + 9])))
	{
		/* non-static component */
	} else {
		return 0;
	}

	p += 9;
	while (p < st->len && isspace((unsigned char)st->src[p]))
		p++;
	if (p >= st->len || !is_ident_start(st->src[p]))
		return 0;
	size_t id_end = p;
	while (id_end < st->len && is_ident_char(st->src[id_end]))
		id_end++;
	size_t after_id = id_end;
	while (after_id < st->len && isspace((unsigned char)st->src[after_id]))
		after_id++;
	if (after_id >= st->len || st->src[after_id] != '(')
		return 0;

	*out_after_kw = p;
	return 1;
}

static void parse_component_decl(cx_state *st)
{
	int is_static = 0;
	size_t name_pos = 0;
	if (!peek_component_decl(st, &is_static, &name_pos))
		return;

	uint32_t decl_line = st->line;
	while (st->pos < name_pos)
		advance(st);

	uint32_t name_off = (uint32_t)st->pos;
	while (st->pos < st->len && is_ident_char(peek_at(st, 0)))
		advance(st);
	size_t name_len = st->pos - name_off;
	char *comp_name = cx_strndup(st->src + name_off, name_len);

	skip_ws_and_comments(st);
	size_t ps = 0, pe = 0;
	scan_balanced_raw(st, '(', ')', &ps, &pe);

	clear_comp_params(st);
	st->comp_param_count = parse_param_slice(st->src, st->src + ps, pe - ps, &st->comp_params);

	/* Emit props struct into st->hoist */
	emit_line_directive(st, &st->hoist, decl_line);
	buf_printf(&st->hoist, "typedef struct %s_props {\n", comp_name);
	if (st->comp_param_count == 0) {
		buf_str(&st->hoist, "\tint _cx_empty;\n");
	} else {
		for (size_t i = 0; i < st->comp_param_count; i++) {
			buf_ch(&st->hoist, '\t');
			uint32_t hc_off = (uint32_t)st->hoist.len;
			buf_str(&st->hoist, st->comp_params[i].decl);
			if (st->comp_params[i].decl_len > 0) {
				span_push(&st->hoist_spans,
				          st->comp_params[i].decl_off, st->comp_params[i].decl_len,
				          hc_off, st->comp_params[i].decl_len, CX_SPAN_PASSTHROUGH);
			}
			buf_str(&st->hoist, ";\n");
		}
	}
	buf_printf(&st->hoist, "} %s_props;\n", comp_name);

	skip_ws_and_comments(st);

	/* Forward declaration: `component Foo(...);` */
	if (peek_at(st, 0) == ';') {
		advance(st);
		emit_line_directive(st, &st->body, decl_line);
		buf_printf(&st->body, "%sbud_node *", is_static ? "static " : "");
		uint32_t fn_c_off = (uint32_t)st->body.len;
		buf_printf(&st->body, "%s_render(void *__cx_ctx, const void *__cx_raw_props);\n", comp_name);
		span_push(&st->body_spans, name_off, (uint32_t)name_len,
		          fn_c_off, (uint32_t)(name_len + 7), CX_SPAN_COMPONENT);
		clear_comp_params(st);
		flush_top_level(st);
		free(comp_name);
		return;
	}

	if (peek_at(st, 0) != '{') {
		add_diag(st, st->line, st->col, (uint32_t)st->pos,
		         "expected '{' or ';' after component '%s(...)'", comp_name);
		clear_comp_params(st);
		free(comp_name);
		return;
	}

	uint32_t body_line = st->line;
	advance(st); /* consume '{' */
	st->brace_depth++;
	st->comp_brace_depth = st->brace_depth;

	emit_line_directive(st, &st->body, decl_line);
	buf_printf(&st->body, "%sbud_node *", is_static ? "static " : "");
	uint32_t fn_c_off = (uint32_t)st->body.len;
	buf_printf(&st->body, "%s_render(void *__cx_ctx, const void *__cx_raw_props) {\n", comp_name);
	span_push(&st->body_spans, name_off, (uint32_t)name_len,
	          fn_c_off, (uint32_t)(name_len + 7), CX_SPAN_COMPONENT);
	buf_str(&st->body, "\t(void)__cx_ctx;\n");
	buf_printf(&st->body, "\tconst %s_props *__cx_p = (const %s_props *)__cx_raw_props;\n",
	           comp_name, comp_name);
	buf_str(&st->body, "\t(void)__cx_p;\n");
	for (size_t i = 0; i < st->comp_param_count; i++) {
		buf_ch(&st->body, '\t');
		uint32_t pc_off = (uint32_t)st->body.len;
		buf_str(&st->body, st->comp_params[i].decl);
		if (st->comp_params[i].decl_len > 0) {
			span_push(&st->body_spans,
			          st->comp_params[i].decl_off, st->comp_params[i].decl_len,
			          pc_off, st->comp_params[i].decl_len, CX_SPAN_PASSTHROUGH);
		}
		buf_printf(&st->body, " = __cx_p->%s; (void)%s;\n",
		           st->comp_params[i].name, st->comp_params[i].name);
	}
	emit_line_directive(st, &st->body, body_line);
	free(comp_name);
}

/* ── Main C Scanner with Context-Aware JSX Entry ──────────────────────── */

static void transpile_c_until(
        cx_state *st, cx_buf *dst, cx_span_list *spans,
        char stop_ch, int initial_allow_jsx, uint8_t passthrough_kind)
{
	int allow_jsx = initial_allow_jsx;
	int local_brace = 0, local_paren = 0, local_bracket = 0;
	int at_line_start = (stop_ch == '\0');

	while (st->pos < st->len) {
		char c = peek_at(st, 0);

		/* Stop condition for nested expressions / blocks */
		if (stop_ch != '\0' && c == stop_ch &&
		    local_brace == 0 && local_paren == 0 && local_bracket == 0)
		{
			return;
		}
		if (stop_ch == ';' && c == '}' &&
		    local_brace == 0 && local_paren == 0 && local_bracket == 0)
		{
			return;
		}
		if (st->recover && stop_ch == '}' && passthrough_kind == CX_SPAN_EXPR &&
		    local_brace == 0 && local_paren == 0 && local_bracket == 0 &&
		    (c == ';' || c == ')'))
		{
			return;
		}

		/* Whitespace preserves allow_jsx */
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			uint32_t s_off = (uint32_t)st->pos;
			uint32_t c_off = (uint32_t)dst->len;
			while (st->pos < st->len) {
				char wc = peek_at(st, 0);
				if (wc == ' ' || wc == '\t' || wc == '\r' || wc == '\n') {
					advance(st);
					buf_ch(dst, wc);
					if (wc == '\n')
						at_line_start = 1;
				} else {
					break;
				}
			}
			span_push(spans, s_off, (uint32_t)(st->pos - s_off),
			          c_off, (uint32_t)(dst->len - c_off), passthrough_kind);
			continue;
		}

		/* Preprocessor directive at start of line */
		if (c == '#' && at_line_start) {
			if (stop_ch == '\0' && st->brace_depth == 0) {
				flush_top_level(st);
				scan_preprocessor_line(st, dst, spans);
				flush_top_level(st);
			} else {
				scan_preprocessor_line(st, dst, spans);
			}
			at_line_start = 1;
			continue;
		}
		at_line_start = 0;

		/* Comments preserve allow_jsx */
		if (c == '/' && peek_at(st, 1) == '/') {
			uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
			scan_line_comment(st, dst);
			span_push(spans, s_off, (uint32_t)(st->pos - s_off),
			          c_off, (uint32_t)(dst->len - c_off), passthrough_kind);
			at_line_start = 1;
			continue;
		}
		if (c == '/' && peek_at(st, 1) == '*') {
			uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
			scan_block_comment(st, dst);
			span_push(spans, s_off, (uint32_t)(st->pos - s_off),
			          c_off, (uint32_t)(dst->len - c_off), passthrough_kind);
			continue;
		}

		/* String / char literals */
		if (c == '"' || c == '\'') {
			uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
			scan_string(st, dst, c);
			span_push(spans, s_off, (uint32_t)(st->pos - s_off),
			          c_off, (uint32_t)(dst->len - c_off), passthrough_kind);
			allow_jsx = 0;
			continue;
		}

		/* Top-level `[static] component Name(...) { ... }` */
		if (stop_ch == '\0' && st->brace_depth == 0 && is_ident_start(c)) {
			int dummy_static = 0;
			size_t dummy_pos = 0;
			if (peek_component_decl(st, &dummy_static, &dummy_pos)) {
				parse_component_decl(st);
				allow_jsx = 0;
				continue;
			}
		}

		/* Identifiers & keywords */
		if (is_ident_start(c)) {
			uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
			while (st->pos < st->len && is_ident_char(peek_at(st, 0)))
				buf_ch(dst, advance(st));
			size_t id_len = st->pos - s_off;
			span_push(spans, s_off, (uint32_t)id_len,
			          c_off, (uint32_t)(dst->len - c_off), passthrough_kind);
			allow_jsx = (id_len == 6 && memcmp(st->src + s_off, "return", 6) == 0);
			continue;
		}

		/* JSX start: '<' in expression-start position followed by '>' or ident */
		if (c == '<' && allow_jsx &&
		    (peek_at(st, 1) == '>' || is_ident_start(peek_at(st, 1))))
		{
			parse_jsx_expr(st, dst, spans);
			allow_jsx = 0;
			continue;
		}

		/* Multi-char operators involving '=' or '<' so `==`, `<=`, `!=`, `+=` never set allow_jsx */
		if ((c == '=' || c == '!' || c == '<' || c == '>' ||
		     c == '+' || c == '-' || c == '*' || c == '/' ||
		     c == '%' || c == '&' || c == '|' || c == '^') &&
		    peek_at(st, 1) == '=')
		{
			uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
			buf_ch(dst, advance(st));
			buf_ch(dst, advance(st));
			span_push(spans, s_off, 2, c_off, 2, passthrough_kind);
			allow_jsx = 0;
			continue;
		}
		if ((c == '<' && peek_at(st, 1) == '<') || (c == '>' && peek_at(st, 1) == '>')) {
			uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
			buf_ch(dst, advance(st));
			buf_ch(dst, advance(st));
			if (peek_at(st, 0) == '=')
				buf_ch(dst, advance(st));
			span_push(spans, s_off, (uint32_t)(st->pos - s_off),
			          c_off, (uint32_t)(dst->len - c_off), passthrough_kind);
			allow_jsx = 0;
			continue;
		}

		/* Single punctuation / numbers */
		uint32_t s_off = (uint32_t)st->pos, c_off = (uint32_t)dst->len;
		advance(st);
		buf_ch(dst, c);
		span_push(spans, s_off, 1, c_off, 1, passthrough_kind);

		if (c == '(') {
			local_paren++;
			allow_jsx = 1;
		} else if (c == ')') {
			if (local_paren > 0)
				local_paren--;
			allow_jsx = 0;
		} else if (c == '[') {
			local_bracket++;
			allow_jsx = 0;
		} else if (c == ']') {
			if (local_bracket > 0)
				local_bracket--;
			allow_jsx = 0;
		} else if (c == '{') {
			if (stop_ch == '\0')
				st->brace_depth++;
			else
				local_brace++;
			allow_jsx = 0;
		} else if (c == '}') {
			if (stop_ch == '\0') {
				if (st->brace_depth == st->comp_brace_depth)
					clear_comp_params(st);
				if (st->brace_depth > 0)
					st->brace_depth--;
				if (st->brace_depth == 0)
					flush_top_level(st);
			} else if (local_brace > 0) {
				local_brace--;
			}
			allow_jsx = 0;
		} else if (c == ';') {
			if (stop_ch == '\0' && st->brace_depth == 0)
				flush_top_level(st);
			allow_jsx = 0;
		} else if (c == '=' || c == ',' || c == '?' || c == ':') {
			allow_jsx = 1;
		} else {
			allow_jsx = 0;
		}
	}
}

/* ── Public API ───────────────────────────────────────────────────────── */

int cx_transpile(const cx_options *opt, const char *src, size_t len, cx_result *out)
{
	if (!src || !out)
		return -1;
	memset(out, 0, sizeof(*out));

	cx_state st;
	memset(&st, 0, sizeof(st));
	st.src = src;
	st.len = len;
	st.pos = 0;
	st.line = 1;
	st.col = 1;
	st.filename = (opt && opt->filename) ? opt->filename : "<input>.cx";
	st.emit_lines = opt ? opt->emit_lines : 1;
	st.recover = opt ? opt->recover : 0;
	st.comp_brace_depth = -1;

	if (st.emit_lines)
		emit_line_directive(&st, &st.out, 1);

	transpile_c_until(&st, &st.body, &st.body_spans, '\0', 0, CX_SPAN_PASSTHROUGH);
	flush_top_level(&st);
	clear_comp_params(&st);

	if (st.out.oom || st.hoist.oom || st.body.oom ||
	    st.out_spans.oom || st.hoist_spans.oom || st.body_spans.oom ||
	    st.diags.oom)
	{
		buf_free(&st.out);
		buf_free(&st.hoist);
		buf_free(&st.body);
		span_list_free(&st.out_spans);
		span_list_free(&st.hoist_spans);
		span_list_free(&st.body_spans);
		free(st.diags.items);
		return -1;
	}

	buf_free(&st.hoist);
	buf_free(&st.body);
	span_list_free(&st.hoist_spans);
	span_list_free(&st.body_spans);

	if (!st.out.data) {
		st.out.data = cx_strndup("", 0);
		if (!st.out.data)
			return -1;
	}

	out->c_code = st.out.data;
	out->c_len = st.out.len;
	out->spans = st.out_spans.items;
	out->span_count = st.out_spans.count;
	out->diags = st.diags.items;
	out->diag_count = st.diags.count;
	out->ok = (st.diags.count == 0) ? 1 : 0;
	return 0;
}

void cx_result_free(cx_result *res)
{
	if (!res)
		return;
	free(res->c_code);
	free(res->spans);
	free(res->diags);
	memset(res, 0, sizeof(*res));
}

int cx_write_map_json(const cx_result *res, const char *path)
{
	if (!res || !path)
		return -1;
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return -1;
	fprintf(fp, "{\"version\":1,\"spans\":[");
	for (size_t i = 0; i < res->span_count; i++) {
		const cx_span *s = &res->spans[i];
		if (i > 0)
			fputc(',', fp);
		fprintf(fp, "{\"cx\":[%u,%u],\"c\":[%u,%u],\"k\":%u}",
		        (unsigned)s->cx_off, (unsigned)s->cx_len,
		        (unsigned)s->c_off, (unsigned)s->c_len,
		        (unsigned)s->kind);
	}
	fprintf(fp, "]}\n");
	fclose(fp);
	return 0;
}

int cx_map_cx_to_c(
        const cx_result *res, uint32_t cx_off,
        uint32_t *out_c_off, uint8_t *out_kind)
{
	if (!res || !res->spans || res->span_count == 0)
		return 0;

	const cx_span *best = NULL;
	const cx_span *edge = NULL;
	for (size_t i = 0; i < res->span_count; i++) {
		const cx_span *s = &res->spans[i];
		if (cx_off >= s->cx_off && cx_off < s->cx_off + s->cx_len) {
			if (!best || s->cx_len < best->cx_len)
				best = s;
		} else if (cx_off == s->cx_off + s->cx_len) {
			if (!edge || s->cx_len < edge->cx_len)
				edge = s;
		}
	}
	if (!best)
		best = edge;
	if (!best)
		return 0;

	uint32_t delta = cx_off - best->cx_off;
	if (delta > best->c_len)
		delta = best->c_len;
	if (out_c_off)
		*out_c_off = best->c_off + delta;
	if (out_kind)
		*out_kind = best->kind;
	return 1;
}

int cx_map_c_to_cx(
        const cx_result *res, uint32_t c_off,
        uint32_t *out_cx_off, uint8_t *out_kind)
{
	if (!res || !res->spans || res->span_count == 0)
		return 0;

	const cx_span *best = NULL;
	const cx_span *edge = NULL;
	for (size_t i = 0; i < res->span_count; i++) {
		const cx_span *s = &res->spans[i];
		if (c_off >= s->c_off && c_off < s->c_off + s->c_len) {
			if (!best || s->c_len < best->c_len)
				best = s;
		} else if (c_off == s->c_off + s->c_len) {
			if (!edge || s->c_len < edge->c_len)
				edge = s;
		}
	}
	if (!best)
		best = edge;
	if (!best)
		return 0;

	uint32_t delta = c_off - best->c_off;
	if (delta > best->cx_len)
		delta = best->cx_len;
	if (out_cx_off)
		*out_cx_off = best->cx_off + delta;
	if (out_kind)
		*out_kind = best->kind;
	return 1;
}

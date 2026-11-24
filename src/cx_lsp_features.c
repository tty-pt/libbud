/**
 * @file cx_lsp_features.c
 * @brief First-class `.cx` LSP features: JSX/cross-file completion, native hover,
 *        diagnostic translation + `.cx` lints, cross-file definition, documentSymbol,
 *        foldingRange, linkedEditingRange, safe rename, and smart codeAction fixes.
 */

#define _POSIX_C_SOURCE 200809L

#include "cx_lsp_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Small Lexing & Slice Helpers ─────────────────────────────────────── */

static int is_id_start(char c)
{
	return isalpha((unsigned char)c) || c == '_';
}

static int is_id_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static int is_jsx_attr_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':';
}

static int slice_contains(cx_slice sl, uint32_t off)
{
	return sl.len > 0 && off >= sl.off && off <= sl.off + sl.len;
}

static int slice_eq_str(const char *src, cx_slice sl, const char *s)
{
	size_t n = strlen(s);
	return sl.len == n && memcmp(src + sl.off, s, n) == 0;
}

static int slice_eq_slice(const char *s1, cx_slice a, const char *s2, cx_slice b)
{
	return a.len == b.len && memcmp(s1 + a.off, s2 + b.off, a.len) == 0;
}

static const char *path_basename(const char *p)
{
	if (!p)
		return "";
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static int pfx_match(const char *cand, const char *pfx)
{
	if (!pfx || !pfx[0])
		return 1;
	size_t plen = strlen(pfx);
	for (size_t i = 0; i < plen; i++) {
		if (!cand[i] || tolower((unsigned char)cand[i]) != tolower((unsigned char)pfx[i]))
			return 0;
	}
	return 1;
}

static int pfx_match_n(const char *cand, size_t clen, const char *pfx)
{
	if (!pfx || !pfx[0])
		return 1;
	size_t plen = strlen(pfx);
	if (clen < plen)
		return 0;
	for (size_t i = 0; i < plen; i++) {
		if (tolower((unsigned char)cand[i]) != tolower((unsigned char)pfx[i]))
			return 0;
	}
	return 1;
}

static jval *make_text_edit(jval *range, const char *new_text)
{
	jval *ed = jval_new(J_OBJ);
	jval_set(ed, "range", range);
	jval_set(ed, "newText", jval_new_str(new_text ? new_text : ""));
	return ed;
}

static void ws_edit_add(jval *ws_edit, const char *uri, jval *edit)
{
	jval *changes = jval_get(ws_edit, "changes");
	if (!changes || changes->kind != J_OBJ) {
		changes = jval_new(J_OBJ);
		jval_set(ws_edit, "changes", changes);
	}
	jval *arr = jval_get(changes, uri);
	if (!arr || arr->kind != J_ARR) {
		arr = jval_new(J_ARR);
		jval_set(changes, uri, arr);
	}
	jval_push(arr, edit);
}

static char *clean_doc_slice(const char *src, cx_slice sl)
{
	if (sl.len == 0)
		return NULL;
	lsp_buf b = { 0 };
	const char *p = src + sl.off;
	const char *end = p + sl.len;
	while (p < end) {
		const char *ls = p;
		while (p < end && *p != '\n')
			p++;
		const char *le = p;
		if (p < end && *p == '\n')
			p++;
		while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r'))
			ls++;
		while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r'))
			le--;
		if (le - ls >= 3 && memcmp(ls, "///", 3) == 0) ls += 3;
		else if (le - ls >= 3 && memcmp(ls, "/**", 3) == 0) ls += 3;
		else if (le - ls >= 2 && memcmp(ls, "/*", 2) == 0) ls += 2;
		else if (ls < le && *ls == '*') {
			ls += (le - ls >= 2 && ls[1] == '/') ? 2 : 1;
		}
		if (le - ls >= 2 && le[-2] == '*' && le[-1] == '/')
			le -= 2;
		while (ls < le && (*ls == ' ' || *ls == '\t'))
			ls++;
		while (le > ls && (le[-1] == ' ' || le[-1] == '\t'))
			le--;
		if (le > ls) {
			if (b.len > 0)
				lbuf_ch(&b, '\n');
			lbuf_append(&b, ls, (size_t)(le - ls));
		}
	}
	return b.data;
}

/* ── JSX Cursor Classification & Completion (Buckets 1 & 2) ───────────── */

typedef enum {
	CTX_C = 0,
	CTX_JSX_TAG,
	CTX_JSX_ATTR
} jsx_cursor_ctx;

typedef struct {
	jsx_cursor_ctx kind;
	char           tag[128];
	char           prefix[128];
	int            is_component;
} jsx_ctx_info;

static void classify_jsx_cursor(const char *src, size_t len, uint32_t cx_off, jsx_ctx_info *out)
{
	memset(out, 0, sizeof(*out));
	out->kind = CTX_C;
	if (!src || cx_off > len)
		return;

	int brace_depth = 0;
	int in_str = 0;
	size_t win_start = (cx_off > 4096) ? (cx_off - 4096) : 0;
	while (win_start > 0 && src[win_start - 1] != '\n')
		win_start--;

	size_t open_lt = (size_t)-1;
	int    lt_brace_depth = 0;

	size_t i = win_start;
	while (i < cx_off) {
		char c = src[i];
		if (c == '/' && i + 1 < cx_off && src[i + 1] == '/') {
			while (i < cx_off && src[i] != '\n')
				i++;
			continue;
		}
		if (c == '/' && i + 1 < cx_off && src[i + 1] == '*') {
			i += 2;
			while (i + 1 < cx_off && !(src[i] == '*' && src[i + 1] == '/'))
				i++;
			if (i + 1 < cx_off)
				i += 2;
			continue;
		}
		if (c == '"' || c == '\'') {
			char q = src[i++];
			in_str = 1;
			while (i < cx_off) {
				if (src[i] == '\\' && i + 1 < cx_off) {
					i += 2;
					continue;
				}
				if (src[i++] == q) {
					in_str = 0;
					break;
				}
			}
			continue;
		}
		if (c == '{') {
			brace_depth++;
			i++;
			continue;
		}
		if (c == '}') {
			if (brace_depth > 0)
				brace_depth--;
			i++;
			continue;
		}
		if (c == '<') {
			if (i + 1 == cx_off || is_id_start(src[i + 1])) {
				size_t prev = i;
				while (prev > win_start && isspace((unsigned char)src[prev - 1]))
					prev--;
				int looks_cmp = 0;
				if (prev > win_start) {
					char pc = src[prev - 1];
					if (pc == ')' || pc == ']' || isdigit((unsigned char)pc))
						looks_cmp = 1;
					else if (is_id_char(pc)) {
						size_t ws = prev;
						while (ws > win_start && is_id_char(src[ws - 1]))
							ws--;
						size_t wl = prev - ws;
						if (!(wl == 6 && memcmp(src + ws, "return", 6) == 0)) {
							looks_cmp = 1;
							for (size_t k = prev; k > win_start; k--) {
								if (src[k - 1] == '>') {
									looks_cmp = 0;
									break;
								}
								if (src[k - 1] == ';' || src[k - 1] == '(')
									break;
							}
						}
					}
				}
				if (!looks_cmp) {
					open_lt = i;
					lt_brace_depth = brace_depth;
				}
			}
			i++;
			continue;
		}
		if (c == '>' && open_lt != (size_t)-1 && brace_depth == lt_brace_depth) {
			if (!(i > win_start && src[i - 1] == '-'))
				open_lt = (size_t)-1;
		}
		i++;
	}

	if (in_str || open_lt == (size_t)-1 || brace_depth != lt_brace_depth)
		return;

	size_t p = open_lt + 1;
	size_t tag_s = p;
	while (p < cx_off && is_jsx_attr_char(src[p]))
		p++;
	size_t tag_e = p;

	if (p == cx_off) {
		out->kind = CTX_JSX_TAG;
		size_t plen = tag_e - tag_s;
		if (plen >= sizeof(out->prefix))
			plen = sizeof(out->prefix) - 1;
		memcpy(out->prefix, src + tag_s, plen);
		out->prefix[plen] = '\0';
		return;
	}

	if (tag_e == tag_s || !isspace((unsigned char)src[p]))
		return;

	size_t tlen = tag_e - tag_s;
	if (tlen >= sizeof(out->tag))
		tlen = sizeof(out->tag) - 1;
	memcpy(out->tag, src + tag_s, tlen);
	out->tag[tlen] = '\0';
	out->is_component = isupper((unsigned char)out->tag[0]);

	size_t ps = cx_off;
	while (ps > tag_e && is_jsx_attr_char(src[ps - 1]))
		ps--;
	size_t before = ps;
	while (before > tag_e && isspace((unsigned char)src[before - 1]))
		before--;
	if (before > tag_e && src[before - 1] == '=')
		return;

	out->kind = CTX_JSX_ATTR;
	size_t plen = cx_off - ps;
	if (plen >= sizeof(out->prefix))
		plen = sizeof(out->prefix) - 1;
	memcpy(out->prefix, src + ps, plen);
	out->prefix[plen] = '\0';
}

static const char *const k_html_tags[] = {
	"div", "span", "button", "p", "a", "ul", "ol", "li", "h1", "h2", "h3", "h4",
	"section", "header", "footer", "nav", "main", "article", "aside",
	"form", "input", "label", "textarea", "select", "option",
	"table", "thead", "tbody", "tr", "th", "td",
	"img", "canvas", "pre", "code", "strong", "em", "small", "hr", "br"
};

static const char *const k_html_attrs[] = {
	"class", "id", "style", "key", "href", "src", "alt", "title", "type",
	"value", "name", "placeholder", "disabled", "checked", "selected",
	"for", "action", "method", "role", "tabindex", "aria-label",
	"onClick", "onInput", "onChange", "onSubmit", "onKeyDown", "onKeyUp",
	"onFocus", "onBlur", "onMouseDown", "onMouseUp", "onMouseEnter", "onMouseLeave"
};

static jval *make_comp_item(const char *label, int kind, const char *detail)
{
	jval *it = jval_new(J_OBJ);
	jval_set(it, "label", jval_new_str(label));
	jval_set(it, "kind", jval_new_int(kind));
	if (detail && detail[0])
		jval_set(it, "detail", jval_new_str(detail));
	return it;
}

static int items_has_label(const jval *items, const char *label, size_t llen)
{
	for (size_t i = 0; i < items->arr.len; i++) {
		const jval *lv = jval_get(items->arr.items[i], "label");
		if (lv && lv->kind == J_STR && lv->slen == llen &&
		    memcmp(lv->str, label, llen) == 0)
		{
			return 1;
		}
	}
	return 0;
}

jval *cx_feat_completion(const lsp_server *srv, const cx_doc *d, long long line, long long col)
{
	uint32_t cx_off = line_index_pos_to_off(&d->cx_lines, d->cx_text, d->cx_len, line, col);
	jsx_ctx_info ctx;
	classify_jsx_cursor(d->cx_text, d->cx_len, cx_off, &ctx);
	if (ctx.kind == CTX_C)
		return NULL;

	jval *items = jval_new(J_ARR);
	if (ctx.kind == CTX_JSX_TAG) {
		/* 1. Current document components */
		for (size_t i = 0; i < d->trans.comp_count; i++) {
			const cx_comp_meta *cm = &d->trans.comps[i];
			const char *nm = d->cx_text + cm->name.off;
			if (pfx_match_n(nm, cm->name.len, ctx.prefix) &&
			    !items_has_label(items, nm, cm->name.len))
			{
				char *label = xstrndup(nm, cm->name.len);
				jval_push(items, make_comp_item(label, 7 /* Class */, "cx component"));
				free(label);
			}
		}
		/* 2. Other open documents (non-static) */
		if (srv) {
			for (size_t di = 0; di < srv->doc_count; di++) {
				const cx_doc *od = srv->docs[di];
				if (od == d || !od->cx_text)
					continue;
				for (size_t i = 0; i < od->trans.comp_count; i++) {
					const cx_comp_meta *cm = &od->trans.comps[i];
					if (cm->is_static)
						continue;
					const char *nm = od->cx_text + cm->name.off;
					if (pfx_match_n(nm, cm->name.len, ctx.prefix) &&
					    !items_has_label(items, nm, cm->name.len))
					{
						char *label = xstrndup(nm, cm->name.len);
						char det[192];
						snprintf(det, sizeof(det), "cx component (%s)",
						         path_basename(od->cx_path));
						jval_push(items, make_comp_item(label, 7, det));
						free(label);
					}
				}
			}
			/* 3. Workspace indexed files (non-static) */
			for (size_t fi = 0; fi < srv->ws.file_count; fi++) {
				const ws_file *wf = &srv->ws.files[fi];
				if (find_doc_by_path(srv, wf->path))
					continue;
				for (size_t i = 0; i < wf->comp_count; i++) {
					const ws_comp *wc = &wf->comps[i];
					if (wc->is_static)
						continue;
					if (pfx_match(wc->name, ctx.prefix) &&
					    !items_has_label(items, wc->name, wc->name_len))
					{
						char det[192];
						snprintf(det, sizeof(det), "cx component (%s)",
						         path_basename(wf->path));
						jval_push(items, make_comp_item(wc->name, 7, det));
					}
				}
			}
		}
		/* 4. HTML / native tags */
		for (size_t i = 0; i < sizeof(k_html_tags) / sizeof(k_html_tags[0]); i++) {
			if (pfx_match(k_html_tags[i], ctx.prefix))
				jval_push(items, make_comp_item(k_html_tags[i], 14 /* Keyword */, "JSX element"));
		}
	} else if (ctx.kind == CTX_JSX_ATTR) {
		if (ctx.is_component) {
			ws_comp_hit hit;
			if (ws_find_comp(srv, d, ctx.tag, strlen(ctx.tag), &hit)) {
				if (hit.doc && hit.doc_comp) {
					const cx_comp_meta *cm = hit.doc_comp;
					for (uint32_t p = 0; p < cm->param_count; p++) {
						const cx_comp_param_meta *pm = &hit.doc->trans.comp_params[cm->param_start + p];
						const char *pn = hit.doc->cx_text + pm->name.off;
						if (pfx_match_n(pn, pm->name.len, ctx.prefix)) {
							char *pname = xstrndup(pn, pm->name.len);
							char *pdecl = xstrndup(hit.doc->cx_text + pm->decl.off, pm->decl.len);
							jval_push(items, make_comp_item(pname, 5 /* Field */, pdecl));
							free(pname);
							free(pdecl);
						}
					}
				} else if (hit.ws_comp) {
					for (size_t p = 0; p < hit.ws_comp->prop_count; p++) {
						if (pfx_match(hit.ws_comp->prop_names[p], ctx.prefix)) {
							jval_push(items, make_comp_item(
							        hit.ws_comp->prop_names[p], 5,
							        hit.ws_comp->prop_decls[p]));
						}
					}
				}
			}
			if (pfx_match("key", ctx.prefix))
				jval_push(items, make_comp_item("key", 5, "reconciler key"));
		} else {
			for (size_t i = 0; i < sizeof(k_html_attrs) / sizeof(k_html_attrs[0]); i++) {
				if (pfx_match(k_html_attrs[i], ctx.prefix)) {
					int is_ev = (strncmp(k_html_attrs[i], "on", 2) == 0 &&
					             isupper((unsigned char)k_html_attrs[i][2]));
					jval_push(items, make_comp_item(
					        k_html_attrs[i],
					        is_ev ? 23 /* Event */ : 10 /* Property */,
					        is_ev ? "bud event handler" : "element attribute"));
				}
			}
		}
	}

	jval *list = jval_new(J_OBJ);
	jval_set(list, "isIncomplete", jval_new_bool(0));
	jval_set(list, "items", items);
	return list;
}

/* ── Native Hover (Bucket 1) ──────────────────────────────────────────── */

static jval *make_markdown_hover(const char *md, jval *range)
{
	jval *contents = jval_new(J_OBJ);
	jval_set(contents, "kind", jval_new_str("markdown"));
	jval_set(contents, "value", jval_new_str(md));
	jval *h = jval_new(J_OBJ);
	jval_set(h, "contents", contents);
	if (range)
		jval_set(h, "range", range);
	return h;
}

static jval *build_comp_hover(const ws_comp_hit *hit, jval *range)
{
	lsp_buf md = { 0 };
	lbuf_str(&md, "```c\n");
	if (hit->doc && hit->doc_comp) {
		lbuf_append(&md, hit->doc->cx_text + hit->doc_comp->decl.off,
		            hit->doc_comp->decl.len);
	} else if (hit->ws_comp && hit->ws_comp->decl_sig) {
		lbuf_str(&md, hit->ws_comp->decl_sig);
	} else {
		lbuf_printf(&md, "component %.*s(...)", (int)hit->name_len, hit->name);
	}
	lbuf_str(&md, "\n```");

	char *doc_tmp = NULL;
	const char *doc_str = NULL;
	if (hit->doc && hit->doc_comp && hit->doc_comp->doc.len > 0) {
		doc_tmp = clean_doc_slice(hit->doc->cx_text, hit->doc_comp->doc);
		doc_str = doc_tmp;
	} else if (hit->ws_comp && hit->ws_comp->doc) {
		doc_str = hit->ws_comp->doc;
	}
	if (doc_str && doc_str[0]) {
		lbuf_str(&md, "\n\n");
		lbuf_str(&md, doc_str);
	}
	if (hit->path && hit->path[0]) {
		lbuf_printf(&md, "\n\n*Defined in `%s`*", path_basename(hit->path));
	}
	free(doc_tmp);

	jval *h = make_markdown_hover(md.data ? md.data : "", range);
	lbuf_free(&md);
	return h;
}

static int find_comp_prop_decl(
        const ws_comp_hit *hit, const char *pname, size_t plen,
        char **out_decl)
{
	*out_decl = NULL;
	if (hit->doc && hit->doc_comp) {
		const cx_comp_meta *cm = hit->doc_comp;
		for (uint32_t p = 0; p < cm->param_count; p++) {
			const cx_comp_param_meta *pm = &hit->doc->trans.comp_params[cm->param_start + p];
			if (pm->name.len == plen &&
			    memcmp(hit->doc->cx_text + pm->name.off, pname, plen) == 0)
			{
				*out_decl = xstrndup(hit->doc->cx_text + pm->decl.off, pm->decl.len);
				return 1;
			}
		}
	} else if (hit->ws_comp) {
		for (size_t p = 0; p < hit->ws_comp->prop_count; p++) {
			if (strlen(hit->ws_comp->prop_names[p]) == plen &&
			    memcmp(hit->ws_comp->prop_names[p], pname, plen) == 0)
			{
				*out_decl = xstrndup(hit->ws_comp->prop_decls[p],
				                     strlen(hit->ws_comp->prop_decls[p]));
				return 1;
			}
		}
	}
	return 0;
}

jval *cx_feat_hover(const lsp_server *srv, const cx_doc *d, long long line, long long col)
{
	if (!d || !d->cx_text)
		return NULL;
	uint32_t cx_off = line_index_pos_to_off(&d->cx_lines, d->cx_text, d->cx_len, line, col);

	/* 1. Component declaration name */
	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		if (slice_contains(cm->name, cx_off)) {
			ws_comp_hit hit;
			if (ws_find_comp(srv, d, d->cx_text + cm->name.off, cm->name.len, &hit)) {
				return build_comp_hover(&hit,
				        cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, cm->name));
			}
		}
	}

	/* 2. Innermost JSX element tag / close_tag / attribute name */
	for (size_t i = d->trans.elem_count; i-- > 0; ) {
		const cx_elem_meta *el = &d->trans.elems[i];
		if (slice_contains(el->tag, cx_off) || slice_contains(el->close_tag, cx_off)) {
			cx_slice hit_sl = slice_contains(el->tag, cx_off) ? el->tag : el->close_tag;
			jval *rng = cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, hit_sl);
			if (el->is_component) {
				ws_comp_hit hit;
				if (ws_find_comp(srv, d, d->cx_text + el->tag.off, el->tag.len, &hit))
					return build_comp_hover(&hit, rng);
				char msg[256];
				snprintf(msg, sizeof(msg), "```c\ncomponent %.*s\n```\n*(unresolved component)*",
				         (int)el->tag.len, d->cx_text + el->tag.off);
				return make_markdown_hover(msg, rng);
			}
			char msg[192];
			snprintf(msg, sizeof(msg), "```html\n<%.*s>\n```\nNative/HTML `bud_element` node",
			         (int)el->tag.len, d->cx_text + el->tag.off);
			return make_markdown_hover(msg, rng);
		}

		for (uint32_t a = 0; a < el->attr_count; a++) {
			const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
			if (!slice_contains(at->name, cx_off))
				continue;
			jval *rng = cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, at->name);
			if (slice_eq_str(d->cx_text, at->name, "key")) {
				return make_markdown_hover(
				        "```c\nkey\n```\nReconciler key for pointer-stable minimal-move LIS diffing.",
				        rng);
			}
			if (el->is_component) {
				ws_comp_hit hit;
				if (ws_find_comp(srv, d, d->cx_text + el->tag.off, el->tag.len, &hit)) {
					char *pdecl = NULL;
					if (find_comp_prop_decl(&hit, d->cx_text + at->name.off,
					                        at->name.len, &pdecl))
					{
						lsp_buf md = { 0 };
						lbuf_printf(&md, "```c\n(prop) %s\n```\nProp of `component %.*s`",
						            pdecl, (int)el->tag.len, d->cx_text + el->tag.off);
						free(pdecl);
						jval *h = make_markdown_hover(md.data ? md.data : "", rng);
						lbuf_free(&md);
						return h;
					}
					if (slice_eq_str(d->cx_text, at->name, "children")) {
						return make_markdown_hover(
						        "```c\n(prop) bud_node *children\n```\nJSX children fragment passed to component.",
						        rng);
					}
				}
				jval_free(rng);
				return NULL;
			}
			if (at->name.len >= 3 && d->cx_text[at->name.off] == 'o' &&
			    d->cx_text[at->name.off + 1] == 'n' &&
			    isupper((unsigned char)d->cx_text[at->name.off + 2]))
			{
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "```c\n%.*s={[captures](bud_event *e) { ... }}\n```\n`bud` event handler binding (`bud_bind_ctx`)",
				         (int)at->name.len, d->cx_text + at->name.off);
				return make_markdown_hover(msg, rng);
			}
			char msg[192];
			snprintf(msg, sizeof(msg), "```html\n%.*s\n```\nAttribute on `<%.*s>`",
			         (int)at->name.len, d->cx_text + at->name.off,
			         (int)el->tag.len, d->cx_text + el->tag.off);
			return make_markdown_hover(msg, rng);
		}
	}

	return NULL;
}

void cx_feat_sanitize_hover(jval *hover_res)
{
	if (!hover_res || hover_res->kind != J_OBJ)
		return;
	jval *contents = jval_get(hover_res, "contents");
	if (!contents || contents->kind != J_OBJ)
		return;
	jval *val = jval_get(contents, "value");
	if (!val || val->kind != J_STR || !val->str)
		return;
	/* Replace `__cx_p->` or `_props` artifacts if clangd mentions them */
	if (strstr(val->str, "__cx_") == NULL)
		return;
	lsp_buf b = { 0 };
	const char *p = val->str;
	while (*p) {
		if (strncmp(p, "__cx_p->", 8) == 0) {
			p += 8;
		} else {
			lbuf_ch(&b, *p++);
		}
	}
	if (b.data) {
		jval_set(contents, "value", jval_new_strn(b.data, b.len));
		lbuf_free(&b);
	}
}

/* ── Diagnostic Translation & `.cx`-Native Lints (Bucket 1) ───────────── */

static int extract_quoted(const char *s, char *out, size_t out_sz)
{
	const char *q1 = strchr(s, '\'');
	if (!q1)
		return 0;
	q1++;
	const char *q2 = strchr(q1, '\'');
	if (!q2)
		return 0;
	size_t n = (size_t)(q2 - q1);
	if (n == 0 || n >= out_sz)
		return 0;
	memcpy(out, q1, n);
	out[n] = '\0';
	return 1;
}

static void push_cx_diag(
        jval *diags_arr, const cx_doc *d, cx_slice sl,
        int severity, const char *code, const char *msg)
{
	jval *diag = jval_new(J_OBJ);
	jval_set(diag, "range", cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, sl));
	jval_set(diag, "severity", jval_new_int(severity));
	jval_set(diag, "source", jval_new_str("cx"));
	if (code && code[0])
		jval_set(diag, "code", jval_new_str(code));
	jval_set(diag, "message", jval_new_str(msg));
	jval_push(diags_arr, diag);
}

static int diags_has_at_line(const jval *diags_arr, long long line, const char *code)
{
	for (size_t i = 0; i < diags_arr->arr.len; i++) {
		const jval *dg = diags_arr->arr.items[i];
		const jval *rng = jval_get(dg, "range");
		const jval *st = rng ? jval_get(rng, "start") : NULL;
		const char *c = jval_as_str(jval_get(dg, "code"));
		if (st && jval_as_int(jval_get(st, "line"), -1) == line &&
		    c && strcmp(c, code) == 0)
		{
			return 1;
		}
	}
	return 0;
}

void cx_feat_process_diags(const lsp_server *srv, const cx_doc *d, jval *diags_arr)
{
	if (!d || !diags_arr || diags_arr->kind != J_ARR)
		return;

	/* 1. Translate shadow-C identifiers inside clangd diagnostics into `.cx` terms */
	for (size_t i = 0; i < diags_arr->arr.len; i++) {
		jval *dg = diags_arr->arr.items[i];
		const char *msg = jval_as_str(jval_get(dg, "message"));
		if (!msg)
			continue;

		/* `field designator 'foo' does not refer to any field in type 'Bar_props'`
		 * or `no member named 'foo' in 'struct Bar_props'` */
		const char *props_tok = strstr(msg, "_props");
		if (props_tok && (strstr(msg, "no member named") || strstr(msg, "does not refer to any field"))) {
			char prop[128] = { 0 };
			extract_quoted(msg, prop, sizeof(prop));
			const char *cname_e = props_tok;
			const char *cname_s = cname_e;
			while (cname_s > msg && is_id_char(cname_s[-1]))
				cname_s--;
			if (prop[0] && cname_e > cname_s) {
				char clean[256];
				snprintf(clean, sizeof(clean), "unknown prop '%s' on component <%.*s>",
				         prop, (int)(cname_e - cname_s), cname_s);
				jval_set(dg, "message", jval_new_str(clean));
				jval_set(dg, "code", jval_new_str("cx-unknown-prop"));
				continue;
			}
		}

		/* `use of undeclared identifier 'Foo_render'` */
		const char *rend_tok = strstr(msg, "_render'");
		if (rend_tok && strstr(msg, "undeclared identifier")) {
			const char *cname_s = rend_tok;
			while (cname_s > msg && is_id_char(cname_s[-1]))
				cname_s--;
			if (rend_tok > cname_s) {
				char clean[256];
				snprintf(clean, sizeof(clean), "unknown component <%.*s>",
				         (int)(rend_tok - cname_s), cname_s);
				jval_set(dg, "message", jval_new_str(clean));
				jval_set(dg, "code", jval_new_str("cx-unknown-component"));
				continue;
			}
		}

		/* `use of undeclared identifier 'Foo_props'` when paired with `_render` */
		if (props_tok && strstr(msg, "undeclared identifier")) {
			const char *cname_s = props_tok;
			while (cname_s > msg && is_id_char(cname_s[-1]))
				cname_s--;
			if (props_tok > cname_s) {
				char clean[256];
				snprintf(clean, sizeof(clean), "unknown component <%.*s>",
				         (int)(props_tok - cname_s), cname_s);
				jval_set(dg, "message", jval_new_str(clean));
				jval_set(dg, "code", jval_new_str("cx-unknown-component"));
			}
		}
	}

	/* 2. Append `.cx` transpiler syntax diagnostics */
	for (size_t i = 0; i < d->trans.diag_count; i++) {
		const cx_diag *cd = &d->trans.diags[i];
		long long line = (cd->line > 0) ? (long long)(cd->line - 1) : 0;
		long long col = (cd->col > 0) ? (long long)(cd->col - 1) : 0;
		jval *diag = jval_new(J_OBJ);
		jval_set(diag, "range", make_lsp_range(line, col, line, col + 1));
		jval_set(diag, "severity", jval_new_int(1));
		jval_set(diag, "source", jval_new_str("cx"));
		jval_set(diag, "code", jval_new_str("cx-syntax"));
		jval_set(diag, "message", jval_new_str(cd->msg));
		jval_push(diags_arr, diag);
	}

	/* 3. `.cx`-native structural lints over flat index */
	for (size_t i = 0; i < d->trans.elem_count; i++) {
		const cx_elem_meta *el = &d->trans.elems[i];

		/* Lint A: element directly inside `{for (...)}` without `key={...}` */
		if (el->in_loop_direct && !el->has_key_attr && el->tag.len > 0) {
			push_cx_diag(diags_arr, d, el->tag, 2 /* Warning */, "cx-missing-key",
			             "Element inside {for (...)} is missing a `key={...}` prop "
			             "(recommended for minimal-move LIS reconciliation)");
		}

		/* Lint B: duplicate attributes on the same element */
		for (uint32_t a = 0; a < el->attr_count; a++) {
			const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
			for (uint32_t b = 0; b < a; b++) {
				const cx_attr_meta *prev = &d->trans.attrs[el->attr_start + b];
				if (slice_eq_slice(d->cx_text, at->name, d->cx_text, prev->name)) {
					char msg[192];
					snprintf(msg, sizeof(msg), "duplicate attribute '%.*s' on <%.*s>",
					         (int)at->name.len, d->cx_text + at->name.off,
					         (int)el->tag.len, d->cx_text + el->tag.off);
					push_cx_diag(diags_arr, d, at->full, 2 /* Warning */,
					             "cx-duplicate-attr", msg);
					break;
				}
			}
		}

		/* Lint C: unknown prop on a known component (deduped against clangd) */
		if (el->is_component && el->tag.len > 0) {
			ws_comp_hit hit;
			if (ws_find_comp(srv, d, d->cx_text + el->tag.off, el->tag.len, &hit)) {
				for (uint32_t a = 0; a < el->attr_count; a++) {
					const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
					if (slice_eq_str(d->cx_text, at->name, "key") ||
					    slice_eq_str(d->cx_text, at->name, "children"))
					{
						continue;
					}
					char *pdecl = NULL;
					int found = find_comp_prop_decl(&hit, d->cx_text + at->name.off,
					                                at->name.len, &pdecl);
					free(pdecl);
					if (!found) {
						long long ln = 0, col = 0;
						line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len,
						                      at->name.off, &ln, &col);
						if (!diags_has_at_line(diags_arr, ln, "cx-unknown-prop")) {
							char msg[192];
							snprintf(msg, sizeof(msg), "unknown prop '%.*s' on component <%.*s>",
							         (int)at->name.len, d->cx_text + at->name.off,
							         (int)el->tag.len, d->cx_text + el->tag.off);
							push_cx_diag(diags_arr, d, at->name, 1 /* Error */,
							             "cx-unknown-prop", msg);
						}
					}
				}
			}
		}
	}
}

/* ── Cross-File Definition & `.cx.h` Remapping (Buckets 1 & 2) ────────── */

static jval *make_location(const char *uri, long long sl, long long sc, long long el, long long ec)
{
	jval *loc = jval_new(J_OBJ);
	jval_set(loc, "uri", jval_new_str(uri));
	jval_set(loc, "range", make_lsp_range(sl, sc, el, ec));
	return loc;
}

jval *cx_feat_definition(const lsp_server *srv, const cx_doc *d, long long line, long long col)
{
	if (!d || !d->cx_text)
		return NULL;
	uint32_t cx_off = line_index_pos_to_off(&d->cx_lines, d->cx_text, d->cx_len, line, col);

	/* 1. Forward component declaration -> jump to real definition across files */
	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		if (cm->is_forward && slice_contains(cm->name, cx_off)) {
			ws_comp_hit hit;
			if (ws_find_comp(srv, d, d->cx_text + cm->name.off, cm->name.len, &hit) &&
			    !hit.is_forward && hit.uri)
			{
				return make_location(hit.uri, hit.line, hit.col,
				                     hit.line, hit.col + hit.name_len);
			}
		}
	}

	/* 2. Component JSX tag / prop name */
	for (size_t i = d->trans.elem_count; i-- > 0; ) {
		const cx_elem_meta *el = &d->trans.elems[i];
		if (!el->is_component || el->tag.len == 0)
			continue;

		if (slice_contains(el->tag, cx_off) || slice_contains(el->close_tag, cx_off)) {
			ws_comp_hit hit;
			if (ws_find_comp(srv, d, d->cx_text + el->tag.off, el->tag.len, &hit) && hit.uri) {
				return make_location(hit.uri, hit.line, hit.col,
				                     hit.line, hit.col + hit.name_len);
			}
			return NULL;
		}

		for (uint32_t a = 0; a < el->attr_count; a++) {
			const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
			if (!slice_contains(at->name, cx_off))
				continue;
			ws_comp_hit hit;
			if (ws_find_comp(srv, d, d->cx_text + el->tag.off, el->tag.len, &hit) && hit.uri) {
				if (hit.doc && hit.doc_comp) {
					for (uint32_t p = 0; p < hit.doc_comp->param_count; p++) {
						const cx_comp_param_meta *pm =
						        &hit.doc->trans.comp_params[hit.doc_comp->param_start + p];
						if (slice_eq_slice(d->cx_text, at->name, hit.doc->cx_text, pm->name)) {
							jval *loc = jval_new(J_OBJ);
							jval_set(loc, "uri", jval_new_str(hit.uri));
							jval_set(loc, "range", cx_slice_to_range(
							        &hit.doc->cx_lines, hit.doc->cx_text,
							        hit.doc->cx_len, pm->name));
							return loc;
						}
					}
				}
				return make_location(hit.uri, hit.line, hit.col,
				                     hit.line, hit.col + hit.name_len);
			}
		}
	}

	return NULL;
}

static void remap_hdr_loc_obj(const lsp_server *srv, jval *obj)
{
	if (!obj || obj->kind != J_OBJ)
		return;
	const char *uri = jval_as_str(jval_get(obj, "uri"));
	if (uri) {
		char *cx_uri = NULL;
		long long ln = 0, col = 0, len = 0;
		if (ws_find_cx_for_generated_header(srv, uri, NULL, &cx_uri, &ln, &col, &len)) {
			jval_set(obj, "uri", jval_new_str(cx_uri));
			jval_set(obj, "range", make_lsp_range(ln, col, ln, col + len));
			free(cx_uri);
		}
	}
	const char *turi = jval_as_str(jval_get(obj, "targetUri"));
	if (turi) {
		char *cx_uri = NULL;
		long long ln = 0, col = 0, len = 0;
		if (ws_find_cx_for_generated_header(srv, turi, NULL, &cx_uri, &ln, &col, &len)) {
			jval_set(obj, "targetUri", jval_new_str(cx_uri));
			jval_set(obj, "targetRange", make_lsp_range(ln, col, ln, col + len));
			jval_set(obj, "targetSelectionRange", make_lsp_range(ln, col, ln, col + len));
			free(cx_uri);
		}
	}
}

void cx_feat_remap_def_result(const lsp_server *srv, const cx_doc *d, jval *res)
{
	(void)d;
	if (!res)
		return;
	if (res->kind == J_OBJ) {
		remap_hdr_loc_obj(srv, res);
	} else if (res->kind == J_ARR) {
		for (size_t i = 0; i < res->arr.len; i++)
			remap_hdr_loc_obj(srv, res->arr.items[i]);
	}
}

/* ── Document Symbols (Bucket 1) ──────────────────────────────────────── */

static int is_synthetic_c_symbol(const char *name)
{
	if (!name || !name[0])
		return 1;
	if (strncmp(name, "__cx_", 5) == 0 || strncmp(name, "_cx_", 4) == 0)
		return 1;
	size_t n = strlen(name);
	if (n > 6 && strcmp(name + n - 6, "_props") == 0)
		return 1;
	if (n > 7 && strcmp(name + n - 7, "_render") == 0)
		return 1;
	return 0;
}

jval *cx_feat_document_symbols(const cx_doc *d)
{
	jval *arr = jval_new(J_ARR);
	if (!d || !d->cx_text)
		return arr;

	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		if (cm->name.len == 0)
			continue;

		cx_slice full = cm->decl;
		if (cm->body.len > 0)
			full.len = (cm->body.off + cm->body.len) - full.off;

		jval *sym = jval_new(J_OBJ);
		jval_set(sym, "name", jval_new_strn(d->cx_text + cm->name.off, cm->name.len));
		jval_set(sym, "detail", jval_new_str(cm->is_forward ? "component decl" : "component"));
		jval_set(sym, "kind", jval_new_int(5 /* Class */));
		jval_set(sym, "range", cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, full));
		jval_set(sym, "selectionRange",
		         cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, cm->name));

		if (cm->param_count > 0) {
			jval *kids = jval_new(J_ARR);
			for (uint32_t p = 0; p < cm->param_count; p++) {
				const cx_comp_param_meta *pm = &d->trans.comp_params[cm->param_start + p];
				jval *psym = jval_new(J_OBJ);
				jval_set(psym, "name", jval_new_strn(d->cx_text + pm->name.off, pm->name.len));
				jval_set(psym, "detail", jval_new_strn(d->cx_text + pm->decl.off, pm->decl.len));
				jval_set(psym, "kind", jval_new_int(8 /* Field */));
				jval_set(psym, "range",
				         cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, pm->decl));
				jval_set(psym, "selectionRange",
				         cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, pm->name));
				jval_push(kids, psym);
			}
			jval_set(sym, "children", kids);
		}
		jval_push(arr, sym);
	}
	return arr;
}

jval *cx_feat_merge_doc_symbols(const cx_doc *d, jval *cx_syms, jval *clangd_syms)
{
	(void)d;
	if (!cx_syms)
		cx_syms = jval_new(J_ARR);
	if (!clangd_syms || clangd_syms->kind != J_ARR)
		return cx_syms;

	for (size_t i = 0; i < clangd_syms->arr.len; i++) {
		jval *sym = clangd_syms->arr.items[i];
		const char *nm = jval_as_str(jval_get(sym, "name"));
		if (is_synthetic_c_symbol(nm))
			continue;
		jval_push(cx_syms, jval_clone(sym));
	}
	return cx_syms;
}

/* ── Folding Ranges & Linked Editing (Bucket 1) ───────────────────────── */

static void push_fold(jval *arr, const cx_doc *d, cx_slice sl)
{
	if (sl.len == 0)
		return;
	long long sln = 0, sc = 0, eln = 0, ec = 0;
	line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len, sl.off, &sln, &sc);
	line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len, sl.off + sl.len, &eln, &ec);
	if (eln > sln) {
		jval *f = jval_new(J_OBJ);
		jval_set(f, "startLine", jval_new_int(sln));
		jval_set(f, "endLine", jval_new_int(eln));
		jval_set(f, "kind", jval_new_str("region"));
		jval_push(arr, f);
	}
}

jval *cx_feat_folding_ranges(const cx_doc *d)
{
	jval *arr = jval_new(J_ARR);
	if (!d || !d->cx_text)
		return arr;
	for (size_t i = 0; i < d->trans.comp_count; i++)
		push_fold(arr, d, d->trans.comps[i].body);
	for (size_t i = 0; i < d->trans.elem_count; i++)
		push_fold(arr, d, d->trans.elems[i].full);
	for (size_t i = 0; i < d->trans.closure_count; i++)
		push_fold(arr, d, d->trans.closures[i].body);
	return arr;
}

jval *cx_feat_linked_editing(const cx_doc *d, long long line, long long col)
{
	if (!d || !d->cx_text)
		return NULL;
	uint32_t cx_off = line_index_pos_to_off(&d->cx_lines, d->cx_text, d->cx_len, line, col);
	for (size_t i = d->trans.elem_count; i-- > 0; ) {
		const cx_elem_meta *el = &d->trans.elems[i];
		if (el->is_self_closing || el->tag.len == 0 || el->close_tag.len == 0)
			continue;
		if (slice_contains(el->tag, cx_off) || slice_contains(el->close_tag, cx_off)) {
			jval *ranges = jval_new(J_ARR);
			jval_push(ranges, cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, el->tag));
			jval_push(ranges, cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, el->close_tag));
			jval *res = jval_new(J_OBJ);
			jval_set(res, "ranges", ranges);
			jval_set(res, "wordPattern", jval_new_str("[A-Za-z_][A-Za-z0-9_:-]*"));
			return res;
		}
	}
	return NULL;
}

/* ── Safe Rename (Bucket 1) ───────────────────────────────────────────── */

static void collect_comp_rename_in_doc(
        jval *ws_edit, const cx_doc *doc,
        const char *old_name, size_t olen, const char *new_name)
{
	for (size_t i = 0; i < doc->trans.comp_count; i++) {
		const cx_comp_meta *cm = &doc->trans.comps[i];
		if (cm->name.len == olen &&
		    memcmp(doc->cx_text + cm->name.off, old_name, olen) == 0)
		{
			ws_edit_add(ws_edit, doc->cx_uri, make_text_edit(
			        cx_slice_to_range(&doc->cx_lines, doc->cx_text, doc->cx_len, cm->name),
			        new_name));
		}
	}
	for (size_t i = 0; i < doc->trans.elem_count; i++) {
		const cx_elem_meta *el = &doc->trans.elems[i];
		if (!el->is_component)
			continue;
		if (el->tag.len == olen &&
		    memcmp(doc->cx_text + el->tag.off, old_name, olen) == 0)
		{
			ws_edit_add(ws_edit, doc->cx_uri, make_text_edit(
			        cx_slice_to_range(&doc->cx_lines, doc->cx_text, doc->cx_len, el->tag),
			        new_name));
		}
		if (el->close_tag.len == olen &&
		    memcmp(doc->cx_text + el->close_tag.off, old_name, olen) == 0)
		{
			ws_edit_add(ws_edit, doc->cx_uri, make_text_edit(
			        cx_slice_to_range(&doc->cx_lines, doc->cx_text, doc->cx_len, el->close_tag),
			        new_name));
		}
	}
}

static void collect_prop_rename_in_doc(
        jval *ws_edit, const cx_doc *doc,
        const char *comp_name, size_t clen,
        const char *old_prop, size_t plen, const char *new_name)
{
	for (size_t i = 0; i < doc->trans.comp_count; i++) {
		const cx_comp_meta *cm = &doc->trans.comps[i];
		if (cm->name.len != clen || memcmp(doc->cx_text + cm->name.off, comp_name, clen) != 0)
			continue;
		for (uint32_t p = 0; p < cm->param_count; p++) {
			const cx_comp_param_meta *pm = &doc->trans.comp_params[cm->param_start + p];
			if (pm->name.len == plen &&
			    memcmp(doc->cx_text + pm->name.off, old_prop, plen) == 0)
			{
				ws_edit_add(ws_edit, doc->cx_uri, make_text_edit(
				        cx_slice_to_range(&doc->cx_lines, doc->cx_text, doc->cx_len, pm->name),
				        new_name));
			}
		}
	}
	for (size_t i = 0; i < doc->trans.elem_count; i++) {
		const cx_elem_meta *el = &doc->trans.elems[i];
		if (!el->is_component || el->tag.len != clen ||
		    memcmp(doc->cx_text + el->tag.off, comp_name, clen) != 0)
		{
			continue;
		}
		for (uint32_t a = 0; a < el->attr_count; a++) {
			const cx_attr_meta *at = &doc->trans.attrs[el->attr_start + a];
			if (at->name.len == plen &&
			    memcmp(doc->cx_text + at->name.off, old_prop, plen) == 0)
			{
				ws_edit_add(ws_edit, doc->cx_uri, make_text_edit(
				        cx_slice_to_range(&doc->cx_lines, doc->cx_text, doc->cx_len, at->name),
				        new_name));
			}
		}
	}
}

jval *cx_feat_prepare_rename(const lsp_server *srv, const cx_doc *d, long long line, long long col)
{
	(void)srv;
	if (!d || !d->cx_text)
		return NULL;
	uint32_t cx_off = line_index_pos_to_off(&d->cx_lines, d->cx_text, d->cx_len, line, col);

	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		if (slice_contains(cm->name, cx_off))
			return cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, cm->name);
		for (uint32_t p = 0; p < cm->param_count; p++) {
			const cx_comp_param_meta *pm = &d->trans.comp_params[cm->param_start + p];
			if (slice_contains(pm->name, cx_off))
				return cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, pm->name);
		}
	}
	for (size_t i = d->trans.elem_count; i-- > 0; ) {
		const cx_elem_meta *el = &d->trans.elems[i];
		if (!el->is_component)
			continue;
		if (slice_contains(el->tag, cx_off))
			return cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, el->tag);
		if (slice_contains(el->close_tag, cx_off))
			return cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, el->close_tag);
		for (uint32_t a = 0; a < el->attr_count; a++) {
			const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
			if (slice_contains(at->name, cx_off) && !slice_eq_str(d->cx_text, at->name, "key"))
				return cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, at->name);
		}
	}
	return NULL;
}

jval *cx_feat_rename(
        const lsp_server *srv, const cx_doc *d,
        long long line, long long col, const char *new_name,
        int *out_merge_clangd)
{
	*out_merge_clangd = 0;
	if (!d || !d->cx_text || !new_name || !new_name[0])
		return NULL;
	uint32_t cx_off = line_index_pos_to_off(&d->cx_lines, d->cx_text, d->cx_len, line, col);

	/* 1. Component name rename (decl or <Comp> / </Comp>) */
	const char *cname = NULL;
	size_t clen = 0;
	int comp_is_static = 0;

	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		if (slice_contains(cm->name, cx_off)) {
			cname = d->cx_text + cm->name.off;
			clen = cm->name.len;
			comp_is_static = cm->is_static;
			break;
		}
	}
	if (!cname) {
		for (size_t i = d->trans.elem_count; i-- > 0; ) {
			const cx_elem_meta *el = &d->trans.elems[i];
			if (!el->is_component)
				continue;
			if (slice_contains(el->tag, cx_off) || slice_contains(el->close_tag, cx_off)) {
				cname = d->cx_text + el->tag.off;
				clen = el->tag.len;
				break;
			}
		}
	}
	if (cname && clen > 0) {
		jval *ws = jval_new(J_OBJ);
		collect_comp_rename_in_doc(ws, d, cname, clen, new_name);
		if (!comp_is_static && srv) {
			for (size_t di = 0; di < srv->doc_count; di++) {
				if (srv->docs[di] != d)
					collect_comp_rename_in_doc(ws, srv->docs[di], cname, clen, new_name);
			}
		}
		return ws;
	}

	/* 2. Component prop rename (param decl or <Comp prop=...>) */
	const char *pcomp = NULL, *pname = NULL;
	size_t pclen = 0, plen = 0;
	int on_param_decl = 0;

	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		for (uint32_t p = 0; p < cm->param_count; p++) {
			const cx_comp_param_meta *pm = &d->trans.comp_params[cm->param_start + p];
			if (slice_contains(pm->name, cx_off)) {
				pcomp = d->cx_text + cm->name.off;
				pclen = cm->name.len;
				pname = d->cx_text + pm->name.off;
				plen = pm->name.len;
				on_param_decl = 1;
				break;
			}
		}
		if (pname)
			break;
	}
	if (!pname) {
		for (size_t i = d->trans.elem_count; i-- > 0; ) {
			const cx_elem_meta *el = &d->trans.elems[i];
			if (!el->is_component)
				continue;
			for (uint32_t a = 0; a < el->attr_count; a++) {
				const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
				if (slice_contains(at->name, cx_off) &&
				    !slice_eq_str(d->cx_text, at->name, "key"))
				{
					pcomp = d->cx_text + el->tag.off;
					pclen = el->tag.len;
					pname = d->cx_text + at->name.off;
					plen = at->name.len;
					break;
				}
			}
			if (pname)
				break;
		}
	}
	if (pcomp && pname) {
		jval *ws = jval_new(J_OBJ);
		collect_prop_rename_in_doc(ws, d, pcomp, pclen, pname, plen, new_name);
		if (srv) {
			for (size_t di = 0; di < srv->doc_count; di++) {
				if (srv->docs[di] != d)
					collect_prop_rename_in_doc(ws, srv->docs[di],
					                           pcomp, pclen, pname, plen, new_name);
			}
		}
		*out_merge_clangd = on_param_decl;
		return ws;
	}

	return NULL;
}

static int edits_overlap_same_range(const jval *arr, const jval *ed)
{
	const jval *r1 = jval_get(ed, "range");
	const jval *s1 = r1 ? jval_get(r1, "start") : NULL;
	if (!s1)
		return 0;
	long long l1 = jval_as_int(jval_get(s1, "line"), -1);
	long long c1 = jval_as_int(jval_get(s1, "character"), -1);
	for (size_t i = 0; i < arr->arr.len; i++) {
		const jval *r2 = jval_get(arr->arr.items[i], "range");
		const jval *s2 = r2 ? jval_get(r2, "start") : NULL;
		if (s2 && jval_as_int(jval_get(s2, "line"), -2) == l1 &&
		    jval_as_int(jval_get(s2, "character"), -2) == c1)
		{
			return 1;
		}
	}
	return 0;
}

/* Filter WorkspaceEdit returned by clangd so unmapped shadow-C edits are never applied to `.cx`. */
void cx_feat_filter_clangd_rename(const lsp_server *srv, jval *ws_edit)
{
	if (!srv || !ws_edit || ws_edit->kind != J_OBJ)
		return;
	jval *changes = jval_get(ws_edit, "changes");
	if (!changes || changes->kind != J_OBJ)
		return;

	for (size_t i = 0; i < changes->obj.len; i++) {
		const char *uri = changes->obj.keys[i];
		const cx_doc *d = find_doc_by_c_uri(srv, uri);
		if (!d)
			continue;
		jval *edits = changes->obj.vals[i];
		if (!edits || edits->kind != J_ARR)
			continue;

		jval *kept = jval_new(J_ARR);
		for (size_t e = 0; e < edits->arr.len; e++) {
			jval *ed = edits->arr.items[e];
			jval *rng = jval_get(ed, "range");
			if (!cx_c_range_is_mapped(d, rng))
				continue;
			jval *cl = jval_clone(ed);
			remap_json_c_to_cx(srv, cl, d);
			if (!edits_overlap_same_range(kept, cl))
				jval_push(kept, cl);
			else
				jval_free(cl);
		}
		free(changes->obj.keys[i]);
		changes->obj.keys[i] = xstrndup(d->cx_uri, strlen(d->cx_uri));
		jval_free(changes->obj.vals[i]);
		changes->obj.vals[i] = kept;
	}
}

void cx_feat_merge_workspace_edits(jval *dst_ws_edit, jval *src_ws_edit)
{
	if (!dst_ws_edit || !src_ws_edit || src_ws_edit->kind != J_OBJ)
		return;
	jval *src_ch = jval_get(src_ws_edit, "changes");
	if (!src_ch || src_ch->kind != J_OBJ)
		return;
	for (size_t i = 0; i < src_ch->obj.len; i++) {
		const char *uri = src_ch->obj.keys[i];
		jval *edits = src_ch->obj.vals[i];
		if (!edits || edits->kind != J_ARR)
			continue;
		jval *dst_ch = jval_get(dst_ws_edit, "changes");
		jval *dst_arr = (dst_ch && dst_ch->kind == J_OBJ) ? jval_get(dst_ch, uri) : NULL;
		for (size_t e = 0; e < edits->arr.len; e++) {
			if (!dst_arr || !edits_overlap_same_range(dst_arr, edits->arr.items[e])) {
				ws_edit_add(dst_ws_edit, uri, jval_clone(edits->arr.items[e]));
				dst_ch = jval_get(dst_ws_edit, "changes");
				dst_arr = jval_get(dst_ch, uri);
			}
		}
	}
}

/* ── Smart `.cx` Quick-Fixes (`textDocument/codeAction`, Bucket 3) ────── */

static jval *make_quickfix(const char *title, int preferred, jval *ws_edit, const jval *diag)
{
	jval *act = jval_new(J_OBJ);
	jval_set(act, "title", jval_new_str(title));
	jval_set(act, "kind", jval_new_str("quickfix"));
	if (preferred)
		jval_set(act, "isPreferred", jval_new_bool(1));
	if (diag) {
		jval *darr = jval_new(J_ARR);
		jval_push(darr, jval_clone(diag));
		jval_set(act, "diagnostics", darr);
	}
	jval_set(act, "edit", ws_edit);
	return act;
}

/* Infer capture snippet (`name` for comp param, `T *name` for bud_signal, or `T name` for local). */
static int infer_capture_decl(
        const cx_doc *d, int32_t comp_idx, uint32_t before_off,
        const char *var, char *out, size_t out_sz)
{
	size_t vlen = strlen(var);
	if (comp_idx >= 0 && (size_t)comp_idx < d->trans.comp_count) {
		const cx_comp_meta *cm = &d->trans.comps[comp_idx];
		/* 1. Component parameter shorthand */
		for (uint32_t p = 0; p < cm->param_count; p++) {
			const cx_comp_param_meta *pm = &d->trans.comp_params[cm->param_start + p];
			if (pm->name.len == vlen &&
			    memcmp(d->cx_text + pm->name.off, var, vlen) == 0)
			{
				snprintf(out, out_sz, "%s", var);
				return 1;
			}
		}

		/* 2. Scan enclosing component body before closure for `bud_signal(T, var, ...)` */
		uint32_t scan_s = cm->body.off;
		uint32_t scan_e = (before_off > scan_s && before_off <= d->cx_len) ? before_off : scan_s;
		const char *s = d->cx_text;
		for (uint32_t i = scan_s; i + 11 < scan_e; i++) {
			if (memcmp(s + i, "bud_signal", 10) == 0 &&
			    (i == 0 || !is_id_char(s[i - 1])) &&
			    !is_id_char(s[i + 10]))
			{
				uint32_t p = i + 10;
				while (p < scan_e && isspace((unsigned char)s[p]))
					p++;
				if (p < scan_e && s[p] == '(') {
					p++;
					while (p < scan_e && isspace((unsigned char)s[p]))
						p++;
					uint32_t ts = p;
					while (p < scan_e && s[p] != ',' && s[p] != ')')
						p++;
					uint32_t te = p;
					while (te > ts && isspace((unsigned char)s[te - 1]))
						te--;
					if (p < scan_e && s[p] == ',') {
						p++;
						while (p < scan_e && isspace((unsigned char)s[p]))
							p++;
						uint32_t ns = p;
						while (p < scan_e && is_id_char(s[p]))
							p++;
						if ((size_t)(p - ns) == vlen &&
						    memcmp(s + ns, var, vlen) == 0 && te > ts)
						{
							snprintf(out, out_sz, "%.*s *%s",
							         (int)(te - ts), s + ts, var);
							return 1;
						}
					}
				}
			}
		}

		/* 3. Simple local declaration line `T var = ...;` or `T *var = ...;` */
		uint32_t ls = scan_s;
		while (ls < scan_e) {
			uint32_t le = ls;
			while (le < scan_e && s[le] != '\n' && s[le] != ';')
				le++;
			uint32_t next_ls = (le < scan_e) ? (le + 1) : le;
			while (ls < le && isspace((unsigned char)s[ls]))
				ls++;
			/* Look for `var` preceded by type/space/`*` and followed by `=` or end */
			for (uint32_t p = ls; p + vlen <= le; p++) {
				if (memcmp(s + p, var, vlen) == 0 &&
				    p > ls && !is_id_char(s[p - 1]) &&
				    (p + vlen == le || !is_id_char(s[p + vlen])))
				{
					uint32_t after = p + (uint32_t)vlen;
					while (after < le && isspace((unsigned char)s[after]))
						after++;
					if (after == le || s[after] == '=') {
						/* Ensure prefix looks like a C type (starts with ident, no '(' before var) */
						if (is_id_start(s[ls]) &&
						    !(le - ls >= 6 && memcmp(s + ls, "return", 6) == 0) &&
						    !(le - ls >= 2 && memcmp(s + ls, "if", 2) == 0 && !is_id_char(s[ls + 2])) &&
						    !(le - ls >= 3 && memcmp(s + ls, "for", 3) == 0 && !is_id_char(s[ls + 3])))
						{
							int has_paren = 0;
							for (uint32_t k = ls; k < p; k++) {
								if (s[k] == '(' || s[k] == '<' || s[k] == '{' || s[k] == '=') {
									has_paren = 1;
									break;
								}
							}
							if (!has_paren) {
								size_t dlen = (size_t)((p + vlen) - ls);
								if (dlen > 0 && dlen < out_sz) {
									memcpy(out, s + ls, dlen);
									out[dlen] = '\0';
									return 1;
								}
							}
						}
					}
				}
			}
			ls = next_ls;
		}
	}
	return 0;
}

static void guess_loop_var(const cx_doc *d, uint32_t elem_off, char *out, size_t out_sz)
{
	snprintf(out, out_sz, "i");
	size_t win = (elem_off > 256) ? (elem_off - 256) : 0;
	const char *s = d->cx_text;
	/* Search backwards for `for (` */
	for (size_t p = elem_off; p > win + 4; p--) {
		if (memcmp(s + p - 3, "for", 3) == 0 &&
		    (p - 3 == 0 || !is_id_char(s[p - 4])) &&
		    !is_id_char(s[p]))
		{
			size_t q = p;
			while (q < elem_off && isspace((unsigned char)s[q]))
				q++;
			if (q < elem_off && s[q] == '(') {
				q++;
				/* Find `=` inside init clause before `;` */
				size_t eq = q;
				while (eq < elem_off && s[eq] != ';' && s[eq] != '=' && s[eq] != ')')
					eq++;
				if (eq < elem_off && s[eq] == '=') {
					size_t ve = eq;
					while (ve > q && isspace((unsigned char)s[ve - 1]))
						ve--;
					size_t vs = ve;
					while (vs > q && is_id_char(s[vs - 1]))
						vs--;
					if (ve > vs && is_id_start(s[vs]) && (ve - vs) < out_sz) {
						memcpy(out, s + vs, ve - vs);
						out[ve - vs] = '\0';
					}
				}
			}
			return;
		}
	}
}

static uint32_t find_tag_header_insert_off(const cx_doc *d, const cx_elem_meta *el)
{
	/* Find `/>` or `>` closing the opening tag header of `el` */
	uint32_t p = el->tag.off + el->tag.len;
	uint32_t end = el->full.off + el->full.len;
	if (end > d->cx_len)
		end = (uint32_t)d->cx_len;
	int brace = 0;
	while (p < end) {
		char c = d->cx_text[p];
		if (c == '"') {
			p++;
			while (p < end && d->cx_text[p] != '"') {
				if (d->cx_text[p] == '\\' && p + 1 < end)
					p++;
				p++;
			}
			if (p < end)
				p++;
			continue;
		}
		if (c == '{') { brace++; p++; continue; }
		if (c == '}') { if (brace > 0) brace--; p++; continue; }
		if (brace == 0) {
			if (c == '/' && p + 1 < end && d->cx_text[p + 1] == '>')
				return p;
			if (c == '>')
				return p;
		}
		p++;
	}
	return el->tag.off + el->tag.len;
}

static int ranges_overlap_off(uint32_t s1, uint32_t e1, uint32_t s2, uint32_t e2)
{
	return s1 <= e2 && s2 <= e1;
}

jval *cx_feat_code_actions(
        const lsp_server *srv, const cx_doc *d,
        const jval *range, const jval *ctx_diags)
{
	jval *actions = jval_new(J_ARR);
	if (!d || !d->cx_text || !range)
		return actions;

	const jval *rs = jval_get(range, "start");
	const jval *re = jval_get(range, "end");
	uint32_t req_s = rs ? line_index_pos_to_off(
	        &d->cx_lines, d->cx_text, d->cx_len,
	        jval_as_int(jval_get(rs, "line"), 0),
	        jval_as_int(jval_get(rs, "character"), 0)) : 0;
	uint32_t req_e = re ? line_index_pos_to_off(
	        &d->cx_lines, d->cx_text, d->cx_len,
	        jval_as_int(jval_get(re, "line"), 0),
	        jval_as_int(jval_get(re, "character"), 0)) : req_s;

	/* 1. Diagnostic-driven Quick-Fixes */
	if (ctx_diags && ctx_diags->kind == J_ARR) {
		for (size_t i = 0; i < ctx_diags->arr.len; i++) {
			const jval *dg = ctx_diags->arr.items[i];
			const char *code = jval_as_str(jval_get(dg, "code"));
			const char *msg = jval_as_str(jval_get(dg, "message"));
			const jval *dr = jval_get(dg, "range");
			const jval *ds = dr ? jval_get(dr, "start") : NULL;
			uint32_t d_off = ds ? line_index_pos_to_off(
			        &d->cx_lines, d->cx_text, d->cx_len,
			        jval_as_int(jval_get(ds, "line"), 0),
			        jval_as_int(jval_get(ds, "character"), 0)) : req_s;

			/* 1a. Auto-capture undeclared identifier inside a closure */
			if (msg && (strstr(msg, "undeclared identifier") || strstr(msg, "undeclared"))) {
				char var[128] = { 0 };
				if (extract_quoted(msg, var, sizeof(var))) {
					for (size_t k = 0; k < d->trans.closure_count; k++) {
						const cx_closure_meta *cl = &d->trans.closures[k];
						if (!slice_contains(cl->body, d_off))
							continue;
						char cap_decl[192];
						if (infer_capture_decl(d, cl->comp_idx, cl->full.off,
						                       var, cap_decl, sizeof(cap_decl)))
						{
							uint32_t ins_off = cl->captures.off + cl->captures.len;
							/* Check if capture list has existing non-ws content */
							int has_existing = 0;
							for (uint32_t c = cl->captures.off; c < ins_off; c++) {
								if (!isspace((unsigned char)d->cx_text[c])) {
									has_existing = 1;
									break;
								}
							}
							char ins[224];
							snprintf(ins, sizeof(ins), "%s%s",
							         has_existing ? ", " : "", cap_decl);
							long long il = 0, ic = 0;
							line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len,
							                      ins_off, &il, &ic);
							jval *ws = jval_new(J_OBJ);
							ws_edit_add(ws, d->cx_uri, make_text_edit(
							        make_lsp_range(il, ic, il, ic), ins));
							char title[384];
							snprintf(title, sizeof(title), "Capture '%s' in closure [%s]",
							         var, cap_decl);
							jval_push(actions, make_quickfix(title, 1, ws, dg));
						}
						break;
					}
				}
			}

			/* 1b. Add missing `key={...}` inside `{for (...)}` */
			if (code && strcmp(code, "cx-missing-key") == 0) {
				for (size_t e = 0; e < d->trans.elem_count; e++) {
					const cx_elem_meta *el = &d->trans.elems[e];
					if (el->in_loop_direct && !el->has_key_attr &&
					    d_off >= el->full.off && d_off <= el->tag.off + el->tag.len)
					{
						char lvar[64];
						guess_loop_var(d, el->full.off, lvar, sizeof(lvar));
						char ins[96];
						snprintf(ins, sizeof(ins), " key={%s}", lvar);
						uint32_t ins_off = el->tag.off + el->tag.len;
						long long il = 0, ic = 0;
						line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len,
						                      ins_off, &il, &ic);
						jval *ws = jval_new(J_OBJ);
						ws_edit_add(ws, d->cx_uri, make_text_edit(
						        make_lsp_range(il, ic, il, ic), ins));
						char title[128];
						snprintf(title, sizeof(title), "Add key={%s} to <%.*s>",
						         lvar, (int)el->tag.len, d->cx_text + el->tag.off);
						jval_push(actions, make_quickfix(title, 1, ws, dg));
						break;
					}
				}
			}

			/* 1c. Remove duplicate attribute */
			if (code && strcmp(code, "cx-duplicate-attr") == 0) {
				for (size_t e = 0; e < d->trans.elem_count; e++) {
					const cx_elem_meta *el = &d->trans.elems[e];
					for (uint32_t a = 0; a < el->attr_count; a++) {
						const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
						if (!slice_contains(at->full, d_off))
							continue;
						cx_slice del = at->full;
						if (del.off > 0 && d->cx_text[del.off - 1] == ' ') {
							del.off--;
							del.len++;
						}
						jval *ws = jval_new(J_OBJ);
						ws_edit_add(ws, d->cx_uri, make_text_edit(
						        cx_slice_to_range(&d->cx_lines, d->cx_text, d->cx_len, del),
						        ""));
						char title[160];
						snprintf(title, sizeof(title), "Remove duplicate attribute '%.*s'",
						         (int)at->name.len, d->cx_text + at->name.off);
						jval_push(actions, make_quickfix(title, 1, ws, dg));
						break;
					}
				}
			}

			/* 1d. Scaffold undefined `<NewComp ... />` */
			if ((code && strcmp(code, "cx-unknown-component") == 0) ||
			    (msg && (strstr(msg, "unknown component") != NULL ||
			             strstr(msg, "_render'") != NULL ||
			             strstr(msg, "_props'") != NULL)))
			{
				for (size_t e = 0; e < d->trans.elem_count; e++) {
					const cx_elem_meta *el = &d->trans.elems[e];
					if (!el->is_component ||
					    !(d_off >= el->full.off && d_off <= el->tag.off + el->tag.len))
						continue;
					uint32_t ins_off = 0;
					if (el->comp_idx >= 0 && (size_t)el->comp_idx < d->trans.comp_count)
						ins_off = d->trans.comps[el->comp_idx].decl.off;
					lsp_buf stub = { 0 };
					lbuf_printf(&stub, "static component %.*s(",
					            (int)el->tag.len, d->cx_text + el->tag.off);
					int nparams = 0;
					for (uint32_t a = 0; a < el->attr_count; a++) {
						const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
						if (slice_eq_str(d->cx_text, at->name, "key"))
							continue;
						if (nparams > 0)
							lbuf_str(&stub, ", ");
						if (at->val_kind == CX_ATTR_STRING)
							lbuf_printf(&stub, "const char *%.*s",
							            (int)at->name.len, d->cx_text + at->name.off);
						else
							lbuf_printf(&stub, "int %.*s",
							            (int)at->name.len, d->cx_text + at->name.off);
						nparams++;
					}
					if (!el->is_self_closing) {
						if (nparams > 0)
							lbuf_str(&stub, ", ");
						lbuf_str(&stub, "bud_node *children");
						nparams++;
					}
					if (nparams == 0)
						lbuf_str(&stub, "void");
					lbuf_str(&stub, ") {\n\treturn <div></div>;\n}\n\n");

					long long il = 0, ic = 0;
					line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len,
					                      ins_off, &il, &ic);
					jval *ws = jval_new(J_OBJ);
					ws_edit_add(ws, d->cx_uri, make_text_edit(
					        make_lsp_range(il, ic, il, ic), stub.data ? stub.data : ""));
					char title[160];
					snprintf(title, sizeof(title), "Create component %.*s",
					         (int)el->tag.len, d->cx_text + el->tag.off);
					jval_push(actions, make_quickfix(title, 1, ws, dg));
					lbuf_free(&stub);
					break;
				}
			}
		}
	}

	/* 2. Cursor-driven Quick-Fix: Add missing props to `<Component ...>` */
	for (size_t e = d->trans.elem_count; e-- > 0; ) {
		const cx_elem_meta *el = &d->trans.elems[e];
		if (!el->is_component || el->tag.len == 0)
			continue;
		uint32_t hdr_end = find_tag_header_insert_off(d, el);
		if (!ranges_overlap_off(req_s, req_e, el->full.off, hdr_end))
			continue;

		ws_comp_hit hit;
		if (!ws_find_comp(srv, d, d->cx_text + el->tag.off, el->tag.len, &hit))
			break;

		lsp_buf ins = { 0 };
		int missing = 0;
		size_t pcount = (hit.doc && hit.doc_comp) ? hit.doc_comp->param_count
		                : (hit.ws_comp ? hit.ws_comp->prop_count : 0);
		for (size_t p = 0; p < pcount; p++) {
			const char *pn = NULL;
			size_t pnl = 0;
			const char *pd = NULL;
			size_t pdl = 0;
			if (hit.doc && hit.doc_comp) {
				const cx_comp_param_meta *pm =
				        &hit.doc->trans.comp_params[hit.doc_comp->param_start + p];
				pn = hit.doc->cx_text + pm->name.off;
				pnl = pm->name.len;
				pd = hit.doc->cx_text + pm->decl.off;
				pdl = pm->decl.len;
			} else {
				pn = hit.ws_comp->prop_names[p];
				pnl = strlen(pn);
				pd = hit.ws_comp->prop_decls[p];
				pdl = strlen(pd);
			}
			if (pnl == 8 && memcmp(pn, "children", 8) == 0)
				continue;

			int present = 0;
			for (uint32_t a = 0; a < el->attr_count; a++) {
				const cx_attr_meta *at = &d->trans.attrs[el->attr_start + a];
				if (at->name.len == pnl &&
				    memcmp(d->cx_text + at->name.off, pn, pnl) == 0)
				{
					present = 1;
					break;
				}
			}
			if (!present) {
				int is_cstr = 0;
				if (pdl >= 4) {
					char *tmp = xstrndup(pd, pdl);
					if (tmp && strstr(tmp, "char") && strchr(tmp, '*'))
						is_cstr = 1;
					free(tmp);
				}
				lbuf_printf(&ins, " %.*s=%s", (int)pnl, pn, is_cstr ? "\"\"" : "{0}");
				missing++;
			}
		}

		if (missing > 0) {
			uint32_t ins_pos = hdr_end;
			while (ins_pos > el->tag.off + el->tag.len && d->cx_text[ins_pos - 1] == ' ')
				ins_pos--;
			long long il = 0, ic = 0;
			line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len, ins_pos, &il, &ic);
			jval *ws = jval_new(J_OBJ);
			ws_edit_add(ws, d->cx_uri, make_text_edit(
			        make_lsp_range(il, ic, il, ic), ins.data ? ins.data : ""));
			char title[160];
			snprintf(title, sizeof(title), "Add %d missing prop%s to <%.*s>",
			         missing, missing == 1 ? "" : "s",
			         (int)el->tag.len, d->cx_text + el->tag.off);
			jval_push(actions, make_quickfix(title, 0, ws, NULL));
		}
		lbuf_free(&ins);
		break;
	}

	return actions;
}

jval *cx_feat_merge_code_actions(const lsp_server *srv, jval *cx_actions, jval *clangd_actions)
{
	if (!cx_actions)
		cx_actions = jval_new(J_ARR);
	if (!clangd_actions || clangd_actions->kind != J_ARR)
		return cx_actions;

	for (size_t i = 0; i < clangd_actions->arr.len; i++) {
		jval *act = clangd_actions->arr.items[i];
		jval *ed = jval_get(act, "edit");
		if (ed)
			cx_feat_filter_clangd_rename(srv, ed);
		jval_push(cx_actions, jval_clone(act));
	}
	return cx_actions;
}

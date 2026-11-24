/**
 * @file cx_lsp_index.c
 * @brief Workspace `.cx`/`.cxh` component indexing, `#include` shadow-C
 *        prototype synthesis, cross-file resolution, and zero-config clangd
 *        fallback include flags.
 */

#define _XOPEN_SOURCE 700

#include "cx_lsp_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WS_MAX_FILES 512
#define WS_MAX_DEPTH 6
#define WS_MAX_FILE_BYTES (4u * 1024u * 1024u)

/* ── Small Helpers ────────────────────────────────────────────────────── */

static char *read_file_bounded(const char *path, size_t *out_len)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return NULL;
	if ((uint64_t)st.st_size > WS_MAX_FILE_BYTES)
		return NULL;
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	size_t sz = (size_t)st.st_size;
	char *buf = (char *)malloc(sz + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	size_t n = (sz > 0) ? fread(buf, 1, sz, fp) : 0;
	fclose(fp);
	buf[n] = '\0';
	if (out_len)
		*out_len = n;
	return buf;
}

static void ws_comp_free(ws_comp *c)
{
	free(c->name);
	free(c->decl_sig);
	free(c->doc);
	for (size_t i = 0; i < c->prop_count; i++) {
		free(c->prop_names[i]);
		free(c->prop_decls[i]);
	}
	free(c->prop_names);
	free(c->prop_decls);
	memset(c, 0, sizeof(*c));
}

static void ws_file_free(ws_file *f)
{
	free(f->path);
	free(f->uri);
	for (size_t i = 0; i < f->comp_count; i++)
		ws_comp_free(&f->comps[i]);
	free(f->comps);
	memset(f, 0, sizeof(*f));
}

void ws_index_free(ws_index *ws)
{
	if (!ws)
		return;
	for (size_t i = 0; i < ws->file_count; i++)
		ws_file_free(&ws->files[i]);
	free(ws->files);
	memset(ws, 0, sizeof(*ws));
}

/* Strip doc-comment delimiters into clean markdown text. */
static char *clean_doc_comment(const char *src, cx_slice sl)
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
		if (le - ls >= 3 && memcmp(ls, "///", 3) == 0) {
			ls += 3;
		} else if (le - ls >= 3 && memcmp(ls, "/**", 3) == 0) {
			ls += 3;
		} else if (le - ls >= 2 && memcmp(ls, "/*", 2) == 0) {
			ls += 2;
		} else if (ls < le && *ls == '*') {
			if (le - ls >= 2 && ls[1] == '/')
				ls += 2;
			else
				ls += 1;
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

static ws_file *ws_find_file_by_path(ws_index *ws, const char *path)
{
	for (size_t i = 0; i < ws->file_count; i++) {
		if (strcmp(ws->files[i].path, path) == 0)
			return &ws->files[i];
	}
	return NULL;
}

static ws_file *ws_upsert_file(ws_index *ws, const char *path)
{
	ws_file *f = ws_find_file_by_path(ws, path);
	if (f)
		return f;
	if (ws->file_count >= ws->file_cap) {
		size_t ncap = ws->file_cap ? ws->file_cap * 2 : 16;
		ws_file *nf = (ws_file *)realloc(ws->files, ncap * sizeof(*nf));
		if (!nf)
			return NULL;
		ws->files = nf;
		ws->file_cap = ncap;
	}
	f = &ws->files[ws->file_count++];
	memset(f, 0, sizeof(*f));
	f->path = xstrndup(path, strlen(path));
	f->uri = path_to_uri(path);
	return f;
}

static void ws_populate_comps_from_result(
        ws_file *f, const char *src, size_t len,
        const cx_result *res, const line_index *li)
{
	for (size_t i = 0; i < f->comp_count; i++)
		ws_comp_free(&f->comps[i]);
	free(f->comps);
	f->comps = NULL;
	f->comp_count = 0;

	if (res->comp_count == 0)
		return;

	f->comps = (ws_comp *)calloc(res->comp_count, sizeof(ws_comp));
	if (!f->comps)
		return;

	for (size_t i = 0; i < res->comp_count; i++) {
		const cx_comp_meta *cm = &res->comps[i];
		if (cm->name.len == 0)
			continue;
		ws_comp *wc = &f->comps[f->comp_count++];
		wc->name = xstrndup(src + cm->name.off, cm->name.len);
		wc->decl_sig = xstrndup(src + cm->decl.off, cm->decl.len);
		wc->doc = clean_doc_comment(src, cm->doc);
		wc->is_static = cm->is_static;
		wc->is_forward = cm->is_forward;
		wc->name_len = cm->name.len;

		long long ln = 0, col = 0;
		line_index_off_to_pos(li, src, len, cm->name.off, &ln, &col);
		wc->line = (uint32_t)(ln >= 0 ? ln : 0);
		wc->col = (uint32_t)(col >= 0 ? col : 0);

		if (cm->param_count > 0) {
			wc->prop_names = (char **)calloc(cm->param_count, sizeof(char *));
			wc->prop_decls = (char **)calloc(cm->param_count, sizeof(char *));
			if (wc->prop_names && wc->prop_decls) {
				for (uint32_t p = 0; p < cm->param_count; p++) {
					const cx_comp_param_meta *pm = &res->comp_params[cm->param_start + p];
					wc->prop_names[p] = xstrndup(src + pm->name.off, pm->name.len);
					wc->prop_decls[p] = xstrndup(src + pm->decl.off, pm->decl.len);
				}
				wc->prop_count = cm->param_count;
			}
		}
	}
}

int ws_index_index_path(lsp_server *srv, const char *path)
{
	if (!srv || !path || !path[0])
		return -1;

	char real_buf[PATH_MAX];
	const char *canon = realpath(path, real_buf) ? real_buf : path;

	struct stat st;
	if (stat(canon, &st) != 0 || !S_ISREG(st.st_mode))
		return -1;

	ws_file *existing = ws_find_file_by_path(&srv->ws, canon);
	if (existing && existing->mtime_sec == (int64_t)st.st_mtime)
		return 0;

	size_t slen = 0;
	char *src = read_file_bounded(canon, &slen);
	if (!src)
		return -1;

	cx_options opt;
	memset(&opt, 0, sizeof(opt));
	opt.filename = canon;
	opt.emit_lines = 0;
	opt.recover = 1;
	opt.build_index = 1;

	cx_result res;
	if (cx_transpile(&opt, src, slen, &res) != 0) {
		free(src);
		return -1;
	}

	line_index li = { 0 };
	line_index_build(&li, src, slen);

	ws_file *f = ws_upsert_file(&srv->ws, canon);
	if (f) {
		f->mtime_sec = (int64_t)st.st_mtime;
		ws_populate_comps_from_result(f, src, slen, &res, &li);
	}

	line_index_free(&li);
	cx_result_free(&res);
	free(src);
	return f ? 0 : -1;
}

void ws_index_update_doc(lsp_server *srv, const cx_doc *d)
{
	if (!srv || !d || !d->cx_path || !d->cx_path[0])
		return;
	char real_buf[PATH_MAX];
	const char *canon = realpath(d->cx_path, real_buf) ? real_buf : d->cx_path;
	ws_file *f = ws_upsert_file(&srv->ws, canon);
	if (!f)
		return;
	struct stat st;
	f->mtime_sec = (stat(canon, &st) == 0) ? (int64_t)st.st_mtime : 0;
	ws_populate_comps_from_result(f, d->cx_text ? d->cx_text : "", d->cx_len,
	                              &d->trans, &d->cx_lines);
}

/* ── Bounded Workspace Directory Scanner ──────────────────────────────── */

static int should_skip_dir(const char *name)
{
	if (name[0] == '.')
		return 1;
	return strcmp(name, "node_modules") == 0 ||
	       strcmp(name, "build") == 0 ||
	       strcmp(name, "var") == 0 ||
	       strcmp(name, "htdocs") == 0 ||
	       strcmp(name, "target") == 0 ||
	       strcmp(name, "dist") == 0 ||
	       strcmp(name, "tmp") == 0 ||
	       strcmp(name, "bin") == 0 ||
	       strcmp(name, "lib") == 0;
}

static void scan_dir_rec(lsp_server *srv, const char *dir_path, int depth, size_t *indexed)
{
	if (depth > WS_MAX_DEPTH || *indexed >= WS_MAX_FILES)
		return;
	DIR *dp = opendir(dir_path);
	if (!dp)
		return;

	struct dirent *ent;
	while ((ent = readdir(dp)) != NULL && *indexed < WS_MAX_FILES) {
		const char *name = ent->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;

		char child[PATH_MAX];
		int n = snprintf(child, sizeof(child), "%s/%s", dir_path, name);
		if (n <= 0 || (size_t)n >= sizeof(child))
			continue;

		struct stat st;
		if (lstat(child, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (!should_skip_dir(name))
				scan_dir_rec(srv, child, depth + 1, indexed);
		} else if (S_ISREG(st.st_mode) && is_cx_or_cxh_path(child)) {
			if (ws_index_index_path(srv, child) == 0)
				(*indexed)++;
		}
	}
	closedir(dp);
}

void ws_index_scan_root(lsp_server *srv, const char *root_path)
{
	if (!srv || !root_path || !root_path[0])
		return;
	size_t indexed = 0;
	scan_dir_rec(srv, root_path, 0, &indexed);
}

/* ── `#include "*.cxh"` / `"*.cx"` Shadow-C Splicer ──────────────────── */

static int resolve_rel_path(const char *base_file, const char *rel, char *out, size_t out_sz)
{
	if (!rel || !rel[0])
		return 0;
	if (rel[0] == '/') {
		if (strlen(rel) >= out_sz)
			return 0;
		strcpy(out, rel);
		return 1;
	}
	const char *slash = base_file ? strrchr(base_file, '/') : NULL;
	if (!slash) {
		if (strlen(rel) >= out_sz)
			return 0;
		strcpy(out, rel);
		return 1;
	}
	size_t dlen = (size_t)(slash - base_file);
	int n = snprintf(out, out_sz, "%.*s/%s", (int)dlen, base_file, rel);
	return n > 0 && (size_t)n < out_sz;
}

static int path_ends_with(const char *s, size_t n, const char *suf)
{
	size_t slen = strlen(suf);
	return n >= slen && memcmp(s + n - slen, suf, slen) == 0;
}

static void append_proto_for_ws_comp(lsp_buf *b, const ws_comp *wc)
{
	if (wc->is_static || !wc->name || !wc->name[0])
		return;
	lbuf_printf(b, "typedef struct %s_props {\n", wc->name);
	if (wc->prop_count == 0) {
		lbuf_str(b, "\tint _cx_empty;\n");
	} else {
		for (size_t p = 0; p < wc->prop_count; p++)
			lbuf_printf(b, "\t%s;\n", wc->prop_decls[p]);
	}
	lbuf_printf(b, "} %s_props;\n", wc->name);
	lbuf_printf(b, "bud_node *%s_render(void *__cx_ctx, const void *__cx_raw_props);\n", wc->name);
}

/* Strip `#pragma once` lines so transpiled `.cxh` text can be inlined into a `.c` shadow file. */
static void append_cxh_transpiled_body(lsp_buf *out, const char *c_code, size_t c_len)
{
	size_t i = 0;
	while (i < c_len) {
		size_t ls = i;
		while (i < c_len && c_code[i] != '\n')
			i++;
		size_t le = i;
		if (i < c_len && c_code[i] == '\n')
			i++;

		const char *p = c_code + ls;
		size_t plen = le - ls;
		while (plen > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			plen--;
		}
		if (plen >= 7 && memcmp(p, "#pragma", 7) == 0) {
			const char *q = p + 7;
			size_t qlen = plen - 7;
			while (qlen > 0 && (*q == ' ' || *q == '\t')) {
				q++;
				qlen--;
			}
			if (qlen >= 4 && memcmp(q, "once", 4) == 0)
				continue;
		}
		lbuf_append(out, c_code + ls, i - ls);
	}
}

static int splice_shadow_c(
        cx_result *res, uint32_t c_off, uint32_t old_len,
        const char *rep, size_t rep_len)
{
	if (!res || !res->c_code || c_off + old_len > res->c_len)
		return -1;
	size_t new_total = res->c_len - old_len + rep_len;
	char *nbuf = (char *)malloc(new_total + 1);
	if (!nbuf)
		return -1;
	memcpy(nbuf, res->c_code, c_off);
	if (rep_len > 0)
		memcpy(nbuf + c_off, rep, rep_len);
	memcpy(nbuf + c_off + rep_len, res->c_code + c_off + old_len,
	       res->c_len - (c_off + old_len));
	nbuf[new_total] = '\0';
	free(res->c_code);
	res->c_code = nbuf;
	res->c_len = new_total;

	int64_t delta = (int64_t)rep_len - (int64_t)old_len;
	uint32_t old_end = c_off + old_len;

	/* Check if [c_off, old_end) sits strictly inside a larger 1:1 passthrough span */
	size_t split_idx = (size_t)-1;
	for (size_t i = 0; i < res->span_count; i++) {
		cx_span *sp = &res->spans[i];
		if (sp->c_off <= c_off && sp->c_off + sp->c_len >= old_end &&
		    !(sp->c_off == c_off && sp->c_len == old_len) &&
		    sp->c_len == sp->cx_len)
		{
			split_idx = i;
			break;
		}
	}

	/* First shift all spans that start at or after old_end */
	for (size_t i = 0; i < res->span_count; i++) {
		if (i == split_idx)
			continue;
		cx_span *sp = &res->spans[i];
		if (sp->c_off == c_off && sp->c_len == old_len) {
			sp->c_len = (uint32_t)rep_len;
		} else if (sp->c_off >= old_end) {
			sp->c_off = (uint32_t)((int64_t)sp->c_off + delta);
		} else if (sp->c_off + sp->c_len > c_off) {
			int64_t nl = (int64_t)sp->c_len + delta;
			sp->c_len = (nl > 0) ? (uint32_t)nl : 0;
		}
	}

	if (split_idx != (size_t)-1) {
		cx_span orig = res->spans[split_idx];
		uint32_t pre_len = c_off - orig.c_off;
		uint32_t suf_c_old = old_end;
		uint32_t suf_len = (orig.c_off + orig.c_len) - suf_c_old;
		uint32_t inc_cx_off = orig.cx_off + pre_len;

		/* Replace split_idx with up to 3 spans: prefix, spliced include, suffix */
		cx_span parts[3];
		size_t nparts = 0;
		if (pre_len > 0) {
			parts[nparts].cx_off = orig.cx_off;
			parts[nparts].cx_len = pre_len;
			parts[nparts].c_off = orig.c_off;
			parts[nparts].c_len = pre_len;
			parts[nparts].kind = orig.kind;
			nparts++;
		}
		parts[nparts].cx_off = inc_cx_off;
		parts[nparts].cx_len = old_len;
		parts[nparts].c_off = c_off;
		parts[nparts].c_len = (uint32_t)rep_len;
		parts[nparts].kind = orig.kind;
		nparts++;
		if (suf_len > 0) {
			parts[nparts].cx_off = inc_cx_off + old_len;
			parts[nparts].cx_len = suf_len;
			parts[nparts].c_off = (uint32_t)((int64_t)suf_c_old + delta);
			parts[nparts].c_len = suf_len;
			parts[nparts].kind = orig.kind;
			nparts++;
		}

		size_t extra = nparts - 1;
		cx_span *nsp = (cx_span *)realloc(
		        res->spans, (res->span_count + extra) * sizeof(*nsp));
		if (nsp) {
			res->spans = nsp;
			if (extra > 0 && split_idx + 1 < res->span_count) {
				memmove(&res->spans[split_idx + nparts],
				        &res->spans[split_idx + 1],
				        (res->span_count - split_idx - 1) * sizeof(*nsp));
			}
			for (size_t k = 0; k < nparts; k++)
				res->spans[split_idx + k] = parts[k];
			res->span_count += extra;
		}
	}
	return 0;
}

static int doc_defines_comp(const cx_doc *d, const char *name)
{
	size_t nlen = strlen(name);
	for (size_t i = 0; i < d->trans.comp_count; i++) {
		const cx_comp_meta *cm = &d->trans.comps[i];
		if (cm->name.len == nlen &&
		    memcmp(d->cx_text + cm->name.off, name, nlen) == 0)
		{
			return 1;
		}
	}
	return 0;
}

void ws_index_resolve_includes(lsp_server *srv, cx_doc *d)
{
	if (!srv || !d || d->trans.include_count == 0 || !d->cx_text)
		return;

	int modified = 0;
	/* Process in reverse byte order so earlier span c_offs stay untouched while splicing */
	for (size_t idx = d->trans.include_count; idx-- > 0; ) {
		const cx_include_meta *inc = &d->trans.includes[idx];
		if (inc->is_system || inc->path.len == 0)
			continue;

		const char *pstr = d->cx_text + inc->path.off;
		size_t plen = inc->path.len;
		int is_cxh = path_ends_with(pstr, plen, ".cxh");
		int is_cx = path_ends_with(pstr, plen, ".cx");
		if (!is_cxh && !is_cx)
			continue;

		char *rel = xstrndup(pstr, plen);
		if (!rel)
			continue;
		char target_raw[PATH_MAX];
		int ok = resolve_rel_path(d->cx_path, rel, target_raw, sizeof(target_raw));
		free(rel);
		if (!ok)
			continue;

		char real_buf[PATH_MAX];
		const char *target = realpath(target_raw, real_buf) ? real_buf : target_raw;

		/* Always index the included file so cross-file hover/def/completion work */
		ws_index_index_path(srv, target);

		/* Find the exact shadow-C slice corresponding to this #include line */
		uint32_t c_off = 0, c_len = 0;
		int found_span = 0;
		for (size_t s = 0; s < d->trans.span_count; s++) {
			const cx_span *sp = &d->trans.spans[s];
			if (sp->c_len == sp->cx_len &&
			    sp->cx_off <= inc->full.off &&
			    inc->full.off + inc->full.len <= sp->cx_off + sp->cx_len)
			{
				c_off = sp->c_off + (inc->full.off - sp->cx_off);
				c_len = inc->full.len;
				found_span = 1;
				break;
			}
		}
		if (!found_span)
			continue;

		lsp_buf rep = { 0 };
		if (is_cxh) {
			/* Transpile `.cxh` in memory so typedefs, enums, and component forward decls all work */
			const cx_doc *open_inc = find_doc_by_path(srv, target);
			if (open_inc && open_inc->trans.c_code) {
				append_cxh_transpiled_body(&rep, open_inc->trans.c_code, open_inc->trans.c_len);
			} else {
				size_t flen = 0;
				char *fsrc = read_file_bounded(target, &flen);
				if (fsrc) {
					cx_options opt;
					memset(&opt, 0, sizeof(opt));
					opt.filename = target;
					opt.emit_lines = 0;
					opt.recover = 1;
					cx_result ires;
					if (cx_transpile(&opt, fsrc, flen, &ires) == 0) {
						append_cxh_transpiled_body(&rep, ires.c_code, ires.c_len);
						cx_result_free(&ires);
					}
					free(fsrc);
				}
			}
		} else {
			/* `#include "other.cx"`: synthesize prototypes for non-static components */
			ws_file *wf = ws_find_file_by_path(&srv->ws, target);
			if (wf) {
				for (size_t c = 0; c < wf->comp_count; c++) {
					if (wf->comps[c].is_static)
						continue;
					if (doc_defines_comp(d, wf->comps[c].name))
						continue;
					/* Deduplicate within wf */
					int dup = 0;
					for (size_t j = 0; j < c; j++) {
						if (strcmp(wf->comps[j].name, wf->comps[c].name) == 0) {
							dup = 1;
							break;
						}
					}
					if (!dup)
						append_proto_for_ws_comp(&rep, &wf->comps[c]);
				}
			}
		}

		if (rep.len > 0) {
			if (rep.data[rep.len - 1] != '\n')
				lbuf_ch(&rep, '\n');
			if (splice_shadow_c(&d->trans, c_off, c_len, rep.data, rep.len) == 0)
				modified = 1;
		}
		lbuf_free(&rep);
	}

	if (modified)
		line_index_build(&d->c_lines, d->trans.c_code ? d->trans.c_code : "", d->trans.c_len);
}

/* ── Cross-File Component Lookup ──────────────────────────────────────── */

static int fill_hit_from_doc(
        const cx_doc *d, const cx_comp_meta *cm, ws_comp_hit *out)
{
	memset(out, 0, sizeof(*out));
	out->name = d->cx_text + cm->name.off;
	out->uri = d->cx_uri;
	out->path = d->cx_path;
	out->doc = d;
	out->doc_comp = cm;
	out->name_len = cm->name.len;
	out->is_forward = cm->is_forward;
	long long ln = 0, col = 0;
	line_index_off_to_pos(&d->cx_lines, d->cx_text, d->cx_len, cm->name.off, &ln, &col);
	out->line = (uint32_t)(ln >= 0 ? ln : 0);
	out->col = (uint32_t)(col >= 0 ? col : 0);
	return 1;
}

static int fill_hit_from_ws(
        const ws_file *wf, const ws_comp *wc, ws_comp_hit *out)
{
	memset(out, 0, sizeof(*out));
	out->name = wc->name;
	out->uri = wf->uri;
	out->path = wf->path;
	out->ws_comp = wc;
	out->line = wc->line;
	out->col = wc->col;
	out->name_len = wc->name_len;
	out->is_forward = wc->is_forward;
	return 1;
}

int ws_find_comp(
        const lsp_server *srv, const cx_doc *cur,
        const char *name, size_t nlen, ws_comp_hit *out)
{
	if (!name || nlen == 0 || !out)
		return 0;

	/* Two passes: pass 0 = non-forward definitions only; pass 1 = forward declarations */
	for (int pass = 0; pass < 2; pass++) {
		int want_fwd = pass;

		/* 1. Current open document (includes static components) */
		if (cur && cur->cx_text) {
			for (size_t i = 0; i < cur->trans.comp_count; i++) {
				const cx_comp_meta *cm = &cur->trans.comps[i];
				if ((int)cm->is_forward != want_fwd)
					continue;
				if (cm->name.len == nlen &&
				    memcmp(cur->cx_text + cm->name.off, name, nlen) == 0)
				{
					return fill_hit_from_doc(cur, cm, out);
				}
			}
		}

		if (!srv)
			continue;

		/* 2. Other open documents (non-static) */
		for (size_t di = 0; di < srv->doc_count; di++) {
			const cx_doc *od = srv->docs[di];
			if (od == cur || !od->cx_text)
				continue;
			for (size_t i = 0; i < od->trans.comp_count; i++) {
				const cx_comp_meta *cm = &od->trans.comps[i];
				if (cm->is_static || (int)cm->is_forward != want_fwd)
					continue;
				if (cm->name.len == nlen &&
				    memcmp(od->cx_text + cm->name.off, name, nlen) == 0)
				{
					return fill_hit_from_doc(od, cm, out);
				}
			}
		}

		/* 3. Indexed workspace files (skip files shadowed by an open cx_doc) */
		for (size_t fi = 0; fi < srv->ws.file_count; fi++) {
			const ws_file *wf = &srv->ws.files[fi];
			if (find_doc_by_path(srv, wf->path))
				continue;
			for (size_t i = 0; i < wf->comp_count; i++) {
				const ws_comp *wc = &wf->comps[i];
				if (wc->is_static || (int)wc->is_forward != want_fwd)
					continue;
				if (wc->name_len == nlen && memcmp(wc->name, name, nlen) == 0)
					return fill_hit_from_ws(wf, wc, out);
			}
		}
	}

	return 0;
}

/* Map `file:///.../foo.cx.h` (from `cx --emit-header`) back to `file:///.../foo.cx`. */
int ws_find_cx_for_generated_header(
        const lsp_server *srv, const char *hdr_uri,
        const char *sym_hint, char **out_cx_uri,
        long long *out_line, long long *out_col, long long *out_len)
{
	if (!hdr_uri)
		return 0;
	size_t ulen = strlen(hdr_uri);
	if (ulen < 5 || strcmp(hdr_uri + ulen - 5, ".cx.h") != 0)
		return 0;

	char *cx_uri = xstrndup(hdr_uri, ulen - 2); /* drop `.h` */
	if (!cx_uri)
		return 0;
	char *cx_path = uri_to_path(cx_uri);
	if (!cx_path) {
		free(cx_uri);
		return 0;
	}

	struct stat st;
	if (stat(cx_path, &st) != 0) {
		free(cx_path);
		free(cx_uri);
		return 0;
	}

	/* If sym_hint matches a component in that .cx file, jump straight to its decl */
	if (srv && sym_hint && sym_hint[0]) {
		ws_comp_hit hit;
		if (ws_find_comp(srv, NULL, sym_hint, strlen(sym_hint), &hit) &&
		    hit.uri && strcmp(hit.uri, cx_uri) == 0)
		{
			*out_cx_uri = cx_uri;
			*out_line = (long long)hit.line;
			*out_col = (long long)hit.col;
			*out_len = (long long)hit.name_len;
			free(cx_path);
			return 1;
		}
	}

	/* Fallback: first component in that file, else line 0 */
	*out_cx_uri = cx_uri;
	*out_line = 0;
	*out_col = 0;
	*out_len = 0;
	if (srv) {
		char real_buf[PATH_MAX];
		const char *canon = realpath(cx_path, real_buf) ? real_buf : cx_path;
		for (size_t i = 0; i < srv->ws.file_count; i++) {
			if (strcmp(srv->ws.files[i].path, canon) == 0 &&
			    srv->ws.files[i].comp_count > 0)
			{
				*out_line = (long long)srv->ws.files[i].comps[0].line;
				*out_col = (long long)srv->ws.files[i].comps[0].col;
				*out_len = (long long)srv->ws.files[i].comps[0].name_len;
				break;
			}
		}
	}
	free(cx_path);
	return 1;
}

/* ── Zero-Config Fallback Include Flags for clangd ────────────────────── */

static int has_compilation_db_in_ancestors(const char *dir)
{
	if (!dir || !dir[0])
		return 0;
	char cur[PATH_MAX];
	size_t n = strlen(dir);
	if (n >= sizeof(cur))
		return 0;
	memcpy(cur, dir, n + 1);

	for (;;) {
		char probe[PATH_MAX + 32];
		snprintf(probe, sizeof(probe), "%s/compile_commands.json", cur);
		if (access(probe, R_OK) == 0)
			return 1;
		snprintf(probe, sizeof(probe), "%s/compile_flags.txt", cur);
		if (access(probe, R_OK) == 0)
			return 1;
		char *slash = strrchr(cur, '/');
		if (!slash || slash == cur)
			break;
		*slash = '\0';
	}
	return 0;
}

static void push_inc_if_exists(jval *flags, const char *dir)
{
	if (!dir || !dir[0])
		return;
	struct stat st;
	if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
		char flag[PATH_MAX + 4];
		int n = snprintf(flag, sizeof(flag), "-I%s", dir);
		if (n > 0 && (size_t)n < sizeof(flag))
			jval_push(flags, jval_new_str(flag));
	}
}

void inject_fallback_flags(jval *init_params, const char *root_path)
{
	if (!init_params || init_params->kind != J_OBJ)
		return;
	if (root_path && has_compilation_db_in_ancestors(root_path))
		return;

	jval *iopts = jval_get(init_params, "initializationOptions");
	if (iopts && iopts->kind == J_OBJ && jval_get(iopts, "fallbackFlags"))
		return; /* respect caller-provided fallbackFlags */

	if (!iopts || iopts->kind != J_OBJ) {
		iopts = jval_new(J_OBJ);
		jval_set(init_params, "initializationOptions", iopts);
	}

	jval *flags = jval_new(J_ARR);
	jval_push(flags, jval_new_str("-std=c11"));

	if (root_path && root_path[0]) {
		char cand[PATH_MAX];
		snprintf(cand, sizeof(cand), "%s/include", root_path);
		push_inc_if_exists(flags, cand);
		snprintf(cand, sizeof(cand), "%s/external/libbud/include", root_path);
		push_inc_if_exists(flags, cand);
		snprintf(cand, sizeof(cand), "%s/external/liblamina/include", root_path);
		push_inc_if_exists(flags, cand);
	}

	const char *home = getenv("HOME");
	if (home && home[0]) {
		char cand[PATH_MAX];
		snprintf(cand, sizeof(cand), "%s/.local/include", home);
		push_inc_if_exists(flags, cand);
	}

	jval_set(iopts, "fallbackFlags", flags);
}

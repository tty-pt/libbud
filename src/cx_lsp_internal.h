#ifndef CX_LSP_INTERNAL_H
#define CX_LSP_INTERNAL_H

#include <bud/cx.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ── Dynamic Byte Buffer ──────────────────────────────────────────────── */

typedef struct lsp_buf {
	char  *data;
	size_t len;
	size_t cap;
} lsp_buf;

void  lbuf_free(lsp_buf *b);
int   lbuf_reserve(lsp_buf *b, size_t extra);
int   lbuf_append(lsp_buf *b, const char *s, size_t n);
int   lbuf_str(lsp_buf *b, const char *s);
int   lbuf_ch(lsp_buf *b, char c);
int   lbuf_printf(lsp_buf *b, const char *fmt, ...);
char *xstrndup(const char *s, size_t n);

/* ── Minimal Owned JSON DOM (backed by jsmn.h) ────────────────────────── */

typedef enum jkind {
	J_NULL = 0,
	J_BOOL,
	J_NUM,
	J_STR,
	J_ARR,
	J_OBJ
} jkind;

typedef struct jval jval;

struct jval {
	jkind  kind;
	int    bval;
	char  *str;
	size_t slen;
	struct {
		jval  **items;
		size_t  len;
		size_t  cap;
	} arr;
	struct {
		char  **keys;
		jval  **vals;
		size_t  len;
		size_t  cap;
	} obj;
};

jval       *jval_new(jkind k);
jval       *jval_new_null(void);
jval       *jval_new_bool(int b);
jval       *jval_new_int(long long n);
jval       *jval_new_num_raw(const char *s, size_t n);
jval       *jval_new_strn(const char *s, size_t n);
jval       *jval_new_str(const char *s);
void        jval_free(jval *v);
int         jval_push(jval *arr, jval *item);
jval       *jval_get(const jval *obj, const char *key);
int         jval_set(jval *obj, const char *key, jval *val);
void        jval_del(jval *obj, const char *key);
long long   jval_as_int(const jval *v, long long def);
const char *jval_as_str(const jval *v);
jval       *jval_clone(const jval *v);
jval       *jval_parse(const char *js, size_t len);
void        jval_serialize(const jval *v, lsp_buf *b);

/* ── Line/Column <-> Byte Offset Index (UTF-16 LSP columns) ───────────── */

typedef struct line_index {
	uint32_t *offs;
	size_t    count;
} line_index;

void     line_index_free(line_index *li);
void     line_index_build(line_index *li, const char *s, size_t len);
uint32_t line_index_pos_to_off(
        const line_index *li, const char *s, size_t len,
        long long line, long long utf16_col);
void     line_index_off_to_pos(
        const line_index *li, const char *s, size_t len, uint32_t off,
        long long *out_line, long long *out_col);

jval *make_lsp_pos(long long line, long long col);
jval *make_lsp_range(long long sl, long long sc, long long el, long long ec);
jval *cx_slice_to_range(const line_index *li, const char *src, size_t len, cx_slice sl);

/* ── Open Document & Workspace Index ──────────────────────────────────── */

typedef struct cx_doc {
	char       *cx_uri;
	char       *c_uri;
	char       *cx_path;
	char       *cx_text;
	size_t      cx_len;
	int         version;
	cx_result   trans;
	line_index  cx_lines;
	line_index  c_lines;
} cx_doc;

typedef struct ws_comp {
	char    *name;
	char    *decl_sig;
	char    *doc;
	char   **prop_names;
	char   **prop_decls;
	size_t   prop_count;
	uint32_t line;      /* 0-based LSP line */
	uint32_t col;       /* 0-based UTF-16 col */
	uint32_t name_len;
	uint8_t  is_static;
	uint8_t  is_forward;
} ws_comp;

typedef struct ws_file {
	char    *path;
	char    *uri;
	int64_t  mtime_sec;
	ws_comp *comps;
	size_t   comp_count;
} ws_file;

typedef struct ws_index {
	ws_file *files;
	size_t   file_count;
	size_t   file_cap;
} ws_index;

/* Resolved component metadata (borrows strings from open doc or ws_index) */
typedef struct ws_comp_hit {
	const char  *name;
	const char  *uri;
	const char  *path;
	const cx_doc       *doc;       /* non-NULL if backed by an open cx_doc */
	const cx_comp_meta *doc_comp;  /* non-NULL if doc != NULL */
	const ws_comp      *ws_comp;   /* non-NULL if backed by ws_index */
	uint32_t     line;
	uint32_t     col;
	uint32_t     name_len;
	uint8_t      is_forward;
} ws_comp_hit;

/* ── Pending Request Tracking & Server State ──────────────────────────── */

typedef enum req_kind {
	REQ_OTHER = 0,
	REQ_INITIALIZE,
	REQ_DOC_REMAP,
	REQ_HOVER_SANITIZE,
	REQ_DEF_REMAP,
	REQ_DOC_SYMBOL_MERGE,
	REQ_RENAME_FILTER,
	REQ_PROP_RENAME_MERGE,
	REQ_CODEACTION_MERGE
} req_kind;

typedef struct pending_req {
	char    *id_key;
	req_kind kind;
	char    *cx_uri;
	jval    *extra; /* optional precomputed .cx-native result to merge */
} pending_req;

typedef struct lsp_server {
	int          client_in;
	int          client_out;
	int          clangd_in;
	int          clangd_out;
	pid_t        clangd_pid;
	int          no_clangd;
	int          shutting_down;

	char        *root_path;

	cx_doc     **docs;
	size_t       doc_count;
	size_t       doc_cap;

	ws_index     ws;

	pending_req *reqs;
	size_t       req_count;
	size_t       req_cap;
} lsp_server;

/* ── Core Helpers Exposed by cx-lsp.c ─────────────────────────────────── */

int     is_cx_uri(const char *uri);
int     is_cxh_uri(const char *uri);
int     is_cx_or_cxh_path(const char *path);
char   *uri_to_path(const char *uri);
char   *path_to_uri(const char *path);
cx_doc *find_doc_by_cx_uri(const lsp_server *srv, const char *uri);
cx_doc *find_doc_by_c_uri(const lsp_server *srv, const char *uri);
cx_doc *find_doc_by_path(const lsp_server *srv, const char *path);

int     map_cx_pos_to_c(
        const cx_doc *d, long long cx_line, long long cx_col,
        long long *out_c_line, long long *out_c_col);
int     map_c_pos_to_cx(
        const cx_doc *d, long long c_line, long long c_col,
        long long *out_cx_line, long long *out_cx_col);
int     cx_c_range_is_mapped(const cx_doc *d, const jval *range);
void    remap_json_c_to_cx(const lsp_server *srv, jval *v, const cx_doc *active_doc);

int     send_lsp_json(int fd, const jval *msg);
void    send_result_to_client(lsp_server *srv, const jval *id, jval *result);
void    track_req_ex(lsp_server *srv, const jval *id, req_kind kind,
                     const char *cx_uri, jval *extra);

/* ── Workspace Index & Includes (cx_lsp_index.c) ──────────────────────── */

void  ws_index_free(ws_index *ws);
void  ws_index_scan_root(lsp_server *srv, const char *root_path);
void  ws_index_update_doc(lsp_server *srv, const cx_doc *d);
int   ws_index_index_path(lsp_server *srv, const char *path);
void  ws_index_resolve_includes(lsp_server *srv, cx_doc *d);
int   ws_find_comp(
        const lsp_server *srv, const cx_doc *cur,
        const char *name, size_t nlen, ws_comp_hit *out);
int   ws_find_cx_for_generated_header(
        const lsp_server *srv, const char *hdr_uri,
        const char *sym_hint, char **out_cx_uri,
        long long *out_line, long long *out_col, long long *out_len);
void  inject_fallback_flags(jval *init_params, const char *root_path);

/* ── LSP Features (cx_lsp_features.c) ─────────────────────────────────── */

jval *cx_feat_completion(const lsp_server *srv, const cx_doc *d, long long line, long long col);
jval *cx_feat_hover(const lsp_server *srv, const cx_doc *d, long long line, long long col);
void  cx_feat_sanitize_hover(jval *hover_res);
void  cx_feat_process_diags(const lsp_server *srv, const cx_doc *d, jval *diags_arr);
jval *cx_feat_definition(const lsp_server *srv, const cx_doc *d, long long line, long long col);
void  cx_feat_remap_def_result(const lsp_server *srv, const cx_doc *d, jval *res);
jval *cx_feat_document_symbols(const cx_doc *d);
jval *cx_feat_merge_doc_symbols(const cx_doc *d, jval *cx_syms, jval *clangd_syms);
jval *cx_feat_folding_ranges(const cx_doc *d);
jval *cx_feat_linked_editing(const cx_doc *d, long long line, long long col);
jval *cx_feat_prepare_rename(const lsp_server *srv, const cx_doc *d, long long line, long long col);
jval *cx_feat_rename(
        const lsp_server *srv, const cx_doc *d,
        long long line, long long col, const char *new_name,
        int *out_merge_clangd);
void  cx_feat_filter_clangd_rename(const lsp_server *srv, jval *ws_edit);
void  cx_feat_merge_workspace_edits(jval *dst_ws_edit, jval *src_ws_edit);
jval *cx_feat_code_actions(
        const lsp_server *srv, const cx_doc *d,
        const jval *range, const jval *ctx_diags);
jval *cx_feat_merge_code_actions(const lsp_server *srv, jval *cx_actions, jval *clangd_actions);

#endif /* CX_LSP_INTERNAL_H */

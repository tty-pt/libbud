#ifndef BUD_CX_H
#define BUD_CX_H

/**
 * @file cx.h
 * @brief Pure-C `.cx` (JSX + component + portable closure) to C11 transpiler API.
 *
 * Designed as an in-memory library callable from both the `cx` CLI and `cx-lsp`
 * with zero external dependencies beyond libc.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Semantic kind of a source-mapping span between `.cx` and emitted `.c`.
 */
typedef enum cx_span_kind {
	CX_SPAN_PASSTHROUGH = 0, /**< Verbatim C slice */
	CX_SPAN_EXPR        = 1, /**< `{expr}` interpolation inside JSX */
	CX_SPAN_TAG         = 2, /**< Lowercase HTML/native tag name */
	CX_SPAN_COMPONENT   = 3, /**< Uppercase component tag name */
	CX_SPAN_ATTR        = 4, /**< JSX attribute or prop name */
	CX_SPAN_CLOSURE     = 5, /**< Lifted `[...](...) { ... }` closure body */
	CX_SPAN_LOOP        = 6  /**< `{for (...)}` or `{if (...)}` control header */
} cx_span_kind;

/**
 * @brief Fixed-width source mapping record (20 bytes).
 */
typedef struct cx_span {
	uint32_t cx_off;
	uint32_t cx_len;
	uint32_t c_off;
	uint32_t c_len;
	uint8_t  kind;
	uint8_t  _pad[3];
} cx_span;

/**
 * @brief Transpiler diagnostic with 1-based line/column and byte offset.
 */
typedef struct cx_diag {
	uint32_t line;
	uint32_t col;
	uint32_t off;
	char     msg[160];
} cx_diag;

/**
 * @brief Byte slice `[off, off + len)` into the original `.cx` source buffer.
 */
typedef struct cx_slice {
	uint32_t off;
	uint32_t len;
} cx_slice;

typedef enum cx_attr_val_kind {
	CX_ATTR_BOOL    = 0,
	CX_ATTR_STRING  = 1,
	CX_ATTR_EXPR    = 2,
	CX_ATTR_CLOSURE = 3
} cx_attr_val_kind;

typedef struct cx_comp_param_meta {
	cx_slice decl; /**< Full C parameter declaration, e.g. `const char *label` */
	cx_slice name; /**< Parameter identifier slice, e.g. `label` */
} cx_comp_param_meta;

typedef struct cx_comp_meta {
	cx_slice name;        /**< Component identifier */
	cx_slice decl;        /**< Header slice from `[static] component` through `)` */
	cx_slice body;        /**< `{ ... }` slice (`len == 0` for forward declarations) */
	cx_slice doc;         /**< Preceding `///` or doc comment slice (`len == 0` if none) */
	uint32_t param_start; /**< First index in `cx_result.comp_params` */
	uint32_t param_count; /**< Number of parameters */
	uint8_t  is_static;
	uint8_t  is_forward;
	uint8_t  _pad[2];
} cx_comp_meta;

typedef struct cx_attr_meta {
	cx_slice name;     /**< Attribute or prop name */
	cx_slice val;      /**< Inner value slice (inside `"..."` or `{...}`, empty for bool) */
	cx_slice full;     /**< Full attribute slice `name` through closing quote/brace */
	uint8_t  val_kind; /**< `cx_attr_val_kind` */
	uint8_t  _pad[3];
} cx_attr_meta;

typedef struct cx_elem_meta {
	cx_slice tag;             /**< Opening tag name (`len == 0` for fragments `<>`) */
	cx_slice close_tag;       /**< Closing tag name (`len == 0` if self-closing/unclosed) */
	cx_slice full;            /**< Full element slice `<...` through `>` or `</tag>` */
	uint32_t attr_start;      /**< First index in `cx_result.attrs` */
	uint32_t attr_count;      /**< Contiguous attribute count */
	int32_t  parent;          /**< Enclosing element index or -1 */
	int32_t  comp_idx;        /**< Enclosing component index or -1 */
	uint8_t  is_component;
	uint8_t  is_self_closing;
	uint8_t  in_loop_direct;  /**< 1 if direct JSX child of `{for (...)}` */
	uint8_t  has_key_attr;    /**< 1 if element specifies a `key` attribute/prop */
} cx_elem_meta;

typedef struct cx_closure_meta {
	cx_slice captures; /**< Capture list slice inside `[` ... `]` */
	cx_slice params;   /**< Parameter list slice inside `(` ... `)` */
	cx_slice body;     /**< Closure body slice `{` ... `}` */
	cx_slice full;     /**< Full closure slice `[` ... `}` */
	int32_t  comp_idx; /**< Enclosing component index or -1 */
	uint32_t id;       /**< Generated closure numeric ID (`__cx_fn_<id>`) */
} cx_closure_meta;

typedef struct cx_include_meta {
	cx_slice path;      /**< Included path inside `"..."` or `<...>` */
	cx_slice full;      /**< Full `#include ...` directive slice */
	uint8_t  is_system; /**< 0 for `"..."`, 1 for `<...>` */
	uint8_t  _pad[3];
} cx_include_meta;

/**
 * @brief Transpilation options.
 */
typedef struct cx_options {
	const char *filename;    /**< Source filename used in `#line` directives */
	int         emit_lines;  /**< Non-zero to emit `#line` directives */
	int         recover;     /**< Non-zero to tolerate incomplete edits (LSP mode) */
	int         build_index; /**< Non-zero to populate flat structural index arrays in `cx_result` */
} cx_options;

/**
 * @brief Output of `cx_transpile()`; release via `cx_result_free()`.
 */
typedef struct cx_result {
	char    *c_code;
	size_t   c_len;
	cx_span *spans;
	size_t   span_count;
	cx_diag *diags;
	size_t   diag_count;
	int      ok; /**< 1 if zero errors, 0 if any diagnostic was emitted */

	/* Optional structural index (populated when `opt->build_index != 0`) */
	cx_comp_meta       *comps;
	size_t              comp_count;
	cx_comp_param_meta *comp_params;
	size_t              comp_param_count;
	cx_elem_meta       *elems;
	size_t              elem_count;
	cx_attr_meta       *attrs;
	size_t              attr_count;
	cx_closure_meta    *closures;
	size_t              closure_count;
	cx_include_meta    *includes;
	size_t              include_count;
} cx_result;

/**
 * @brief Transpile `.cx` source text into portable C11 targeting `bud_reconcile.h`.
 * @return 0 on allocation/API success (check `out->ok` for syntax diagnostics), -1 on OOM/invalid args.
 */
int cx_transpile(const cx_options *opt, const char *src, size_t len, cx_result *out);

/**
 * @brief Free buffers owned by `res` and zero the struct.
 */
void cx_result_free(cx_result *res);

/**
 * @brief Write the span table to a JSON file (`--map`).
 * @return 0 on success, -1 on I/O error.
 */
int cx_write_map_json(const cx_result *res, const char *path);

/**
 * @brief Write a C header declaring props structs and render prototypes for all
 *        non-static components in `res` (requires `opt->build_index = 1`).
 * @return 0 on success, -1 on I/O error.
 */
int cx_write_header(const cx_result *res, const char *src, const char *path);

/**
 * @brief Map a byte offset in `.cx` source to the corresponding offset in emitted `.c`.
 * @return 1 if mapped, 0 if no span covers `cx_off`.
 */
int cx_map_cx_to_c(
        const cx_result *res, uint32_t cx_off,
        uint32_t *out_c_off, uint8_t *out_kind);

/**
 * @brief Map a byte offset in emitted `.c` back to the corresponding offset in `.cx`.
 * @return 1 if mapped, 0 if no span covers `c_off`.
 */
int cx_map_c_to_cx(
        const cx_result *res, uint32_t c_off,
        uint32_t *out_cx_off, uint8_t *out_kind);

#ifdef __cplusplus
}
#endif

#endif /* BUD_CX_H */

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
 * @brief Transpilation options.
 */
typedef struct cx_options {
	const char *filename;   /**< Source filename used in `#line` directives */
	int         emit_lines; /**< Non-zero to emit `#line` directives */
	int         recover;    /**< Non-zero to tolerate incomplete edits (LSP mode) */
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

#ifndef BUD_APP_H
#define BUD_APP_H

/**
 * @file bud_app.h
 * @brief Single-renderer app entry point (SSR native + WASM).
 *
 * WASM builds must expose bud_app_render() once; host-side patch
 * emission is declared here for both environments.
 */

#include "bud.h"
#include "bud_jsx.h"

#ifdef __wasm__

/**
 * @brief Application render entry point (WASM builds must define it).
 * @return Root node of the application tree.
 */
extern bud_node *bud_app_render(void);

/**
 * @brief Define bud_app_render() in terms of a single render function.
 * @param[in] render_fn Function returning the root node.
 */
#define BUD_APP_ROUTE(render_fn)                                               \
	bud_node *bud_app_render(void)                                         \
	{                                                                      \
		return render_fn();                                            \
	}

/**
 * @brief Host import: emit one patch operation to the browser adapter.
 */
__attribute__((import_module("env"), import_name("bud_host_emit_patch"))) void
bud_host_emit_patch(
        const char *op, size_t op_len, const char *a, size_t a_len,
        const char *b, size_t b_len, const char *c, size_t c_len);

/**
 * @brief Queue an attribute patch for a node.
 * @param[in] node  Target node.
 * @param[in] name  Attribute name.
 * @param[in] value Attribute value.
 */
void bud_patch_attr(bud_node *node, const char *name, const char *value);

/**
 * @brief Queue a text patch for a node.
 * @param[in] node  Target node.
 * @param[in] value Replacement text.
 */
void bud_patch_text(bud_node *node, const char *value);

/**
 * @brief Signal that application state has changed.
 */
void bud_app_set_state(void);

/**
 * @brief Current WASM runtime handle.
 * @return The runtime, or NULL before initialization.
 */
bud_runtime *wasm_get_runtime(void);

#else

/* Native stub — provided by libbud.so */
/**
 * @brief Emit one patch operation (native stub provided by libbud).
 */
void bud_host_emit_patch(
        const char *op, size_t op_len, const char *a, size_t a_len,
        const char *b, size_t b_len, const char *c, size_t c_len);

/**
 * @brief Queue an attribute patch for a node.
 * @param[in] node  Target node.
 * @param[in] name  Attribute name.
 * @param[in] value Attribute value.
 */
void bud_patch_attr(bud_node *node, const char *name, const char *value);

/**
 * @brief Queue a text patch for a node.
 * @param[in] node  Target node.
 * @param[in] value Replacement text.
 */
void bud_patch_text(bud_node *node, const char *value);

/**
 * @brief Signal that application state has changed.
 */
void bud_app_set_state(void);

/**
 * @brief Current WASM runtime handle.
 * @return The runtime, or NULL before initialization.
 */
bud_runtime *wasm_get_runtime(void);

#endif

#endif

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

#if __has_include(<ttypt/iso.h>)
#include <ttypt/iso.h>
#else
#if defined(__wasm__)
  #define ISO_EXPORT(ret, name, ...) \
      __attribute__((export_name(#name))) ret name(__VA_ARGS__)
  #define ISO_EXPORT_AS(ret, c_name, wasm_name, ...) \
      __attribute__((export_name(#wasm_name))) ret c_name(__VA_ARGS__)
  #define ISO_IMPORT(ret, module_name, name, ...) \
      __attribute__((import_module(#module_name), import_name(#name))) ret name(__VA_ARGS__)
#else
  #define ISO_EXPORT(ret, name, ...) \
      __attribute__((visibility("default"))) ret name(__VA_ARGS__)
  #define ISO_EXPORT_AS(ret, c_name, wasm_name, ...) \
      __attribute__((visibility("default"))) ret c_name(__VA_ARGS__)
  #define ISO_IMPORT(ret, module_name, name, ...) \
      ret name(__VA_ARGS__)
#endif
#endif

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
	ISO_EXPORT(bud_node *, bud_app_render, void)                           \
	{                                                                      \
		return render_fn();                                            \
	}

/**
 * @brief Host import: emit one patch operation to the browser adapter.
 */
ISO_IMPORT(void, bud, bud_host_emit_patch,
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

/**
 * @brief Standardized isomorphic frame loop contract (60fps animation tick).
 * Called by browser requestAnimationFrame.
 * @param[in] dt Delta time in seconds.
 * @param[in] width Viewport width in pixels.
 * @param[in] height Viewport height in pixels.
 */
void wasm_tick(float dt, int width, int height);

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

/**
 * @brief Standardized isomorphic frame loop contract (60fps animation tick).
 * Called by browser requestAnimationFrame.
 * @param[in] dt Delta time in seconds.
 * @param[in] width Viewport width in pixels.
 * @param[in] height Viewport height in pixels.
 */
void wasm_tick(float dt, int width, int height);

#endif

#endif

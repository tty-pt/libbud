#ifndef BUD_APP_H
#define BUD_APP_H

/**
 * @file bud_app.h
 * @brief Pure isomorphic C UI rendering contract for libbud.
 */

#include "bud.h"
#include "bud_jsx.h"

#if __has_include(<ttypt/iso.h>)
#include <ttypt/iso.h>
#elif !defined(ISO_EXPORT)
#define ISO_EXPORT(ret, name, ...) ret name(__VA_ARGS__)
#endif

/**
 * @brief Application render entry point.
 * @return Root node of the application tree.
 */
bud_node *bud_app_render(void);

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
 * @brief Queue an attribute patch for a node.
 */
void bud_patch_attr(bud_node *node, const char *name, const char *value);

/**
 * @brief Queue a text patch for a node.
 */
void bud_patch_text(bud_node *node, const char *value);

/**
 * @brief Signal that application state has changed.
 */
void bud_app_set_state(void);

/* Platform host function pointers (set by WASM adapter, NULL on native) */
extern void (*bud_host_fetch_fn)(const char *url, size_t len, int id);
extern void (*bud_host_log_fn)(const char *msg, size_t len);
extern void (*bud_host_set_location_fn)(const char *url, size_t len);

#endif /* BUD_APP_H */


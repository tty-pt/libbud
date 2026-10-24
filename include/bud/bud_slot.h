#ifndef BUD_SLOT_H
#define BUD_SLOT_H

#include "bud.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bud_slot_id {
	BUD_SLOT_SHELL_NAV    = 0,
	BUD_SLOT_SHELL_DRAWER = 1,
	BUD_SLOT_PRIMARY_VIEW = 2,
	BUD_SLOT_SIDE_PANEL   = 3,
	BUD_SLOT_STATUS_GAUGES= 4,
	BUD_SLOT_ACTIONS_DOCK = 5,
	BUD_SLOT_MODALS       = 6,
	BUD_SLOT_CUSTOM       = 100
} bud_slot_id_t;

/**
 * @brief Macro for declaring or documenting a named slot.
 */
#define BUD_SLOT_DEFINE(slot_id, slot_name) /* slot_id: slot_name */

typedef bud_node *(*bud_slot_render_fn)(int slot_id, const void *state);
typedef int (*bud_slot_fill_fn)(int slot_id, bud_node *parent, const void *state);

typedef struct bud_slot_contrib {
	int slot_id;
	int priority; /* lower = earlier (e.g. 0 before 10) */
	const char *name;
	bud_slot_render_fn render;
	bud_slot_fill_fn fill;
	struct bud_slot_contrib *next;
} bud_slot_contrib_t;

/**
 * @brief Register a slot contribution into the centralized libbud slot registry.
 * @return 0 on success, < 0 on failure.
 */
int bud_slot_register(const bud_slot_contrib_t *contrib);

/**
 * @brief Render the first non-NULL node from registered contributors for slot_id.
 * Contributors are consulted in ascending priority order.
 * @return bud_node or NULL if no contributor returned a node.
 */
bud_node *bud_slot_render(int slot_id, const void *state);

/**
 * @brief Append nodes from all registered contributors for slot_id into parent.
 * Contributors are processed in ascending priority order.
 * @return Number of nodes appended, or < 0 on error.
 */
int bud_slot_fill(int slot_id, bud_node *parent, const void *state);

/**
 * @brief Reset the centralized slot registry (primarily for test isolation).
 */
void bud_slot_reset(void);

/**
 * @brief Macro for automatic static constructor registration.
 */
#define BUD_SLOT_CONTRIBUTE(tag, ...) \
	static const bud_slot_contrib_t _bud_slot_##tag = { __VA_ARGS__ }; \
	static void __attribute__((constructor)) _bud_slot_reg_##tag(void) \
	{ bud_slot_register(&_bud_slot_##tag); }

#ifdef __cplusplus
}
#endif

#endif /* BUD_SLOT_H */

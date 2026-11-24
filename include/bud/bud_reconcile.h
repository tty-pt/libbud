#ifndef BUD_RECONCILE_H
#define BUD_RECONCILE_H

/**
 * @file bud_reconcile.h
 * @brief Host-agnostic reactive VDOM reconciler, hooks, and component runtime.
 *
 * Reconciles bud_node trees (including lazy BUD_NODE_COMPONENT nodes) into any
 * concrete UI host implementing bud_host_ops (browser DOM via WASM, native
 * desktop / WebGL via liblamina, or test mocks). Pure C, zero host deps.
 */

#include "bud.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque mounted reconciler instance.
 */
typedef struct bud_mount bud_mount;

/**
 * @brief Cleanup callback returned by a bud_effect_fn.
 */
typedef void (*bud_effect_cleanup_fn)(void *user);

/**
 * @brief Post-commit effect callback; may return an optional cleanup callback.
 */
typedef bud_effect_cleanup_fn (*bud_effect_fn)(void *user);

/**
 * @brief Host realization vtable (DOM, liblamina, or custom renderer).
 *
 * Ownership contract:
 *  - insert_before inserts or moves child before `before` (or appends if NULL)
 *    without destroying child.
 *  - remove_child detaches child from parent without freeing it.
 *  - destroy_node frees a detached host node and any host-side metadata.
 *  - clear_children (optional) destroys all children of parent in O(1) when
 *    no mounted subtree holds active effect cleanups.
 */
typedef struct bud_host_ops {
	void *(*create_el)(void *ctx, const char *tag);
	void *(*create_text)(void *ctx, const char *text, size_t len);
	void  (*set_text)(void *ctx, void *host_node, const char *text, size_t len);
	void  (*set_attr)(void *ctx, void *host_node, const char *name, const char *val);
	void  (*remove_attr)(void *ctx, void *host_node, const char *name);
	void  (*insert_before)(void *ctx, void *parent, void *child, void *before);
	void  (*remove_child)(void *ctx, void *parent, void *child);
	void  (*destroy_node)(void *ctx, void *host_node);
	void  (*set_event)(void *ctx, void *host_node, const char *event,
	                   int bubbles, bud_event_handler_fn fn, void *ud);
	void  (*clear_children)(void *ctx, void *parent);
} bud_host_ops;

/**
 * @brief Create a new reconciler mount targeting root_host.
 * @param[in] ops       Host vtable (copied internally; must remain valid functions).
 * @param[in] host_ctx  Opaque context forwarded to every host callback.
 * @param[in] root_host Concrete root container handle in the host.
 * @return Mounted reconciler handle, or NULL on allocation failure.
 */
bud_mount *bud_mount_new(
        const bud_host_ops *ops, void *host_ctx, void *root_host);

/**
 * @brief Reconcile root_vnode into the mount and run post-commit effects.
 *
 * Borrows root_vnode (caller retains ownership and may free or SSR-render it).
 * Pass NULL to unmount all children from root_host.
 *
 * @param[in] mount      Target mount.
 * @param[in] root_vnode Root virtual node tree (or NULL to clear).
 * @return 0 on success, -1 on error.
 */
int bud_mount_update(bud_mount *mount, const bud_node *root_vnode);

/**
 * @brief Re-render only dirty component subtrees (sorted parent-first) and run effects.
 * @param[in] mount Target mount.
 * @return Number of component cells re-rendered, or -1 on error.
 */
int bud_mount_flush(bud_mount *mount);

/**
 * @brief Non-zero when at least one mounted component cell is dirty.
 */
int bud_mount_is_dirty(const bud_mount *mount);

/**
 * @brief Return the root host container handle passed to bud_mount_new.
 */
void *bud_mount_root_host(const bud_mount *mount);

/**
 * @brief Unmount all cells (invoking effect cleanups and destroying host nodes).
 */
void bud_mount_unmount(bud_mount *mount);

/**
 * @brief Unmount and free the bud_mount handle.
 */
void bud_mount_free(bud_mount *mount);

/* ── Hooks API (valid inside BUD_NODE_COMPONENT render functions) ─────── */

/**
 * @brief Allocate or retrieve stable state storage for the current component cell.
 * @param[in] size Byte size of the state value.
 * @param[in] init Pointer to initial value bytes (or NULL for zero-init).
 * @return Stable pointer to the state payload (valid until component unmounts).
 */
void *bud_use_state_raw(size_t size, const void *init);

/**
 * @brief Update state value obtained from bud_use_state_raw and mark owner dirty.
 *
 * Performs byte-level equality bailout: if the value is unchanged, the owning
 * component cell is not marked dirty.
 *
 * @param[in] state_ptr Stable pointer previously returned by bud_use_state_raw.
 * @param[in] next_val  Pointer to new value bytes.
 * @param[in] size      Byte size of value (must match state slot size).
 * @return 1 if state changed and owner was dirtied, 0 if unchanged, -1 on error.
 */
int bud_set_state_raw(void *state_ptr, const void *next_val, size_t size);

/**
 * @brief Allocate or retrieve stable mutable ref storage (mutations do not re-render).
 */
void *bud_use_ref_raw(size_t size, const void *init);

/**
 * @brief Sentinel deps pointer meaning run effect after every commit.
 */
extern const char bud_deps_always_sentinel;
#define BUD_DEPS_ONCE   NULL, 0
#define BUD_DEPS_ALWAYS (&bud_deps_always_sentinel), 1

/**
 * @brief Schedule a post-commit effect with optional byte-compared dependency snapshot.
 *
 * @param[in] fn        Effect callback invoked after commit; may return cleanup fn.
 * @param[in] user      User pointer passed to fn (and to its returned cleanup fn).
 * @param[in] deps      Pointer to dependency struct/value, NULL (once on mount),
 *                      or BUD_DEPS_ALWAYS.
 * @param[in] deps_size Byte size of deps (0 when deps is NULL).
 */
void bud_use_effect(
        bud_effect_fn fn, void *user, const void *deps, size_t deps_size);

/**
 * @brief Typed state hook macro: `int *count = bud_use_state(int, 0);`
 */
#define bud_use_state(T, init_val) \
	((T *)bud_use_state_raw(sizeof(T), &(T){ init_val }))

/**
 * @brief Typed state setter macro: `bud_set(count, *count + 1);`
 */
#define bud_set(ptr, next_val) \
	bud_set_state_raw((ptr), &(__typeof__(*(ptr))){ next_val }, sizeof(*(ptr)))

/**
 * @brief Typed ref hook macro: `int *r = bud_use_ref(int, 0);`
 */
#define bud_use_ref(T, init_val) \
	((T *)bud_use_ref_raw(sizeof(T), &(T){ init_val }))

/**
 * @brief Convenience constructor for a keyed or unkeyed component VNode.
 */
#define bud_comp(fn, props_type, ...) \
	bud_component_vnode((fn), NULL, &(props_type){ __VA_ARGS__ }, sizeof(props_type))

/* ── Typed Text, Attr, Key & C11 _Generic Helpers (for C & .cx) ──────── */

bud_node *bud_text_int(int val);
bud_node *bud_text_uint(unsigned int val);
bud_node *bud_text_long(long val);
bud_node *bud_text_ulong(unsigned long val);
bud_node *bud_text_llong(long long val);
bud_node *bud_text_ullong(unsigned long long val);
bud_node *bud_text_size(size_t val);
bud_node *bud_text_double(double val);

int bud_set_attr_str(bud_node *node, const char *name, const char *val);
int bud_set_attr_bool(bud_node *node, const char *name, _Bool val);
int bud_set_attr_int(bud_node *node, const char *name, int val);
int bud_set_attr_uint(bud_node *node, const char *name, unsigned int val);
int bud_set_attr_long(bud_node *node, const char *name, long val);
int bud_set_attr_ulong(bud_node *node, const char *name, unsigned long val);
int bud_set_attr_llong(bud_node *node, const char *name, long long val);
int bud_set_attr_ullong(bud_node *node, const char *name, unsigned long long val);
int bud_set_attr_double(bud_node *node, const char *name, double val);

int bud_set_key_int(bud_node *node, int val);
int bud_set_key_uint(bud_node *node, unsigned int val);
int bud_set_key_long(bud_node *node, long val);
int bud_set_key_ulong(bud_node *node, unsigned long val);
int bud_set_key_llong(bud_node *node, long long val);
int bud_set_key_ullong(bud_node *node, unsigned long long val);

static inline bud_node *bud_child_identity(bud_node *node)
{
	return node;
}

static inline bud_node *bud_child_null(void *p)
{
	(void)p;
	return NULL;
}

static inline bud_node *bud_child_str(const char *s)
{
	return s ? bud_text(s) : NULL;
}

static inline bud_node *bud_child_bool(_Bool b)
{
	(void)b;
	return NULL;
}

static inline void bud_append_opt(bud_node *parent, bud_node *child)
{
	if (parent && child)
		bud_append(parent, child);
}

static inline int bud_set_attr_null(bud_node *node, const char *name, void *p)
{
	(void)node;
	(void)name;
	(void)p;
	return 0;
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/* Uses canonical C11 fundamental types (guaranteed distinct across LP64/ILP32/WASM32). */
#define bud_child_any(x) (_Generic((x), \
	bud_node *:         bud_child_identity, \
	void *:             bud_child_null, \
	const char *:       bud_child_str, \
	char *:             bud_child_str, \
	_Bool:              bud_child_bool, \
	int:                bud_text_int, \
	unsigned int:       bud_text_uint, \
	long:               bud_text_long, \
	unsigned long:      bud_text_ulong, \
	long long:          bud_text_llong, \
	unsigned long long: bud_text_ullong, \
	float:              bud_text_double, \
	double:             bud_text_double \
)((x)))

#define bud_set_attr_any(el, name, val) (_Generic((val), \
	const char *:       bud_set_attr_str, \
	char *:             bud_set_attr_str, \
	void *:             bud_set_attr_null, \
	_Bool:              bud_set_attr_bool, \
	int:                bud_set_attr_int, \
	unsigned int:       bud_set_attr_uint, \
	long:               bud_set_attr_long, \
	unsigned long:      bud_set_attr_ulong, \
	long long:          bud_set_attr_llong, \
	unsigned long long: bud_set_attr_ullong, \
	float:              bud_set_attr_double, \
	double:             bud_set_attr_double \
)((el), (name), (val)))

#define bud_set_key_any(el, val) (_Generic((val), \
	const char *:       bud_set_key, \
	char *:             bud_set_key, \
	int:                bud_set_key_int, \
	unsigned int:       bud_set_key_uint, \
	long:               bud_set_key_long, \
	unsigned long:      bud_set_key_ulong, \
	long long:          bud_set_key_llong, \
	unsigned long long: bud_set_key_ullong \
)((el), (val)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* BUD_RECONCILE_H */

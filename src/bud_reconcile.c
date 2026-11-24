#include "bud_internal.h"
#include "../include/bud/bud_reconcile.h"

#include <stddef.h>

const char bud_deps_always_sentinel = 1;

enum {
	BUD_CELL_DIRTY       = (1u << 0),
	BUD_CELL_HAS_CLEANUP = (1u << 1),
	BUD_CELL_MOUNTED     = (1u << 2)
};

enum {
	BUD_HOOK_STATE  = 1,
	BUD_HOOK_REF    = 2,
	BUD_HOOK_EFFECT = 3
};

typedef struct bud_cell bud_cell;

typedef struct bud_state_slot {
	bud_cell *owner;
	uint32_t  hook_idx;
	uint32_t  size;
	max_align_t payload[];
} bud_state_slot;

typedef struct bud_hook {
	uint8_t kind;
	bud_state_slot *slot;
	/* Effect fields */
	bud_effect_fn         effect_fn;
	bud_effect_cleanup_fn cleanup_fn;
	void                 *effect_user;
	void                 *deps;
	size_t                deps_size;
	uint8_t               deps_always;
	uint8_t               pending;
	uint8_t               initialized;
} bud_hook;

typedef struct bud_cell_attr {
	char *name;
	char *value;
	int   is_boolean;
} bud_cell_attr;

typedef struct bud_cell_listener {
	char                *event;
	int                  bubbles;
	bud_event_handler_fn fn;
	void                *user;
	size_t               user_size;
} bud_cell_listener;

struct bud_cell {
	bud_node_kind kind;
	char         *tag;
	char         *text;
	char         *key;

	bud_component_fn comp_fn;
	void            *comp_ctx;
	void            *comp_props;
	size_t           comp_props_size;

	bud_cell_attr     *attrs;
	size_t             attr_count;
	bud_cell_listener *listeners;
	size_t             listener_count;

	void     *host_node;
	void     *parent_host;
	bud_cell *parent;

	bud_cell **children;
	size_t     child_count;
	size_t     child_cap;

	bud_hook *hooks;
	size_t    hook_count;
	size_t    hook_cap;

	uint16_t   depth;
	uint8_t    flags;
	bud_mount *mount;
};

struct bud_mount {
	bud_host_ops ops;
	void        *host_ctx;
	void        *root_host;
	bud_cell    *root_cell;

	bud_cell **dirty;
	size_t     dirty_count;
	size_t     dirty_cap;

	bud_cell **pending_effects;
	size_t     effect_count;
	size_t     effect_cap;
};

/* ── Active hook rendering context (single-threaded / WASM-safe) ──────── */

typedef struct bud_ssr_frame {
	struct bud_ssr_frame *prev;
	void                **allocs;
	size_t                count;
	size_t                cap;
} bud_ssr_frame;

static bud_cell      *g_active_cell = NULL;
static size_t         g_active_hook_idx = 0;
static bud_ssr_frame *g_ssr_frame = NULL;

/* ── Internal helpers ─────────────────────────────────────────────────── */

static const char *vnode_key(const bud_node *vnode)
{
	if (!vnode)
		return NULL;
	if (vnode->key)
		return vnode->key;
	if (vnode->kind == BUD_NODE_ELEMENT)
		return bud_get_attr(vnode, "data-bud-key");
	return NULL;
}

static int str_eq_opt(const char *a, const char *b)
{
	if (a == b)
		return 1;
	if (!a || !b)
		return 0;
	return strcmp(a, b) == 0;
}

static void mount_queue_dirty(bud_mount *m, bud_cell *cell)
{
	if (!m || !cell || !(cell->flags & BUD_CELL_MOUNTED))
		return;
	if (cell->flags & BUD_CELL_DIRTY)
		return;
	cell->flags |= BUD_CELL_DIRTY;
	if (m->dirty_count >= m->dirty_cap) {
		size_t ncap = m->dirty_cap ? m->dirty_cap * 2 : 16;
		bud_cell **narr = (bud_cell **)realloc(
		        m->dirty, ncap * sizeof(*narr));
		if (!narr)
			return;
		m->dirty = narr;
		m->dirty_cap = ncap;
	}
	m->dirty[m->dirty_count++] = cell;
}

static void mount_queue_effect_cell(bud_mount *m, bud_cell *cell)
{
	size_t i;
	if (!m || !cell)
		return;
	for (i = 0; i < m->effect_count; i++) {
		if (m->pending_effects[i] == cell)
			return;
	}
	if (m->effect_count >= m->effect_cap) {
		size_t ncap = m->effect_cap ? m->effect_cap * 2 : 16;
		bud_cell **narr = (bud_cell **)realloc(
		        m->pending_effects, ncap * sizeof(*narr));
		if (!narr)
			return;
		m->pending_effects = narr;
		m->effect_cap = ncap;
	}
	m->pending_effects[m->effect_count++] = cell;
}

static int cell_has_own_cleanup(const bud_cell *c)
{
	size_t i;
	for (i = 0; i < c->hook_count; i++) {
		if (c->hooks[i].kind == BUD_HOOK_EFFECT &&
		    c->hooks[i].cleanup_fn != NULL)
		{
			return 1;
		}
	}
	return 0;
}

static void cell_recompute_cleanup_up(bud_cell *c)
{
	while (c) {
		int has = cell_has_own_cleanup(c);
		size_t i;
		if (!has) {
			for (i = 0; i < c->child_count; i++) {
				if (c->children[i]->flags & BUD_CELL_HAS_CLEANUP) {
					has = 1;
					break;
				}
			}
		}
		if (has) {
			if (c->flags & BUD_CELL_HAS_CLEANUP)
				break;
			c->flags |= BUD_CELL_HAS_CLEANUP;
		} else {
			if (!(c->flags & BUD_CELL_HAS_CLEANUP))
				break;
			c->flags &= (uint8_t)~BUD_CELL_HAS_CLEANUP;
		}
		c = c->parent;
	}
}

/* ── Host node navigation & anchoring ─────────────────────────────────── */

static void *cell_first_host(const bud_cell *cell)
{
	size_t i;
	if (!cell)
		return NULL;
	if (cell->host_node)
		return cell->host_node;
	for (i = 0; i < cell->child_count; i++) {
		void *h = cell_first_host(cell->children[i]);
		if (h)
			return h;
	}
	return NULL;
}

static void *cell_next_host(bud_cell *parent, size_t after_idx)
{
	while (parent) {
		size_t i;
		for (i = after_idx + 1; i < parent->child_count; i++) {
			void *h = cell_first_host(parent->children[i]);
			if (h)
				return h;
		}
		/* Concrete element/root boundary stops sibling search. */
		if (parent->host_node != NULL)
			return NULL;
		bud_cell *gp = parent->parent;
		if (!gp)
			return NULL;
		size_t p_idx = 0;
		for (i = 0; i < gp->child_count; i++) {
			if (gp->children[i] == parent) {
				p_idx = i;
				break;
			}
		}
		parent = gp;
		after_idx = p_idx;
	}
	return NULL;
}

static void cell_move_host(bud_mount *m, bud_cell *cell, void *before_host)
{
	size_t i;
	if (!cell)
		return;
	if (cell->host_node) {
		if (m->ops.insert_before) {
			m->ops.insert_before(
			        m->host_ctx, cell->parent_host,
			        cell->host_node, before_host);
		}
		return;
	}
	for (i = 0; i < cell->child_count; i++) {
		cell_move_host(m, cell->children[i], before_host);
	}
}

/* ── Cell attribute & listener diffing ────────────────────────────────── */

static void cell_free_attrs(bud_cell *cell)
{
	size_t i;
	for (i = 0; i < cell->attr_count; i++) {
		free(cell->attrs[i].name);
		free(cell->attrs[i].value);
	}
	free(cell->attrs);
	cell->attrs = NULL;
	cell->attr_count = 0;
}

static void cell_free_listeners(bud_cell *cell)
{
	size_t i;
	for (i = 0; i < cell->listener_count; i++) {
		free(cell->listeners[i].event);
		if (cell->listeners[i].user_size > 0)
			free(cell->listeners[i].user);
	}
	free(cell->listeners);
	cell->listeners = NULL;
	cell->listener_count = 0;
}

static const bud_cell_attr *
cell_find_attr(const bud_cell *cell, const char *name)
{
	size_t i;
	for (i = 0; i < cell->attr_count; i++) {
		if (strcmp(cell->attrs[i].name, name) == 0)
			return &cell->attrs[i];
	}
	return NULL;
}

static const bud_cell_listener *
cell_find_listener(const bud_cell *cell, const char *event)
{
	size_t i;
	for (i = 0; i < cell->listener_count; i++) {
		if (strcmp(cell->listeners[i].event, event) == 0)
			return &cell->listeners[i];
	}
	return NULL;
}

static void cell_patch_attrs(bud_mount *m, bud_cell *cell, const bud_node *vnode)
{
	size_t i;
	const bud_attr *a;
	size_t new_count = 0;
	bud_cell_attr *next_attrs = NULL;

	/* 1. Remove old attrs absent from new vnode */
	for (i = 0; i < cell->attr_count; i++) {
		if (!bud_attr_find((bud_node *)vnode, cell->attrs[i].name)) {
			if (m->ops.remove_attr) {
				m->ops.remove_attr(
				        m->host_ctx, cell->host_node,
				        cell->attrs[i].name);
			}
		}
	}

	/* 2. Count & build new attr cache, emitting only changed/added attrs */
	for (a = vnode->attrs; a; a = a->next)
		new_count++;

	if (new_count > 0) {
		next_attrs = (bud_cell_attr *)calloc(new_count, sizeof(*next_attrs));
		if (!next_attrs)
			return;
	}

	i = 0;
	for (a = vnode->attrs; a; a = a->next, i++) {
		const char *val = a->is_boolean ? "" : (a->value ? a->value : "");
		const bud_cell_attr *old = cell_find_attr(cell, a->name);
		if (!old || old->is_boolean != a->is_boolean ||
		    !str_eq_opt(old->value, val))
		{
			if (m->ops.set_attr) {
				m->ops.set_attr(
				        m->host_ctx, cell->host_node,
				        a->name, val);
			}
		}
		next_attrs[i].name = bud_strdup(a->name);
		next_attrs[i].value = bud_strdup(val);
		next_attrs[i].is_boolean = a->is_boolean;
	}

	cell_free_attrs(cell);
	cell->attrs = next_attrs;
	cell->attr_count = new_count;
}

static int listener_user_eq(const bud_cell_listener *old, const bud_listener *nw)
{
	if (old->user_size != nw->user_size)
		return 0;
	if (old->user_size == 0)
		return old->user == nw->user;
	if (!old->user || !nw->user)
		return old->user == nw->user;
	return memcmp(old->user, nw->user, old->user_size) == 0;
}

static void
cell_patch_listeners(bud_mount *m, bud_cell *cell, const bud_node *vnode)
{
	size_t i;
	const bud_listener *l;
	size_t new_count = 0;
	bud_cell_listener *next_ls = NULL;

	/* 1. Unbind removed events */
	for (i = 0; i < cell->listener_count; i++) {
		if (!bud_listener_find((bud_node *)vnode, cell->listeners[i].event)) {
			if (m->ops.set_event) {
				m->ops.set_event(
				        m->host_ctx, cell->host_node,
				        cell->listeners[i].event, 0, NULL, NULL);
			}
		}
	}

	/* 2. Build owned listener cache; re-emit set_event only on changes */
	for (l = vnode->listeners; l; l = l->next)
		new_count++;

	if (new_count > 0) {
		next_ls = (bud_cell_listener *)calloc(new_count, sizeof(*next_ls));
		if (!next_ls)
			return;
	}

	i = 0;
	for (l = vnode->listeners; l; l = l->next, i++) {
		const bud_cell_listener *old = cell_find_listener(cell, l->event);
		next_ls[i].event = bud_strdup(l->event);
		next_ls[i].bubbles = l->bubbles;
		next_ls[i].fn = l->handler;
		next_ls[i].user_size = l->user_size;
		if (l->user && l->user_size > 0) {
			next_ls[i].user = malloc(l->user_size);
			if (next_ls[i].user)
				memcpy(next_ls[i].user, l->user, l->user_size);
		} else {
			next_ls[i].user = l->user;
		}

		/* Always update host if event is new, changed, or user_size > 0
		 * (because the owned heap pointer address moves on re-copy). */
		int unchanged = (old != NULL && old->fn == l->handler &&
		                 old->bubbles == l->bubbles &&
		                 old->user_size == 0 && l->user_size == 0 &&
		                 old->user == l->user);
		if (!unchanged && old != NULL && old->fn == l->handler &&
		    old->bubbles == l->bubbles && listener_user_eq(old, l) &&
		    old->user_size > 0)
		{
			/* Reuse old buffer address so host pointer stays valid
			 * with zero set_event churn! */
			free(next_ls[i].user);
			next_ls[i].user = old->user;
			/* Steal ownership from old entry */
			((bud_cell_listener *)old)->user = NULL;
			((bud_cell_listener *)old)->user_size = 0;
			unchanged = 1;
		}

		if (!unchanged && m->ops.set_event) {
			m->ops.set_event(
			        m->host_ctx, cell->host_node,
			        next_ls[i].event, next_ls[i].bubbles,
			        next_ls[i].fn, next_ls[i].user);
		}
	}

	cell_free_listeners(cell);
	cell->listeners = next_ls;
	cell->listener_count = new_count;
}

/* ── Cell creation, unmount & destruction ─────────────────────────────── */

static bud_cell *cell_alloc(
        bud_mount *m, bud_cell *parent, void *parent_host, uint16_t depth)
{
	bud_cell *c = (bud_cell *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->mount = m;
	c->parent = parent;
	c->parent_host = parent_host;
	c->depth = depth;
	c->flags = BUD_CELL_MOUNTED;
	return c;
}

static void cell_free_hooks(bud_cell *cell, int run_cleanups)
{
	size_t i;
	for (i = 0; i < cell->hook_count; i++) {
		bud_hook *h = &cell->hooks[i];
		if (h->kind == BUD_HOOK_EFFECT) {
			if (run_cleanups && h->cleanup_fn) {
				h->cleanup_fn(h->effect_user);
			}
			free(h->deps);
		} else if (h->slot) {
			free(h->slot);
		}
	}
	free(cell->hooks);
	cell->hooks = NULL;
	cell->hook_count = 0;
	cell->hook_cap = 0;
}

static void cell_unmount(bud_mount *m, bud_cell *cell, int detach_host)
{
	size_t i;
	if (!cell)
		return;

	cell->flags &= (uint8_t)~(BUD_CELL_MOUNTED | BUD_CELL_DIRTY);

	/* Unmount children bottom-up; if we are detaching a concrete host
	 * element, children inside it only need metadata + effect cleanup and
	 * host destruction is owned by the parent's destroy_node unless the
	 * host needs explicit child teardown. Calling destroy_node on each
	 * leaf is safest if remove_child is done only on the top detached node,
	 * OR we unmount children cleanly before destroying the parent node. */
	for (i = 0; i < cell->child_count; i++) {
		cell_unmount(m, cell->children[i], cell->host_node == NULL ? detach_host : 0);
	}
	free(cell->children);
	cell->children = NULL;
	cell->child_count = 0;
	cell->child_cap = 0;

	cell_free_hooks(cell, 1);
	cell_free_attrs(cell);
	cell_free_listeners(cell);

	if (cell->host_node) {
		if (detach_host && m->ops.remove_child) {
			m->ops.remove_child(
			        m->host_ctx, cell->parent_host, cell->host_node);
		}
		if (detach_host && m->ops.destroy_node) {
			m->ops.destroy_node(m->host_ctx, cell->host_node);
		}
		cell->host_node = NULL;
	}

	free(cell->tag);
	free(cell->text);
	free(cell->key);
	free(cell->comp_props);
	free(cell);
}

/* Free metadata only (used after O(1) host->clear_children when no cleanups) */
static void cell_free_metadata_only(bud_cell *cell)
{
	size_t i;
	if (!cell)
		return;
	cell->flags = 0;
	for (i = 0; i < cell->child_count; i++) {
		cell_free_metadata_only(cell->children[i]);
	}
	free(cell->children);
	cell_free_hooks(cell, 0);
	cell_free_attrs(cell);
	cell_free_listeners(cell);
	free(cell->tag);
	free(cell->text);
	free(cell->key);
	free(cell->comp_props);
	free(cell);
}

/* ── Forward declarations for recursive mount/patch ───────────────────── */

static bud_cell *mount_vnode(
        bud_mount *m, const bud_node *vnode, bud_cell *parent,
        void *parent_host, void *before_host, uint16_t depth);

static void patch_cell(
        bud_mount *m, bud_cell *cell, const bud_node *vnode, void *before_host);

static void patch_children(
        bud_mount *m, bud_cell *parent, const bud_node *const *new_vnodes,
        size_t new_count);

/* ── Component execution & reconciliation ─────────────────────────────── */

static bud_node *run_component_cell(bud_cell *cell)
{
	bud_cell *prev_cell = g_active_cell;
	size_t    prev_idx  = g_active_hook_idx;
	bud_node *out;

	g_active_cell = cell;
	g_active_hook_idx = 0;
	cell->flags &= (uint8_t)~BUD_CELL_DIRTY;

	out = cell->comp_fn(cell->comp_ctx, cell->comp_props);

	g_active_cell = prev_cell;
	g_active_hook_idx = prev_idx;
	return out;
}

static void reconcile_component_cell(bud_mount *m, bud_cell *cell)
{
	bud_node *out = run_component_cell(cell);
	if (out) {
		const bud_node *arr[1] = { out };
		patch_children(m, cell, arr, 1);
		bud_free(out);
	} else {
		patch_children(m, cell, NULL, 0);
	}
}

/* ── Keyed LIS helper (O(k log k)) ────────────────────────────────────── */

static void compute_lis(const int *seq, size_t n, uint8_t *in_lis)
{
	size_t i;
	memset(in_lis, 0, n);
	if (n == 0)
		return;

	int    *tails    = (int *)malloc(n * sizeof(int));
	size_t *tail_pos = (size_t *)malloc(n * sizeof(size_t));
	int    *prev     = (int *)malloc(n * sizeof(int));
	if (!tails || !tail_pos || !prev) {
		free(tails);
		free(tail_pos);
		free(prev);
		return;
	}

	size_t len = 0;
	for (i = 0; i < n; i++) {
		int v = seq[i];
		if (v < 0) {
			prev[i] = -1;
			continue;
		}
		size_t lo = 0, hi = len;
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;
			if (tails[mid] < v)
				lo = mid + 1;
			else
				hi = mid;
		}
		tails[lo] = v;
		tail_pos[lo] = i;
		prev[i] = (lo > 0) ? (int)tail_pos[lo - 1] : -1;
		if (lo == len)
			len++;
	}

	if (len > 0) {
		int cur = (int)tail_pos[len - 1];
		while (cur >= 0) {
			in_lis[cur] = 1;
			cur = prev[cur];
		}
	}

	free(tails);
	free(tail_pos);
	free(prev);
}

/* ── Cell compatibility check ─────────────────────────────────────────── */

static int same_cell_vnode(const bud_cell *cell, const bud_node *vnode)
{
	if (!cell || !vnode)
		return 0;
	if (cell->kind != vnode->kind)
		return 0;
	if (!str_eq_opt(cell->key, vnode_key(vnode)))
		return 0;
	switch (cell->kind) {
	case BUD_NODE_ELEMENT:
		return str_eq_opt(cell->tag, vnode->tag);
	case BUD_NODE_COMPONENT:
		return cell->comp_fn == vnode->comp_fn;
	case BUD_NODE_TEXT:
	case BUD_NODE_RAW_HTML:
	case BUD_NODE_FRAGMENT:
		return 1;
	}
	return 0;
}

/* ── Collect linked children of a bud_node into a flat array ──────────── */

static const bud_node **collect_vnode_children(
        const bud_node *vnode, size_t *out_count,
        const bud_node **stack_buf, size_t stack_cap)
{
	const bud_node *ch;
	size_t n = 0;
	for (ch = vnode->first_child; ch; ch = ch->next_sibling)
		n++;
	*out_count = n;
	if (n == 0)
		return NULL;
	const bud_node **arr = (n <= stack_cap)
	        ? stack_buf
	        : (const bud_node **)malloc(n * sizeof(*arr));
	if (!arr) {
		*out_count = 0;
		return NULL;
	}
	size_t i = 0;
	for (ch = vnode->first_child; ch; ch = ch->next_sibling)
		arr[i++] = ch;
	return arr;
}

/* ── Mount a fresh bud_node into a new bud_cell ───────────────────────── */

static bud_cell *mount_vnode(
        bud_mount *m, const bud_node *vnode, bud_cell *parent,
        void *parent_host, void *before_host, uint16_t depth)
{
	if (!vnode)
		return NULL;

	bud_cell *cell = cell_alloc(m, parent, parent_host, depth);
	if (!cell)
		return NULL;

	cell->kind = vnode->kind;
	const char *k = vnode_key(vnode);
	if (k)
		cell->key = bud_strdup(k);

	switch (vnode->kind) {
	case BUD_NODE_TEXT:
	case BUD_NODE_RAW_HTML: {
		const char *txt = vnode->text ? vnode->text : "";
		size_t tlen = strlen(txt);
		cell->text = bud_strdup(txt);
		if (m->ops.create_text) {
			cell->host_node = m->ops.create_text(m->host_ctx, txt, tlen);
			if (cell->host_node && m->ops.insert_before) {
				m->ops.insert_before(
				        m->host_ctx, parent_host,
				        cell->host_node, before_host);
			}
		}
		break;
	}
	case BUD_NODE_ELEMENT: {
		cell->tag = bud_strdup(vnode->tag ? vnode->tag : "div");
		if (m->ops.create_el) {
			cell->host_node = m->ops.create_el(m->host_ctx, cell->tag);
		}
		if (cell->host_node) {
			cell_patch_attrs(m, cell, vnode);
			cell_patch_listeners(m, cell, vnode);
		}

		const bud_node *stack_buf[32];
		size_t n = 0;
		const bud_node **kids = collect_vnode_children(
		        vnode, &n, stack_buf, 32);
		if (n > 0) {
			cell->children = (bud_cell **)calloc(n, sizeof(*cell->children));
			cell->child_cap = n;
			for (size_t i = 0; i < n; i++) {
				bud_cell *ch = mount_vnode(
				        m, kids[i], cell, cell->host_node,
				        NULL, (uint16_t)(depth + 1));
				if (ch)
					cell->children[cell->child_count++] = ch;
			}
			if (kids != stack_buf)
				free((void *)kids);
		}

		if (cell->host_node && m->ops.insert_before) {
			m->ops.insert_before(
			        m->host_ctx, parent_host,
			        cell->host_node, before_host);
		}
		break;
	}
	case BUD_NODE_FRAGMENT: {
		const bud_node *stack_buf[32];
		size_t n = 0;
		const bud_node **kids = collect_vnode_children(
		        vnode, &n, stack_buf, 32);
		if (n > 0) {
			cell->children = (bud_cell **)calloc(n, sizeof(*cell->children));
			cell->child_cap = n;
			for (size_t i = 0; i < n; i++) {
				bud_cell *ch = mount_vnode(
				        m, kids[i], cell, parent_host,
				        before_host, (uint16_t)(depth + 1));
				if (ch)
					cell->children[cell->child_count++] = ch;
			}
			if (kids != stack_buf)
				free((void *)kids);
		}
		break;
	}
	case BUD_NODE_COMPONENT: {
		cell->comp_fn = vnode->comp_fn;
		cell->comp_ctx = vnode->comp_ctx;
		if (vnode->comp_props && vnode->comp_props_size > 0) {
			cell->comp_props = malloc(vnode->comp_props_size);
			if (cell->comp_props) {
				memcpy(cell->comp_props, vnode->comp_props,
				       vnode->comp_props_size);
				cell->comp_props_size = vnode->comp_props_size;
			}
		}
		bud_node *out = run_component_cell(cell);
		if (out) {
			cell->children = (bud_cell **)calloc(1, sizeof(*cell->children));
			cell->child_cap = 1;
			bud_cell *ch = mount_vnode(
			        m, out, cell, parent_host,
			        before_host, (uint16_t)(depth + 1));
			if (ch)
				cell->children[cell->child_count++] = ch;
			bud_free(out);
		}
		break;
	}
	}

	return cell;
}

/* ── Patch an existing bud_cell in place against a compatible vnode ───── */

static void patch_cell(
        bud_mount *m, bud_cell *cell, const bud_node *vnode, void *before_host)
{
	(void)before_host;
	switch (cell->kind) {
	case BUD_NODE_TEXT:
	case BUD_NODE_RAW_HTML: {
		const char *nt = vnode->text ? vnode->text : "";
		if (!str_eq_opt(cell->text, nt)) {
			free(cell->text);
			cell->text = bud_strdup(nt);
			if (m->ops.set_text && cell->host_node) {
				m->ops.set_text(
				        m->host_ctx, cell->host_node,
				        nt, strlen(nt));
			}
		}
		break;
	}
	case BUD_NODE_ELEMENT: {
		cell_patch_attrs(m, cell, vnode);
		cell_patch_listeners(m, cell, vnode);
		const bud_node *stack_buf[32];
		size_t n = 0;
		const bud_node **kids = collect_vnode_children(
		        vnode, &n, stack_buf, 32);
		patch_children(m, cell, kids, n);
		if (kids && kids != stack_buf)
			free((void *)kids);
		break;
	}
	case BUD_NODE_FRAGMENT: {
		const bud_node *stack_buf[32];
		size_t n = 0;
		const bud_node **kids = collect_vnode_children(
		        vnode, &n, stack_buf, 32);
		patch_children(m, cell, kids, n);
		if (kids && kids != stack_buf)
			free((void *)kids);
		break;
	}
	case BUD_NODE_COMPONENT: {
		cell->comp_ctx = vnode->comp_ctx;
		if (vnode->comp_props_size != cell->comp_props_size) {
			free(cell->comp_props);
			cell->comp_props = NULL;
			cell->comp_props_size = 0;
			if (vnode->comp_props && vnode->comp_props_size > 0) {
				cell->comp_props = malloc(vnode->comp_props_size);
				if (cell->comp_props) {
					memcpy(cell->comp_props, vnode->comp_props,
					       vnode->comp_props_size);
					cell->comp_props_size = vnode->comp_props_size;
				}
			}
		} else if (vnode->comp_props && vnode->comp_props_size > 0) {
			memcpy(cell->comp_props, vnode->comp_props,
			       vnode->comp_props_size);
		}
		reconcile_component_cell(m, cell);
		break;
	}
	}
}

/* ── 5-step child reconciliation (O(1) clear, prefix/suffix, keyed LIS) ─ */

static void patch_children(
        bud_mount *m, bud_cell *parent, const bud_node *const *new_vnodes,
        size_t new_count)
{
	bud_cell **old_arr = parent->children;
	size_t old_count = parent->child_count;
	void *host_parent = parent->host_node ? parent->host_node : parent->parent_host;
	uint16_t child_depth = (uint16_t)(parent->depth + 1);
	size_t i;

	/* Step 1: Empty target — O(1) bulk clear when safe */
	if (new_count == 0) {
		if (old_count == 0)
			return;
		if (m->ops.clear_children && parent->host_node != NULL &&
		    !(parent->flags & BUD_CELL_HAS_CLEANUP))
		{
			for (i = 0; i < old_count; i++)
				cell_free_metadata_only(old_arr[i]);
			free(old_arr);
			parent->children = NULL;
			parent->child_count = 0;
			parent->child_cap = 0;
			m->ops.clear_children(m->host_ctx, parent->host_node);
			return;
		}
		for (i = 0; i < old_count; i++)
			cell_unmount(m, old_arr[i], 1);
		free(old_arr);
		parent->children = NULL;
		parent->child_count = 0;
		parent->child_cap = 0;
		cell_recompute_cleanup_up(parent);
		return;
	}

	/* Step 2: Empty source — bulk mount */
	if (old_count == 0) {
		void *anchor = cell_next_host(parent, 0);
		if (parent->host_node != NULL)
			anchor = NULL;
		bud_cell **next_arr = (bud_cell **)calloc(new_count, sizeof(*next_arr));
		if (!next_arr)
			return;
		parent->children = next_arr;
		parent->child_cap = new_count;
		parent->child_count = 0;
		for (i = 0; i < new_count; i++) {
			bud_cell *ch = mount_vnode(
			        m, new_vnodes[i], parent, host_parent,
			        anchor, child_depth);
			if (ch)
				parent->children[parent->child_count++] = ch;
		}
		return;
	}

	/* Step 3: Common prefix & suffix trim */
	size_t s = 0;
	size_t oe = old_count;
	size_t ne = new_count;

	while (s < oe && s < ne && same_cell_vnode(old_arr[s], new_vnodes[s])) {
		patch_cell(m, old_arr[s], new_vnodes[s], NULL);
		s++;
	}

	while (oe > s && ne > s &&
	       same_cell_vnode(old_arr[oe - 1], new_vnodes[ne - 1]))
	{
		oe--;
		ne--;
		patch_cell(m, old_arr[oe], new_vnodes[ne], NULL);
	}

	/* Fast path: identical length and all matched by prefix+suffix */
	if (s == oe && s == ne)
		return;

	size_t rem_old = oe - s;
	size_t rem_new = ne - s;
	size_t suffix_len = old_count - oe;

	bud_cell **next_arr = (bud_cell **)calloc(new_count, sizeof(*next_arr));
	if (!next_arr)
		return;

	/* Copy matched prefix & suffix cells into final positions */
	for (i = 0; i < s; i++)
		next_arr[i] = old_arr[i];
	for (i = 0; i < suffix_len; i++)
		next_arr[ne + i] = old_arr[oe + i];

	/* Determine right-hand anchor for middle insertions */
	void *suffix_anchor = NULL;
	if (suffix_len > 0) {
		for (i = 0; i < suffix_len; i++) {
			suffix_anchor = cell_first_host(next_arr[ne + i]);
			if (suffix_anchor)
				break;
		}
	}
	if (!suffix_anchor && parent->host_node == NULL)
		suffix_anchor = cell_next_host(parent, old_count);

	/* Fast path: pure insertions (rem_old == 0) */
	if (rem_old == 0) {
		for (i = 0; i < rem_new; i++) {
			next_arr[s + i] = mount_vnode(
			        m, new_vnodes[s + i], parent, host_parent,
			        suffix_anchor, child_depth);
		}
		free(old_arr);
		parent->children = next_arr;
		parent->child_count = new_count;
		parent->child_cap = new_count;
		return;
	}

	/* Fast path: pure removals (rem_new == 0) */
	if (rem_new == 0) {
		for (i = 0; i < rem_old; i++)
			cell_unmount(m, old_arr[s + i], 1);
		free(old_arr);
		parent->children = next_arr;
		parent->child_count = new_count;
		parent->child_cap = new_count;
		cell_recompute_cleanup_up(parent);
		return;
	}

	/* Check whether any remaining old or new node is keyed */
	int has_keys = 0;
	for (i = 0; i < rem_old && !has_keys; i++) {
		if (old_arr[s + i]->key != NULL)
			has_keys = 1;
	}
	for (i = 0; i < rem_new && !has_keys; i++) {
		if (vnode_key(new_vnodes[s + i]) != NULL)
			has_keys = 1;
	}

	if (!has_keys) {
		/* Step 4a: Unkeyed positional diff */
		size_t common = rem_old < rem_new ? rem_old : rem_new;
		for (i = 0; i < common; i++) {
			bud_cell *oc = old_arr[s + i];
			const bud_node *nv = new_vnodes[s + i];
			if (same_cell_vnode(oc, nv)) {
				patch_cell(m, oc, nv, NULL);
				next_arr[s + i] = oc;
			} else {
				void *anchor = cell_first_host(oc);
				bud_cell *nc = mount_vnode(
				        m, nv, parent, host_parent,
				        anchor ? anchor : suffix_anchor,
				        child_depth);
				cell_unmount(m, oc, 1);
				next_arr[s + i] = nc;
			}
		}
		for (i = common; i < rem_old; i++)
			cell_unmount(m, old_arr[s + i], 1);
		for (i = common; i < rem_new; i++) {
			next_arr[s + i] = mount_vnode(
			        m, new_vnodes[s + i], parent, host_parent,
			        suffix_anchor, child_depth);
		}
		free(old_arr);
		parent->children = next_arr;
		parent->child_count = new_count;
		parent->child_cap = new_count;
		cell_recompute_cleanup_up(parent);
		return;
	}

	/* Step 4b: Keyed LIS reconciliation */
	uint8_t *old_used   = (uint8_t *)calloc(rem_old, 1);
	int     *old_idx_of = (int *)malloc(rem_new * sizeof(int));
	uint8_t *in_lis     = (uint8_t *)calloc(rem_new, 1);
	if (!old_used || !old_idx_of || !in_lis) {
		free(old_used);
		free(old_idx_of);
		free(in_lis);
		free(next_arr);
		return;
	}

	/* Match each remaining new node to an old cell by key (or positional unkeyed) */
	size_t matched_count = 0;
	for (i = 0; i < rem_new; i++) {
		const bud_node *nv = new_vnodes[s + i];
		const char *nk = vnode_key(nv);
		int found = -1;
		if (nk) {
			for (size_t j = 0; j < rem_old; j++) {
				if (old_used[j])
					continue;
				if (old_arr[s + j]->key &&
				    strcmp(old_arr[s + j]->key, nk) == 0 &&
				    same_cell_vnode(old_arr[s + j], nv))
				{
					found = (int)j;
					break;
				}
			}
		} else {
			for (size_t j = 0; j < rem_old; j++) {
				if (old_used[j])
					continue;
				if (!old_arr[s + j]->key &&
				    same_cell_vnode(old_arr[s + j], nv))
				{
					found = (int)j;
					break;
				}
			}
		}

		old_idx_of[i] = found;
		if (found >= 0) {
			old_used[found] = 1;
			next_arr[s + i] = old_arr[s + found];
			matched_count++;
		}
	}

	/* Zero-match full replace fast path */
	if (matched_count == 0 && s == 0 && suffix_len == 0 &&
	    m->ops.clear_children && parent->host_node != NULL &&
	    !(parent->flags & BUD_CELL_HAS_CLEANUP))
	{
		for (i = 0; i < rem_old; i++)
			cell_free_metadata_only(old_arr[i]);
		m->ops.clear_children(m->host_ctx, parent->host_node);
		for (i = 0; i < rem_new; i++) {
			next_arr[i] = mount_vnode(
			        m, new_vnodes[i], parent, host_parent,
			        NULL, child_depth);
		}
		free(old_used);
		free(old_idx_of);
		free(in_lis);
		free(old_arr);
		parent->children = next_arr;
		parent->child_count = new_count;
		parent->child_cap = new_count;
		return;
	}

	/* Unmount unmatched old cells first */
	for (i = 0; i < rem_old; i++) {
		if (!old_used[i])
			cell_unmount(m, old_arr[s + i], 1);
	}

	/* Patch matched cells in place */
	for (i = 0; i < rem_new; i++) {
		if (old_idx_of[i] >= 0) {
			patch_cell(m, next_arr[s + i], new_vnodes[s + i], NULL);
		}
	}

	/* Compute LIS over old indices; move/mount right-to-left for minimal moves */
	compute_lis(old_idx_of, rem_new, in_lis);

	void *anchor = suffix_anchor;
	for (size_t ri = rem_new; ri > 0; ri--) {
		size_t idx = ri - 1;
		if (old_idx_of[idx] < 0) {
			next_arr[s + idx] = mount_vnode(
			        m, new_vnodes[s + idx], parent, host_parent,
			        anchor, child_depth);
		} else if (!in_lis[idx]) {
			cell_move_host(m, next_arr[s + idx], anchor);
		}
		void *first = cell_first_host(next_arr[s + idx]);
		if (first)
			anchor = first;
	}

	free(old_used);
	free(old_idx_of);
	free(in_lis);
	free(old_arr);
	parent->children = next_arr;
	parent->child_count = new_count;
	parent->child_cap = new_count;
	cell_recompute_cleanup_up(parent);
}

/* ── Post-commit effects execution ────────────────────────────────────── */

static void mount_run_effects(bud_mount *m)
{
	if (!m || m->effect_count == 0)
		return;

	bud_cell **batch = m->pending_effects;
	size_t count = m->effect_count;
	m->pending_effects = NULL;
	m->effect_count = 0;
	m->effect_cap = 0;

	for (size_t i = 0; i < count; i++) {
		bud_cell *c = batch[i];
		if (!(c->flags & BUD_CELL_MOUNTED))
			continue;
		int cleanup_changed = 0;
		for (size_t j = 0; j < c->hook_count; j++) {
			bud_hook *h = &c->hooks[j];
			if (h->kind != BUD_HOOK_EFFECT || !h->pending)
				continue;
			h->pending = 0;
			int had_cleanup = (h->cleanup_fn != NULL);
			if (h->cleanup_fn) {
				h->cleanup_fn(h->effect_user);
				h->cleanup_fn = NULL;
			}
			if (h->effect_fn) {
				h->cleanup_fn = h->effect_fn(h->effect_user);
			}
			if (had_cleanup != (h->cleanup_fn != NULL))
				cleanup_changed = 1;
		}
		if (cleanup_changed)
			cell_recompute_cleanup_up(c);
	}
	free(batch);
}

/* ── Public Mount Lifecycle API ───────────────────────────────────────── */

bud_mount *bud_mount_new(
        const bud_host_ops *ops, void *host_ctx, void *root_host)
{
	if (!ops)
		return NULL;
	bud_mount *m = (bud_mount *)calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->ops = *ops;
	m->host_ctx = host_ctx;
	m->root_host = root_host;
	m->root_cell = cell_alloc(m, NULL, root_host, 0);
	if (!m->root_cell) {
		free(m);
		return NULL;
	}
	m->root_cell->kind = BUD_NODE_ELEMENT;
	m->root_cell->host_node = root_host;
	return m;
}

int bud_mount_update(bud_mount *mount, const bud_node *root_vnode)
{
	if (!mount || !mount->root_cell)
		return -1;

	if (root_vnode) {
		const bud_node *arr[1] = { root_vnode };
		patch_children(mount, mount->root_cell, arr, 1);
	} else {
		patch_children(mount, mount->root_cell, NULL, 0);
	}

	mount_run_effects(mount);
	return 0;
}

int bud_mount_flush(bud_mount *mount)
{
	int total_rendered = 0;
	int round;

	if (!mount)
		return -1;

	for (round = 0; round < 32 && mount->dirty_count > 0; round++) {
		bud_cell **batch = mount->dirty;
		size_t count = mount->dirty_count;
		mount->dirty = NULL;
		mount->dirty_count = 0;
		mount->dirty_cap = 0;

		/* Insertion sort ascending by depth (parent before child) */
		for (size_t i = 1; i < count; i++) {
			bud_cell *key = batch[i];
			size_t j = i;
			while (j > 0 && batch[j - 1]->depth > key->depth) {
				batch[j] = batch[j - 1];
				j--;
			}
			batch[j] = key;
		}

		for (size_t i = 0; i < count; i++) {
			bud_cell *c = batch[i];
			if (!(c->flags & BUD_CELL_MOUNTED))
				continue;
			if (!(c->flags & BUD_CELL_DIRTY))
				continue;
			if (c->kind == BUD_NODE_COMPONENT) {
				reconcile_component_cell(mount, c);
				total_rendered++;
			} else {
				c->flags &= (uint8_t)~BUD_CELL_DIRTY;
			}
		}

		free(batch);
		mount_run_effects(mount);
	}

	return total_rendered;
}

int bud_mount_is_dirty(const bud_mount *mount)
{
	return (mount && mount->dirty_count > 0) ? 1 : 0;
}

void *bud_mount_root_host(const bud_mount *mount)
{
	return mount ? mount->root_host : NULL;
}

void bud_mount_unmount(bud_mount *mount)
{
	if (!mount || !mount->root_cell)
		return;
	patch_children(mount, mount->root_cell, NULL, 0);
	free(mount->dirty);
	mount->dirty = NULL;
	mount->dirty_count = 0;
	mount->dirty_cap = 0;
	free(mount->pending_effects);
	mount->pending_effects = NULL;
	mount->effect_count = 0;
	mount->effect_cap = 0;
}

void bud_mount_free(bud_mount *mount)
{
	if (!mount)
		return;
	bud_mount_unmount(mount);
	/* Do not destroy caller-owned root_host; only free root_cell shell */
	if (mount->root_cell) {
		mount->root_cell->host_node = NULL;
		cell_unmount(mount, mount->root_cell, 0);
		mount->root_cell = NULL;
	}
	free(mount);
}

/* ── Hooks Implementation ─────────────────────────────────────────────── */

static bud_hook *ensure_hook(bud_cell *cell, size_t idx, uint8_t kind)
{
	if (idx >= cell->hook_cap) {
		size_t ncap = cell->hook_cap ? cell->hook_cap * 2 : 4;
		while (ncap <= idx)
			ncap *= 2;
		bud_hook *nh = (bud_hook *)realloc(
		        cell->hooks, ncap * sizeof(*nh));
		if (!nh)
			return NULL;
		memset(nh + cell->hook_cap, 0,
		       (ncap - cell->hook_cap) * sizeof(*nh));
		cell->hooks = nh;
		cell->hook_cap = ncap;
	}
	if (idx >= cell->hook_count) {
		cell->hook_count = idx + 1;
		cell->hooks[idx].kind = kind;
	}
	return &cell->hooks[idx];
}

static void *ssr_alloc_slot(size_t size, const void *init)
{
	bud_state_slot *slot = (bud_state_slot *)calloc(
	        1, offsetof(bud_state_slot, payload) + size);
	if (!slot)
		return NULL;
	slot->size = (uint32_t)size;
	if (init && size > 0)
		memcpy(slot->payload, init, size);
	if (g_ssr_frame) {
		if (g_ssr_frame->count >= g_ssr_frame->cap) {
			size_t ncap = g_ssr_frame->cap ? g_ssr_frame->cap * 2 : 8;
			void **narr = (void **)realloc(
			        g_ssr_frame->allocs, ncap * sizeof(*narr));
			if (narr) {
				g_ssr_frame->allocs = narr;
				g_ssr_frame->cap = ncap;
			}
		}
		if (g_ssr_frame->count < g_ssr_frame->cap)
			g_ssr_frame->allocs[g_ssr_frame->count++] = slot;
	}
	return slot->payload;
}

void *bud_use_state_raw(size_t size, const void *init)
{
	if (!g_active_cell) {
		if (g_ssr_frame)
			return ssr_alloc_slot(size, init);
		return NULL;
	}

	size_t idx = g_active_hook_idx++;
	bud_hook *h = ensure_hook(g_active_cell, idx, BUD_HOOK_STATE);
	if (!h)
		return NULL;

	if (!h->slot) {
		bud_state_slot *slot = (bud_state_slot *)calloc(
		        1, offsetof(bud_state_slot, payload) + size);
		if (!slot)
			return NULL;
		slot->owner = g_active_cell;
		slot->hook_idx = (uint32_t)idx;
		slot->size = (uint32_t)size;
		if (init && size > 0)
			memcpy(slot->payload, init, size);
		h->slot = slot;
	}
	return h->slot->payload;
}

int bud_set_state_raw(void *state_ptr, const void *next_val, size_t size)
{
	bud_state_slot *slot;
	if (!state_ptr || !next_val || size == 0)
		return -1;

	slot = (bud_state_slot *)((char *)state_ptr -
	                          offsetof(bud_state_slot, payload));
	if (slot->size != size)
		return -1;
	if (memcmp(slot->payload, next_val, size) == 0)
		return 0;

	memcpy(slot->payload, next_val, size);
	if (slot->owner && slot->owner->mount)
		mount_queue_dirty(slot->owner->mount, slot->owner);
	return 1;
}

void *bud_use_ref_raw(size_t size, const void *init)
{
	if (!g_active_cell) {
		if (g_ssr_frame)
			return ssr_alloc_slot(size, init);
		return NULL;
	}

	size_t idx = g_active_hook_idx++;
	bud_hook *h = ensure_hook(g_active_cell, idx, BUD_HOOK_REF);
	if (!h)
		return NULL;

	if (!h->slot) {
		bud_state_slot *slot = (bud_state_slot *)calloc(
		        1, offsetof(bud_state_slot, payload) + size);
		if (!slot)
			return NULL;
		slot->owner = g_active_cell;
		slot->hook_idx = (uint32_t)idx;
		slot->size = (uint32_t)size;
		if (init && size > 0)
			memcpy(slot->payload, init, size);
		h->slot = slot;
	}
	return h->slot->payload;
}

void bud_use_effect(
        bud_effect_fn fn, void *user, const void *deps, size_t deps_size)
{
	if (!g_active_cell)
		return;

	size_t idx = g_active_hook_idx++;
	bud_hook *h = ensure_hook(g_active_cell, idx, BUD_HOOK_EFFECT);
	if (!h)
		return;

	int is_always = (deps == &bud_deps_always_sentinel);
	int should_run = 0;

	if (!h->initialized) {
		should_run = 1;
		h->initialized = 1;
	} else if (is_always) {
		should_run = 1;
	} else if (deps == NULL || deps_size == 0) {
		/* BUD_DEPS_ONCE: already ran on mount */
		should_run = 0;
	} else if (h->deps_size != deps_size || !h->deps ||
	           memcmp(h->deps, deps, deps_size) != 0)
	{
		should_run = 1;
	}

	h->effect_fn = fn;
	h->effect_user = user;
	h->deps_always = (uint8_t)is_always;

	if (should_run) {
		free(h->deps);
		h->deps = NULL;
		h->deps_size = 0;
		if (!is_always && deps && deps_size > 0) {
			h->deps = malloc(deps_size);
			if (h->deps) {
				memcpy(h->deps, deps, deps_size);
				h->deps_size = deps_size;
			}
		}
		h->pending = 1;
		mount_queue_effect_cell(g_active_cell->mount, g_active_cell);
	}
}

/* ── Stateless SSR Component Expansion ────────────────────────────────── */

static bud_node *bud_reconcile_ssr_expand(const bud_node *node)
{
	bud_ssr_frame frame;
	bud_node *out;

	if (!node || node->kind != BUD_NODE_COMPONENT || !node->comp_fn)
		return NULL;

	memset(&frame, 0, sizeof(frame));
	frame.prev = g_ssr_frame;
	g_ssr_frame = &frame;

	out = node->comp_fn(node->comp_ctx, node->comp_props);

	g_ssr_frame = frame.prev;
	for (size_t i = 0; i < frame.count; i++)
		free(frame.allocs[i]);
	free(frame.allocs);
	return out;
}

static void __attribute__((constructor)) bud_reconcile_register_ssr(void)
{
	bud_ssr_expand_component_hook = bud_reconcile_ssr_expand;
}

/* ── Typed Text, Attr & Key Helpers (for C11 _Generic macros) ─────────── */

bud_node *bud_text_int(int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", val);
	return bud_text(buf);
}

bud_node *bud_text_uint(unsigned int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", val);
	return bud_text(buf);
}

bud_node *bud_text_long(long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", val);
	return bud_text(buf);
}

bud_node *bud_text_ulong(unsigned long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lu", val);
	return bud_text(buf);
}

bud_node *bud_text_llong(long long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld", val);
	return bud_text(buf);
}

bud_node *bud_text_ullong(unsigned long long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%llu", val);
	return bud_text(buf);
}

bud_node *bud_text_size(size_t val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%zu", val);
	return bud_text(buf);
}

bud_node *bud_text_double(double val)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%g", val);
	return bud_text(buf);
}

int bud_set_attr_str(bud_node *node, const char *name, const char *val)
{
	if (!val)
		return 0;
	return bud_set_attr(node, name, val);
}

int bud_set_attr_bool(bud_node *node, const char *name, _Bool val)
{
	if (!val)
		return 0;
	return bud_set_attr(node, name, "");
}

int bud_set_attr_int(bud_node *node, const char *name, int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_attr_uint(bud_node *node, const char *name, unsigned int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_attr_long(bud_node *node, const char *name, long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_attr_ulong(bud_node *node, const char *name, unsigned long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lu", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_attr_llong(bud_node *node, const char *name, long long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_attr_ullong(bud_node *node, const char *name, unsigned long long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%llu", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_attr_double(bud_node *node, const char *name, double val)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%g", val);
	return bud_set_attr(node, name, buf);
}

int bud_set_key_int(bud_node *node, int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", val);
	return bud_set_key(node, buf);
}

int bud_set_key_uint(bud_node *node, unsigned int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u", val);
	return bud_set_key(node, buf);
}

int bud_set_key_long(bud_node *node, long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", val);
	return bud_set_key(node, buf);
}

int bud_set_key_ulong(bud_node *node, unsigned long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lu", val);
	return bud_set_key(node, buf);
}

int bud_set_key_llong(bud_node *node, long long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld", val);
	return bud_set_key(node, buf);
}

int bud_set_key_ullong(bud_node *node, unsigned long long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%llu", val);
	return bud_set_key(node, buf);
}

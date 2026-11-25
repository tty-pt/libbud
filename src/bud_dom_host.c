#include "../include/bud/bud_dom_host.h"
#include <stdlib.h>
#include <string.h>

#if __has_include(<ttypt/iso.h>)
#include <ttypt/iso.h>
#else
#ifdef __wasm__
#define ISO_IMPORT(ret, mod, name, ...) \
	__attribute__((import_module(#mod), import_name(#name))) ret name(__VA_ARGS__)
#define ISO_EXPORT(ret, name, ...) \
	__attribute__((export_name(#name))) ret name(__VA_ARGS__)
#else
#define ISO_IMPORT(ret, mod, name, ...) ret name(__VA_ARGS__)
#define ISO_EXPORT(ret, name, ...) ret name(__VA_ARGS__)
#endif
#endif

#ifdef __wasm__
ISO_IMPORT(uint32_t, bud, bud_dom_create_el, const char *tag, size_t tag_len);
ISO_IMPORT(uint32_t, bud, bud_dom_create_text, const char *text, size_t len);
ISO_IMPORT(void, bud, bud_dom_set_text, uint32_t node, const char *text, size_t len);
ISO_IMPORT(void, bud, bud_dom_set_attr, uint32_t node, const char *name, size_t name_len,
           const char *val, size_t val_len);
ISO_IMPORT(void, bud, bud_dom_remove_attr, uint32_t node, const char *name, size_t name_len);
ISO_IMPORT(void, bud, bud_dom_insert_before, uint32_t parent, uint32_t child, uint32_t before);
ISO_IMPORT(void, bud, bud_dom_remove_child, uint32_t parent, uint32_t child);
ISO_IMPORT(void, bud, bud_dom_destroy_node, uint32_t node);
ISO_IMPORT(void, bud, bud_dom_set_event, uint32_t node, const char *event, size_t event_len,
           int bubbles, uint32_t handler_id);
ISO_IMPORT(void, bud, bud_dom_clear_children, uint32_t parent);
#endif

typedef struct bud_dom_listener {
	uint32_t             node_id;
	char                *event;
	int                  bubbles;
	bud_event_handler_fn fn;
	void                *ud;
	uint8_t              active;
} bud_dom_listener_t;

static bud_dom_listener_t *g_listeners = NULL;
static uint32_t            g_listeners_cap = 0;

static char *dom_strdup(const char *s)
{
	if (!s) return NULL;
	size_t n = strlen(s);
	char *p = (char *)malloc(n + 1);
	if (p) memcpy(p, s, n + 1);
	return p;
}

static uint32_t listener_register(uint32_t node_id, const char *event, int bubbles,
                                  bud_event_handler_fn fn, void *ud)
{
	uint32_t free_slot = 0;
	for (uint32_t i = 1; i < g_listeners_cap; i++) {
		if (g_listeners[i].active) {
			if (g_listeners[i].node_id == node_id &&
			    g_listeners[i].event &&
			    strcmp(g_listeners[i].event, event) == 0) {
				if (!fn) {
					free(g_listeners[i].event);
					g_listeners[i].event = NULL;
					g_listeners[i].fn = NULL;
					g_listeners[i].ud = NULL;
					g_listeners[i].active = 0;
					return 0;
				}
				g_listeners[i].bubbles = bubbles;
				g_listeners[i].fn = fn;
				g_listeners[i].ud = ud;
				return i;
			}
		} else if (free_slot == 0) {
			free_slot = i;
		}
	}

	if (!fn)
		return 0;

	if (free_slot == 0) {
		uint32_t ncap = g_listeners_cap ? g_listeners_cap * 2u : 64u;
		bud_dom_listener_t *nl = (bud_dom_listener_t *)realloc(
			g_listeners, ncap * sizeof(*nl));
		if (!nl)
			return 0;
		memset(nl + g_listeners_cap, 0, (ncap - g_listeners_cap) * sizeof(*nl));
		free_slot = g_listeners_cap ? g_listeners_cap : 1u;
		g_listeners = nl;
		g_listeners_cap = ncap;
	}

	g_listeners[free_slot].node_id = node_id;
	g_listeners[free_slot].event = dom_strdup(event);
	g_listeners[free_slot].bubbles = bubbles;
	g_listeners[free_slot].fn = fn;
	g_listeners[free_slot].ud = ud;
	g_listeners[free_slot].active = 1;
	return free_slot;
}

static void listener_remove_node(uint32_t node_id)
{
	for (uint32_t i = 1; i < g_listeners_cap; i++) {
		if (g_listeners[i].active && g_listeners[i].node_id == node_id) {
			free(g_listeners[i].event);
			g_listeners[i].event = NULL;
			g_listeners[i].fn = NULL;
			g_listeners[i].ud = NULL;
			g_listeners[i].active = 0;
		}
	}
}

void bud_dom_host_reset(void)
{
	for (uint32_t i = 1; i < g_listeners_cap; i++) {
		free(g_listeners[i].event);
	}
	free(g_listeners);
	g_listeners = NULL;
	g_listeners_cap = 0;
	bud_dom_set_event_payload("");
}

ISO_EXPORT(int, bud_dom_dispatch, uint32_t handler_id, const char *payload, size_t payload_len)
{
	if (handler_id == 0 || handler_id >= g_listeners_cap)
		return 0;
	bud_dom_listener_t *l = &g_listeners[handler_id];
	if (!l->active || !l->fn)
		return 0;

	char stack_buf[512];
	char *heap_buf = NULL;
	const char *pstr = "";
	if (payload && payload_len > 0) {
		if (payload_len < sizeof(stack_buf)) {
			memcpy(stack_buf, payload, payload_len);
			stack_buf[payload_len] = '\0';
			pstr = stack_buf;
		} else {
			heap_buf = (char *)malloc(payload_len + 1);
			if (heap_buf) {
				memcpy(heap_buf, payload, payload_len);
				heap_buf[payload_len] = '\0';
				pstr = heap_buf;
			}
		}
	}

	const char *prev_payload = bud_dom_set_event_payload(pstr);

	bud_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = l->event ? l->event : "";
	ev.bubbles = l->bubbles;
	ev.user = l->ud ? l->ud : (void *)pstr;

	int rc = l->fn(&ev);

	bud_dom_set_event_payload(prev_payload);
	free(heap_buf);
	return rc ? rc : ev.stopped;
}

static void *dom_op_create_el(void *ctx, const char *tag)
{
	(void)ctx;
	if (!tag) return NULL;
#ifdef __wasm__
	uint32_t h = bud_dom_create_el(tag, strlen(tag));
	return (void *)(uintptr_t)h;
#else
	static uint32_t next_id = 2;
	return (void *)(uintptr_t)(next_id++);
#endif
}

static void *dom_op_create_text(void *ctx, const char *text, size_t len)
{
	(void)ctx;
#ifdef __wasm__
	uint32_t h = bud_dom_create_text(text ? text : "", text ? len : 0);
	return (void *)(uintptr_t)h;
#else
	(void)text; (void)len;
	static uint32_t next_id = 1000000u;
	return (void *)(uintptr_t)(next_id++);
#endif
}

static void dom_op_set_text(void *ctx, void *hn, const char *text, size_t len)
{
	(void)ctx;
#ifdef __wasm__
	bud_dom_set_text((uint32_t)(uintptr_t)hn, text ? text : "", text ? len : 0);
#else
	(void)hn; (void)text; (void)len;
#endif
}

static void dom_op_set_attr(void *ctx, void *hn, const char *name, const char *val)
{
	(void)ctx;
	if (!name) return;
#ifdef __wasm__
	bud_dom_set_attr((uint32_t)(uintptr_t)hn, name, strlen(name),
	                 val ? val : "", val ? strlen(val) : 0);
#else
	(void)hn; (void)val;
#endif
}

static void dom_op_remove_attr(void *ctx, void *hn, const char *name)
{
	(void)ctx;
	if (!name) return;
#ifdef __wasm__
	bud_dom_remove_attr((uint32_t)(uintptr_t)hn, name, strlen(name));
#else
	(void)hn;
#endif
}

static void dom_op_insert_before(void *ctx, void *hp, void *hc, void *hb)
{
	(void)ctx;
#ifdef __wasm__
	bud_dom_insert_before((uint32_t)(uintptr_t)hp,
	                      (uint32_t)(uintptr_t)hc,
	                      (uint32_t)(uintptr_t)hb);
#else
	(void)hp; (void)hc; (void)hb;
#endif
}

static void dom_op_remove_child(void *ctx, void *hp, void *hc)
{
	(void)ctx;
#ifdef __wasm__
	bud_dom_remove_child((uint32_t)(uintptr_t)hp, (uint32_t)(uintptr_t)hc);
#else
	(void)hp; (void)hc;
#endif
}

static void dom_op_destroy_node(void *ctx, void *hn)
{
	(void)ctx;
	uint32_t id = (uint32_t)(uintptr_t)hn;
	listener_remove_node(id);
#ifdef __wasm__
	bud_dom_destroy_node(id);
#endif
}

static void dom_op_set_event(void *ctx, void *hn, const char *event, int bubbles,
                             bud_event_handler_fn fn, void *ud)
{
	(void)ctx;
	if (!event) return;
	uint32_t node_id = (uint32_t)(uintptr_t)hn;
	uint32_t hid = listener_register(node_id, event, bubbles, fn, ud);
#ifdef __wasm__
	bud_dom_set_event(node_id, event, strlen(event), bubbles, hid);
#else
	(void)hid;
#endif
}

static void dom_op_clear_children(void *ctx, void *hp)
{
	(void)ctx;
#ifdef __wasm__
	bud_dom_clear_children((uint32_t)(uintptr_t)hp);
#else
	(void)hp;
#endif
}

bud_host_ops bud_dom_host_ops(void)
{
	bud_host_ops ops;
	ops.create_el      = dom_op_create_el;
	ops.create_text    = dom_op_create_text;
	ops.set_text       = dom_op_set_text;
	ops.set_attr       = dom_op_set_attr;
	ops.remove_attr    = dom_op_remove_attr;
	ops.insert_before  = dom_op_insert_before;
	ops.remove_child   = dom_op_remove_child;
	ops.destroy_node   = dom_op_destroy_node;
	ops.set_event      = dom_op_set_event;
	ops.clear_children = dom_op_clear_children;
	return ops;
}

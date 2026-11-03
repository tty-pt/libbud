#include "../include/bud/bud_app.h"
#include "../include/bud/bud.h"
#include <stddef.h>

#ifdef __wasm__

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

ISO_IMPORT(void, bud, bud_host_log, const char *msg, size_t len);
#define WASM_LOG(msg)                                                          \
	do {                                                                   \
		const char *_m = (msg);                                        \
		bud_host_log(_m, strlen(_m));                                  \
	} while (0)
static void wasm_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bud_host_log(buf, strlen(buf));
}

ISO_IMPORT(void, bud, bud_host_mark_dirty, void);
ISO_IMPORT(void, bud, bud_host_flush, void);
ISO_IMPORT(void, bud, bud_host_fetch, const char *url, size_t url_len, int request_id);
ISO_IMPORT(void, bud, bud_host_set_location, const char *url, size_t url_len);

/* Platform host function pointers (declared in bud.h) */
extern void (*bud_host_fetch_fn)(const char *, size_t, int);
extern void (*bud_host_log_fn)(const char *, size_t);
extern void (*bud_host_set_location_fn)(const char *, size_t);

__attribute__((constructor)) static void wasm_platform_init(void)
{
	bud_host_fetch_fn = bud_host_fetch;
	bud_host_log_fn = bud_host_log;
	bud_host_set_location_fn = bud_host_set_location;
}

static bud_runtime *runtime = NULL;
#define BUD_MAX_SLOTS 4
static bud_runtime *g_slot_runtimes[BUD_MAX_SLOTS];
static bud_node *(*g_slot_render_fns[BUD_MAX_SLOTS])(void);
static char wasm_dbg_buf[65536];

extern bud_node *bud_app_render(void) __attribute__((weak));

static int noop_emit(
        void *user, const char *op, const char *a, const char *b, const char *c)
{
	(void)user;
	(void)op;
	(void)a;
	(void)b;
	(void)c;
	return 0;
}

static int emit_patch_wrapper(
        void *user, const char *op, const char *a, const char *b, const char *c)
{
	(void)user;
	bud_host_emit_patch(
	        op, op ? strlen(op) : 0, a, a ? strlen(a) : 0, b,
	        b ? strlen(b) : 0, c, c ? strlen(c) : 0);
	return 0;
}

int bud_slot_mount(int slot, bud_node *(*render_fn)(void))
{
	bud_host_fetch_fn = bud_host_fetch;
	bud_host_log_fn = bud_host_log;
	bud_host_set_location_fn = bud_host_set_location;

	if (slot < 0 || slot >= BUD_MAX_SLOTS || !render_fn)
		return -1;
	g_slot_render_fns[slot] = render_fn;
	if (g_slot_runtimes[slot]) {
		bud_runtime_free(g_slot_runtimes[slot]);
		g_slot_runtimes[slot] = NULL;
	}
	bud_node *root = render_fn();
	if (root) {
		bud_render_ops(root, noop_emit, NULL);
		g_slot_runtimes[slot] = bud_runtime_new(root);
		bud_runtime_mount(g_slot_runtimes[slot]);
		if (slot == 1 || !runtime) runtime = g_slot_runtimes[slot];
	}
	return 0;
}

int bud_slot_update(int slot, bud_node *(*render_fn)(void))
{
	if (slot < 0 || slot >= BUD_MAX_SLOTS)
		return -1;
	if (!render_fn)
		render_fn = g_slot_render_fns[slot];
	if (!render_fn)
		return -1;
#if defined(__wasm__)
	if (g_slot_runtimes[slot]) {
		bud_node *old_root = bud_runtime_root(g_slot_runtimes[slot]);
		bud_node *new_root = render_fn();
		if (new_root) {
			bud_vdom_diff(old_root, new_root, emit_patch_wrapper, (void *)(intptr_t)slot);
			bud_runtime_free(g_slot_runtimes[slot]);
			g_slot_runtimes[slot] = bud_runtime_new(new_root);
			bud_runtime_mount(g_slot_runtimes[slot]);
			if (slot == 1 || !runtime) runtime = g_slot_runtimes[slot];
		}
	}
#endif
	return 0;
}

void bud_slot_cleanup(int slot)
{
	if (slot >= 0 && slot < BUD_MAX_SLOTS && g_slot_runtimes[slot]) {
		if (runtime == g_slot_runtimes[slot]) runtime = NULL;
		g_slot_render_fns[slot] = NULL;
		bud_runtime_free(g_slot_runtimes[slot]);
		g_slot_runtimes[slot] = NULL;
	}
}

ISO_EXPORT_AS(void, wasm_mount, bud_app_mount)
{
	bud_host_emit_patch("MOUNT_START", 11, NULL, 0, NULL, 0, NULL, 0);
	if (!runtime && bud_app_render) {
		bud_node *app = bud_app_render();
		if (app) {
			bud_render_ops(app, noop_emit, NULL);
			runtime = bud_runtime_new(app);
			bud_runtime_mount(runtime);
		}
		bud_host_emit_patch(
		        "MOUNT_DONE", 10, NULL, 0, NULL, 0, NULL, 0);
	}
}

void bud_app_set_state(void)
{
	if (g_slot_runtimes[1] && g_slot_render_fns[1]) {
		bud_slot_update(1, g_slot_render_fns[1]);
		return;
	}
	if (g_slot_runtimes[0] && g_slot_render_fns[0]) {
		bud_slot_update(0, g_slot_render_fns[0]);
		return;
	}
	if (!runtime || !bud_app_render)
		return;
	bud_node *old_root = bud_runtime_root(runtime);
	bud_node *new_root = bud_app_render();
	if (!new_root)
		return;

	bud_vdom_diff(old_root, new_root, emit_patch_wrapper, NULL);

	bud_runtime_free(runtime);
	runtime = bud_runtime_new(new_root);
	bud_runtime_mount(runtime);
}

ISO_EXPORT_AS(void, wasm_update, bud_app_update)
{
	bud_app_set_state();
}

ISO_EXPORT_AS(void, wasm_unmount, bud_app_unmount)
{
	if (runtime) {
		bud_runtime_free(runtime);
		runtime = NULL;
	}
}

ISO_EXPORT_AS(void, wasm_mark_dirty, bud_app_mark_dirty)
{
	if (runtime) {
		bud_runtime_mark_dirty(runtime);
	}
}

ISO_EXPORT_AS(void, wasm_flush, bud_app_flush)
{
	bud_host_emit_patch("DEBUG_FLUSH", 11, NULL, 0, NULL, 0, NULL, 0);
	if (runtime) {
		bud_render_patch_ops(
		        bud_runtime_root(runtime), emit_patch_wrapper, NULL);
		bud_runtime_flush(runtime);
	}
}

__attribute__((weak)) ISO_EXPORT(void, wasm_tick, float dt, int width, int height)
{
	(void)dt;
	(void)width;
	(void)height;
	extern void wasm_canvas_render(void) __attribute__((weak));
	if (wasm_canvas_render) {
		wasm_canvas_render();
	}
}

static bud_node *find_node_by_id(const bud_node *node, unsigned int id)
{
	if (!node)
		return NULL;
	if (bud_node_id(node) == id)
		return (bud_node *)node;
	for (size_t i = 0; i < bud_node_child_count(node); i++) {
		bud_node *found = find_node_by_id(bud_node_child(node, i), id);
		if (found)
			return found;
	}
	return NULL;
}

bud_runtime *wasm_get_runtime(void)
{
	return runtime;
}

void bud_patch_attr(bud_node *node, const char *name, const char *value)
{
	if (!node || !name || !value)
		return;
	char id_str[16];
	snprintf(id_str, sizeof(id_str), "%u", bud_node_id(node));
	bud_host_emit_patch(
	        "patch-attr", 10, id_str, strlen(id_str), name, strlen(name),
	        value, strlen(value));
}

void bud_patch_text(bud_node *node, const char *value)
{
	if (!node || !value)
		return;
	char id_str[16];
	snprintf(id_str, sizeof(id_str), "%u", bud_node_id(node));
	bud_host_emit_patch(
	        "patch-text", strlen("patch-text"), value, strlen(value),
	        id_str, strlen(id_str), NULL, 0);
}

ISO_EXPORT(void, bud_patch_innerhtml, unsigned int node_id, const char *html)
{
	if (!html)
		return;
	char id_str[16];
	snprintf(id_str, sizeof(id_str), "%u", node_id);
	bud_host_emit_patch(
	        "patch-innerhtml", 15, id_str, strlen(id_str), html,
	        strlen(html), NULL, 0);
}

int bud_slot_dispatch_event(int slot, unsigned int node_id, const char *event_name, int bubbles, void *event_data)
{
	(void)bubbles;
	if (slot < 0 || slot >= BUD_MAX_SLOTS || !g_slot_runtimes[slot])
		return -1;
	bud_node *root = bud_runtime_root(g_slot_runtimes[slot]);
	bud_node *target = find_node_by_id(root, node_id);
	if (!target)
		return -2;
	return bud_runtime_dispatch(g_slot_runtimes[slot], target, event_name, event_data);
}

int bud_slot_dispatch_any(unsigned int node_id, const char *event_name, int bubbles, void *event_data)
{
	(void)bubbles;
	int order[] = { 1, 0, 2, 3 };
	for (size_t i = 0; i < sizeof(order)/sizeof(order[0]); i++) {
		int s = order[i];
		if (g_slot_runtimes[s]) {
			bud_node *root = bud_runtime_root(g_slot_runtimes[s]);
			bud_node *target = find_node_by_id(root, node_id);
			if (target) {
				return bud_runtime_dispatch(g_slot_runtimes[s], target, event_name, event_data);
			}
		}
	}
	if (runtime) {
		bud_node *root = bud_runtime_root(runtime);
		bud_node *target = find_node_by_id(root, node_id);
		if (target) {
			return bud_runtime_dispatch(runtime, target, event_name, event_data);
		}
	}
	return -1;
}

ISO_EXPORT_AS(int, wasm_dispatch_event, bud_app_dispatch,
        unsigned int node_id, const char *event_name, int bubbles,
        void *event_data)
{
	return bud_slot_dispatch_any(node_id, event_name, bubbles, event_data);
}

/* ── Debug exports ── */

ISO_EXPORT(void, wasm_dump_tree, void)
{
	bud_node *root;

	if (!runtime) {
		WASM_LOG("(no runtime)");
		return;
	}
	root = bud_runtime_root(runtime);
	if (!root) {
		WASM_LOG("(no root)");
		return;
	}
	bud_sprint_tree(root, wasm_dbg_buf, sizeof(wasm_dbg_buf));
	WASM_LOG("--- WASM tree ---");
	WASM_LOG(wasm_dbg_buf);
	WASM_LOG("--- end tree ---");
}

ISO_EXPORT(const char *, wasm_get_tree_text, void)
{
	bud_node *root = NULL;

	if (g_slot_runtimes[1]) {
		root = bud_runtime_root(g_slot_runtimes[1]);
	} else if (g_slot_runtimes[0]) {
		root = bud_runtime_root(g_slot_runtimes[0]);
	} else if (runtime) {
		root = bud_runtime_root(runtime);
	}

	if (!root) {
		wasm_dbg_buf[0] = '\0';
		return wasm_dbg_buf;
	}
	bud_sprint_tree(root, wasm_dbg_buf, sizeof(wasm_dbg_buf));
	return wasm_dbg_buf;
}

ISO_EXPORT(const char *, wasm_get_src, unsigned int node_id)
{
	bud_node *root;
	bud_node *target;

	if (!runtime) {
		wasm_dbg_buf[0] = '\0';
		return wasm_dbg_buf;
	}
	root = bud_runtime_root(runtime);
	if (!root) {
		wasm_dbg_buf[0] = '\0';
		return wasm_dbg_buf;
	}
	target = find_node_by_id(root, node_id);
	if (!target) {
		snprintf(
		        wasm_dbg_buf, sizeof(wasm_dbg_buf),
		        "node=%d NOT IN WASM TREE", node_id);
		return wasm_dbg_buf;
	}
	snprintf(
	        wasm_dbg_buf, sizeof(wasm_dbg_buf), "%s",
	        bud_node_get_src(target));
	return wasm_dbg_buf;
}

#else

/* Native (non-WASM) stubs — these are needed so that libbud.so exports
 * the wasm_* symbols for use by native modules like mods/song/song.so.
 * In the native server context there is no browser DOM, so each stub
 * is a no-op or returns a sensible default. */

void wasm_mount(void)
{
	(void)0;
}

void wasm_update(void)
{
	(void)0;
}

void wasm_unmount(void)
{
	(void)0;
}

void wasm_mark_dirty(void)
{
	(void)0;
}

void wasm_flush(void)
{
	(void)0;
}

bud_runtime *wasm_get_runtime(void)
{
	return NULL;
}

void bud_patch_attr(bud_node *node, const char *name, const char *value)
{
	(void)node;
	(void)name;
	(void)value;
}

void bud_patch_text(bud_node *node, const char *value)
{
	(void)node;
	(void)value;
}

int wasm_dispatch_event(
        unsigned int node_id, const char *event_name, int bubbles,
        void *event_data)
{
	(void)node_id;
	(void)event_name;
	(void)bubbles;
	(void)event_data;
	return -1;
}

void bud_patch_innerhtml(unsigned int node_id, const char *html)
{
	(void)node_id;
	(void)html;
}

void bud_host_set_location(const char *url, size_t len)
{
	(void)url;
	(void)len;
}

void wasm_dump_tree(void)
{
	(void)0;
}

const char *wasm_get_tree_text(void)
{
	return "";
}

const char *wasm_get_src(unsigned int node_id)
{
	(void)node_id;
	return "";
}

void bud_app_set_state(void)
{}

void wasm_tick(float dt, int width, int height)
{
	(void)dt;
	(void)width;
	(void)height;
}

int bud_slot_mount(int slot, bud_node *(*render_fn)(void))
{
	(void)slot;
	(void)render_fn;
	return 0;
}

int bud_slot_update(int slot, bud_node *(*render_fn)(void))
{
	(void)slot;
	(void)render_fn;
	return 0;
}

int bud_slot_dispatch_event(int slot, unsigned int node_id, const char *event_name, int bubbles, void *event_data)
{
	(void)slot;
	(void)node_id;
	(void)event_name;
	(void)bubbles;
	(void)event_data;
	return -1;
}

int bud_slot_dispatch_any(unsigned int node_id, const char *event_name, int bubbles, void *event_data)
{
	(void)node_id;
	(void)event_name;
	(void)bubbles;
	(void)event_data;
	return -1;
}

void bud_slot_cleanup(int slot)
{
	(void)slot;
}

#endif

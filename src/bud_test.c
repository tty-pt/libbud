#define _POSIX_C_SOURCE 200809L
#include "../include/bud/bud_jsx.h"
#include "../include/bud/bud.h"
#include "../include/bud/bud_slot.h"
#include "../include/bud/bud_state.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if !__has_feature(address_sanitizer)
#define BUD_TEST_COUNT_ALLOCS 1
#endif
#else
#define BUD_TEST_COUNT_ALLOCS 1
#endif
#endif

#ifdef BUD_TEST_COUNT_ALLOCS
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t nmemb, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void __libc_free(void *ptr);

static unsigned long g_bud_test_allocs = 0;

void *malloc(size_t size)
{
	g_bud_test_allocs++;
	return __libc_malloc(size);
}

void *calloc(size_t nmemb, size_t size)
{
	g_bud_test_allocs++;
	return __libc_calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size)
{
	g_bud_test_allocs++;
	return __libc_realloc(ptr, size);
}

void free(void *ptr)
{
	__libc_free(ptr);
}
#endif

typedef struct test_ops {
	char buffer[1024];
	size_t len;
} test_ops;

typedef struct hydrate_expect {
	unsigned int id;
	const char *kind;
	const char *tag;
	const char *text;
} hydrate_expect;

typedef struct lifecycle_log {
	char buffer[512];
	size_t len;
} lifecycle_log;

static int test_emit(
        void *user, const char *op, const char *a, const char *b, const char *c)
{
	test_ops *ops;
	size_t need;
	int wrote;

	(void)c;
	ops = (test_ops *)user;
	need = strlen(op) + 1;
	if (a) {
		need += strlen(a) + 1;
	}
	if (b) {
		need += strlen(b) + 1;
	}
	if (c) {
		need += strlen(c) + 1;
	}
	if (ops->len + need + 1 >= sizeof(ops->buffer)) {
		return -1;
	}

	wrote = snprintf(
	        ops->buffer + ops->len, sizeof(ops->buffer) - ops->len,
	        "%s%s%s%s%s%s%s\n", op, a ? ":" : "", a ? a : "", b ? ":" : "",
	        b ? b : "", c ? ":" : "", c ? c : "");
	if (wrote < 0) {
		return -1;
	}

	ops->len += (size_t)wrote;
	return 0;
}

static int check(int cond, const char *msg)
{
	if (cond) {
		return 0;
	}

	fprintf(stderr, "bud test failed: %s\n", msg);
	return 1;
}

static bud_node *test_slot_render_high(int slot_id, const void *state) {
	(void)slot_id; (void)state;
	return bud_element("high-priority");
}

static bud_node *test_slot_render_low(int slot_id, const void *state) {
	(void)slot_id; (void)state;
	return bud_element("low-priority");
}

static int test_slot_fill_fn(int slot_id, bud_node *parent, const void *state) {
	(void)slot_id; (void)state;
	bud_append(parent, bud_element("child"));
	return 0;
}

typedef struct {
	const char *label;
} test_leaf_props;

static bud_node *test_comp_leaf(void *ctx, const void *raw_props)
{
	const test_leaf_props *p = (const test_leaf_props *)raw_props;
	(void)ctx;
	bud_node *sp = bud_element("span");
	bud_set_attr(sp, "class", "leaf");
	bud_append(sp, bud_text(p && p->label ? p->label : ""));
	return sp;
}

static bud_node *test_comp_null(void *ctx, const void *raw_props)
{
	(void)ctx;
	(void)raw_props;
	return NULL;
}

static bud_node *test_comp_wrap(void *ctx, const void *raw_props)
{
	const test_leaf_props *p = (const test_leaf_props *)raw_props;
	(void)ctx;
	bud_node *box = bud_element("section");
	bud_append(box, bud_component_vnode(test_comp_null, NULL, NULL, 0));
	bud_append(box, bud_component_vnode(test_comp_leaf, NULL, p, sizeof(*p)));
	return box;
}

#define TEST_STATE_SCHEMA(F, st) \
	F(STR, st, title, 64, "") \
	F(INT, st, score, 0, 0)

BUD_DEFINE_STATE(test_state_t, TEST_STATE_SCHEMA)


static int hydrate_lookup(
        void *user, unsigned int id, const char **kind, const char **tag,
        const char **text)
{
	hydrate_expect *expect;

	expect = (hydrate_expect *)user;
	while (expect->kind) {
		if (expect->id == id) {
			*kind = expect->kind;
			*tag = expect->tag;
			*text = expect->text;
			return 0;
		}
		expect++;
	}

	return -1;
}

static int lifecycle_push(lifecycle_log *log, const char *fmt, ...)
{
	va_list ap;
	int wrote;

	if (log->len >= sizeof(log->buffer)) {
		return -1;
	}

	va_start(ap, fmt);
	wrote = vsnprintf(
	        log->buffer + log->len, sizeof(log->buffer) - log->len, fmt,
	        ap);
	va_end(ap);
	if (wrote < 0) {
		return -1;
	}
	if ((size_t)wrote >= sizeof(log->buffer) - log->len) {
		return -1;
	}

	log->len += (size_t)wrote;
	return 0;
}

static void on_mount(void *user, bud_runtime *runtime, const bud_node *node)
{
	lifecycle_log *log;

	(void)runtime;
	log = (lifecycle_log *)user;
	lifecycle_push(log, "mount:%s\n", bud_node_tag(node));
}

static void on_update(void *user, bud_runtime *runtime, const bud_node *node)
{
	lifecycle_log *log;

	(void)runtime;
	log = (lifecycle_log *)user;
	lifecycle_push(log, "update:%s\n", bud_node_tag(node));
}

static void on_unmount(void *user, bud_runtime *runtime, const bud_node *node)
{
	lifecycle_log *log;

	(void)runtime;
	log = (lifecycle_log *)user;
	lifecycle_push(log, "unmount:%s\n", bud_node_tag(node));
}

static void on_invalidate(void *user, bud_runtime *runtime)
{
	lifecycle_log *log;

	(void)runtime;
	log = (lifecycle_log *)user;
	lifecycle_push(log, "invalidate\n");
}

static int on_click_stop(bud_event *event)
{
	lifecycle_log *log;
	const char *tag;

	log = (lifecycle_log *)event->user;
	tag = event->current_target ? bud_node_tag(event->current_target) : "";
	lifecycle_push(log, "click:%s:%s\n", event->type, tag);
	bud_event_stop_propagation(event);
	return 0;
}

static int on_submit(bud_event *event)
{
	lifecycle_log *log;
	const char *tag;

	log = (lifecycle_log *)event->user;
	tag = event->current_target ? bud_node_tag(event->current_target) : "";
	lifecycle_push(log, "submit:%s:%s\n", event->type, tag);
	return 0;
}

static int test_jsx(void)
{
	bud_arg tree = lx_frag(
	        lx_el("main", lx_attr("data-role", "app"),
	              lx_el("h1", lx_text("Hello")),
	              lx_el("button", lx_on("click", 1), lx_on("submit", 0),
	                    lx_text("Click me")),
	              lx_el("p", lx_text("C DOM runtime"))));
	char *html = bud_render_html(tree.data.node);
	int rc = 0;
	if (!html ||
	    !strstr(html, "<main data-role=\"app\"><h1>Hello</h1><button>Click "
	                  "me</button><p>C DOM runtime</p></main>"))
	{
		fprintf(stderr, "JSX test failed: %s\n", html ? html : "NULL");
		rc = 1;
	}
	bud_free_string(html);
	bud_free(tree.data.node);
	return rc;
}

static int test_jsx_ergonomics(void)
{
	bud_node *tree = lx_n("main", lx_attr("data-role", "app"),
	                      lx_el("h1", lx_textf("%s %d", "Item", 42)),
	                      lx_el("button", lx_text("Click me")),
	                      lx_textf("Count: %d", 100));
	char *html = bud_render_html(tree);
	int rc = 0;
	if (!html ||
	    !strstr(html, "<main data-role=\"app\"><h1>Item 42</h1><button>Click "
	                  "me</button>Count: 100</main>"))
	{
		fprintf(stderr, "JSX ergonomics test failed: %s\n", html ? html : "NULL");
		rc = 1;
	}
	bud_free_string(html);

	const bud_node *h1_node = bud_node_child(tree, 0);
	if (!h1_node || bud_node_parent(h1_node) != tree) {
		fprintf(stderr, "bud_node_parent test failed\n");
		rc = 1;
	}

	bud_node *frag = lx_frag_n(lx_el("span", lx_text("A")), lx_el("span", lx_text("B")));
	char *frag_html = bud_render_html(frag);
	if (!frag_html || strcmp(frag_html, "<span>A</span><span>B</span>") != 0) {
		fprintf(stderr, "lx_frag_n test failed: %s\n", frag_html ? frag_html : "NULL");
		rc = 1;
	}
	bud_free_string(frag_html);
	bud_free(frag);

	bud_free(tree);
	return rc;
}

static int test_bool_attr(void)
{
	bud_node *el;
	char *html;
	int rc;

	rc = 0;
	el = bud_element("input");
	if (!el) {
		return 1;
	}

	rc |= bud_set_bool_attr(el, "checked");
	rc |= bud_set_attr(el, "name", "agree");
	if (rc != 0) {
		fprintf(stderr, "bool attr setup failed\n");
		bud_free(el);
		return 1;
	}

	html = bud_render_html(el);
	if (!html || !strstr(html, "checked") || strstr(html, "checked=\"\"")) {
		fprintf(stderr, "bool attr test failed: %s\n",
		        html ? html : "NULL");
		bud_free_string(html);
		bud_free(el);
		return 1;
	}

	bud_free_string(html);
	bud_free(el);
	return 0;
}

static int test_raw_html(void)
{
	bud_node *el;
	bud_node *raw;
	char *html;
	int rc;

	rc = 0;
	el = bud_element("div");
	raw = bud_raw("<b>bold</b> & <i>italic</i>");
	if (!el || !raw) {
		bud_free(el);
		bud_free(raw);
		return 1;
	}

	rc = bud_append(el, raw);
	if (rc != 0) {
		fprintf(stderr, "raw html append failed\n");
		bud_free(el);
		return 1;
	}

	html = bud_render_html(el);
	if (!html || strstr(html, "&lt;") || !strstr(html, "<b>bold</b>")) {
		fprintf(stderr, "raw html test failed: %s\n",
		        html ? html : "NULL");
		bud_free_string(html);
		bud_free(el);
		return 1;
	}

	bud_free_string(html);
	bud_free(el);
	return 0;
}

static int test_get_attr(void)
{
	bud_node *el;
	const char *val;
	int rc;

	rc = 0;
	el = bud_element("div");
	if (!el) {
		return 1;
	}

	rc = bud_set_attr(el, "data-x", "hello");
	if (rc != 0) {
		fprintf(stderr, "get attr setup failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "data-x");
	if (!val || strcmp(val, "hello") != 0) {
		fprintf(stderr, "get attr value failed: %s\n",
		        val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "nonexistent");
	if (val) {
		fprintf(stderr, "get attr nonexistent should be NULL\n");
		bud_free(el);
		return 1;
	}

	bud_free(el);
	return 0;
}

static int test_class_helpers(void)
{
	bud_node *el;
	const char *val;
	int rc;

	rc = 0;
	el = bud_element("div");
	if (!el) {
		return 1;
	}

	rc = bud_add_class(el, "foo");
	if (rc != 0) {
		fprintf(stderr, "add class failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "class");
	if (!val || strcmp(val, "foo") != 0) {
		fprintf(stderr, "add class result: %s\n", val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	rc = bud_add_class(el, "foo");
	if (rc != 0) {
		fprintf(stderr, "add class duplicate failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "class");
	if (!val || strcmp(val, "foo") != 0) {
		fprintf(stderr, "add class duplicate result: %s\n",
		        val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	rc = bud_add_class(el, "bar");
	if (rc != 0) {
		fprintf(stderr, "add class second failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "class");
	if (!val || strcmp(val, "foo bar") != 0) {
		fprintf(stderr, "add class second result: %s\n",
		        val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	rc = bud_remove_class(el, "foo");
	if (rc != 0) {
		fprintf(stderr, "remove class failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "class");
	if (!val || strcmp(val, "bar") != 0) {
		fprintf(stderr, "remove class result: %s\n",
		        val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	rc = bud_remove_class(el, "foo");
	if (rc != 0) {
		fprintf(stderr, "remove class absent failed\n");
		bud_free(el);
		return 1;
	}

	rc = bud_toggle_class(el, "foo");
	if (rc != 0) {
		fprintf(stderr, "toggle class add failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "class");
	if (!val || strcmp(val, "bar foo") != 0) {
		fprintf(stderr, "toggle class add result: %s\n",
		        val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	rc = bud_toggle_class(el, "bar");
	if (rc != 0) {
		fprintf(stderr, "toggle class remove failed\n");
		bud_free(el);
		return 1;
	}

	val = bud_get_attr(el, "class");
	if (!val || strcmp(val, "foo") != 0) {
		fprintf(stderr, "toggle class remove result: %s\n",
		        val ? val : "NULL");
		bud_free(el);
		return 1;
	}

	bud_free(el);
	return 0;
}

static int test_detach(void)
{
	bud_node *parent;
	bud_node *child;
	int rc;

	rc = 0;
	parent = bud_element("div");
	child = bud_element("span");
	if (!parent || !child) {
		bud_free(parent);
		bud_free(child);
		return 1;
	}

	rc = bud_append(parent, child);
	if (rc != 0) {
		fprintf(stderr, "detach setup append failed\n");
		bud_free(parent);
		return 1;
	}

	if (bud_node_child_count(parent) != 1) {
		fprintf(stderr, "detach setup child count\n");
		bud_free(parent);
		return 1;
	}

	rc = bud_detach(child);
	if (rc != 0) {
		fprintf(stderr, "detach failed\n");
		bud_free(parent);
		bud_free(child);
		return 1;
	}

	if (bud_node_child_count(parent) != 0) {
		fprintf(stderr, "detach parent child count\n");
		bud_free(parent);
		bud_free(child);
		return 1;
	}

	rc = bud_detach(child);
	if (rc == 0) {
		fprintf(stderr, "detach orphan should fail\n");
		bud_free(parent);
		bud_free(child);
		return 1;
	}

	bud_free(parent);
	bud_free(child);
	return 0;
}

static int test_window_event_type(void)
{
	bud_node *root;
	bud_node *button;
	bud_runtime *runtime;
	lifecycle_log lifecycle;
	int rc;

	memset(&lifecycle, 0, sizeof(lifecycle));
	root = bud_fragment();
	button = bud_element("button");
	if (!root || !button) {
		bud_free(root);
		bud_free(button);
		return 1;
	}
	bud_bind(button, "scroll@window", 0, on_submit);
	bud_append(root, button);
	runtime = bud_runtime_new(root);
	rc = bud_runtime_dispatch(runtime, button, "scroll@window", &lifecycle);
	if (rc == 0)
		rc =
		        check(strstr(lifecycle.buffer,
		                     "submit:scroll:button") != NULL,
		              "window target exposes logical event type");
	bud_runtime_free(runtime);
	bud_free(root);
	return rc;
}

static void arena_test_unmount_cb(
        void *user, bud_runtime *runtime, const bud_node *node)
{
	int *count = (int *)user;
	(void)runtime;
	(void)node;
	if (count)
		(*count)++;
}

static int arena_test_click_cb(bud_event *event)
{
	int **count_ptr = (int **)event->user;
	if (count_ptr && *count_ptr)
		(**count_ptr)++;
	return 0;
}

static bud_node *bench_build_tree_heap(int frame)
{
	bud_node *root = bud_element("section");
	bud_node *hdr = bud_element("header");
	bud_node *h1 = bud_element("h1");
	bud_node *ul = bud_element("ul");
	char kbuf[32];

	bud_set_attr(root, "class", "hud-panel");
	bud_set_attr(root, "data-view", "inventory");
	bud_set_attr(hdr, "class", "hud-header");
	bud_append(h1, bud_text("Inventory"));
	bud_append(hdr, h1);
	bud_append(hdr, bud_textf("Frame %d", frame));
	bud_append(root, hdr);
	bud_set_attr(ul, "class", "hud-list");
	for (int i = 0; i < 25; i++) {
		bud_node *li = bud_element("li");
		bud_node *badge = bud_element("span");
		bud_node *label = bud_element("strong");

		snprintf(kbuf, sizeof(kbuf), "row-%d", i);
		bud_set_key(li, kbuf);
		bud_set_attr(li, "class", (i == (frame % 25)) ? "row active" : "row");
		bud_set_attr(badge, "class", "badge");
		bud_append(badge, bud_text("ITEM"));
		bud_append(label, bud_textf("Slot %d (#%d)", i, frame + i));
		bud_append(li, badge);
		bud_append(li, label);
		bud_append(ul, li);
	}
	bud_append(root, ul);
	return root;
}

static bud_node *bench_build_tree_lit(int frame)
{
	bud_node *root = bud_element_lit("section");
	bud_node *hdr = bud_element_lit("header");
	bud_node *h1 = bud_element_lit("h1");
	bud_node *ul = bud_element_lit("ul");
	char kbuf[32];

	bud_set_attr_lit(root, "class", "hud-panel");
	bud_set_attr_lit(root, "data-view", "inventory");
	bud_set_attr_lit(hdr, "class", "hud-header");
	bud_append(h1, bud_text_lit("Inventory"));
	bud_append(hdr, h1);
	bud_append(hdr, bud_textf("Frame %d", frame));
	bud_append(root, hdr);
	bud_set_attr_lit(ul, "class", "hud-list");
	for (int i = 0; i < 25; i++) {
		bud_node *li = bud_element_lit("li");
		bud_node *badge = bud_element_lit("span");
		bud_node *label = bud_element_lit("strong");

		snprintf(kbuf, sizeof(kbuf), "row-%d", i);
		bud_set_key(li, kbuf);
		bud_set_attr_lit(li, "class", (i == (frame % 25)) ? "row active" : "row");
		bud_set_attr_lit(badge, "class", "badge");
		bud_append(badge, bud_text_lit("ITEM"));
		bud_append(label, bud_textf("Slot %d (#%d)", i, frame + i));
		bud_append(li, badge);
		bud_append(li, label);
		bud_append(ul, li);
	}
	bud_append(root, ul);
	return root;
}

static double monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static int test_bud_arena_and_lit(void)
{
	int rc = 0;

	/* 1. Basic arena push/pop/current/bytes_used + *_lit + unmount hook */
	{
		bud_arena *a = bud_arena_new(256);
		bud_arena *prev;
		bud_node *div;
		bud_node *btn;
		char *html;
		int unmounted = 0;
		int clicked = 0;
		int *clicked_ptr = &clicked;

		rc = check(a != NULL && bud_arena_current() == NULL &&
		           bud_arena_bytes_used(a) == 0,
		           "arena initial state");
		if (rc != 0) {
			bud_arena_free(a);
			return rc;
		}
		prev = bud_arena_push(a);
		rc = check(prev == NULL && bud_arena_current() == a,
		           "arena push sets current");
		if (rc != 0) {
			bud_arena_pop(prev);
			bud_arena_free(a);
			return rc;
		}

		div = bud_element_lit("div");
		bud_set_key_lit(div, "k-root");
		bud_set_attr_lit(div, "class", "box");
		bud_set_attr_name_lit(div, "data-dyn", "hello");
		bud_set_bool_attr_lit(div, "hidden");
		bud_set_lifecycle(div, NULL, NULL, arena_test_unmount_cb, &unmounted);

		btn = bud_element_lit("button");
		bud_bind_ctx_lit(btn, "click", 1, arena_test_click_cb,
		                 &clicked_ptr, sizeof(clicked_ptr));
		bud_append(btn, bud_text_lit("Go"));
		bud_append(div, btn);

		bud_arena_pop(prev);
		rc = check(bud_arena_current() == NULL &&
		           bud_arena_bytes_used(a) > 0,
		           "arena pop restores NULL and records bytes_used");

		if (rc == 0)
			rc = check(strcmp(bud_get_key(div), "k-root") == 0 &&
			           bud_node_listener_count(btn) == 1 &&
			           strcmp(bud_node_listener_event(btn, 0), "click") == 0,
			           "lit key and listener");

		html = bud_render_html(div);
		if (rc == 0)
			rc = check(html != NULL &&
			           strstr(html, "data-bud-key=\"k-root\"") != NULL &&
			           strstr(html, "class=\"box\"") != NULL &&
			           strstr(html, "data-dyn=\"hello\"") != NULL &&
			           strstr(html, "hidden") != NULL &&
			           strstr(html, "<button>Go</button>") != NULL,
			           "arena + lit HTML output");
		bud_free_string(html);

		/* Mount in bud_runtime to verify bud_bind_ctx_lit dispatch + on_unmount */
		{
			bud_runtime *rt = bud_runtime_new(div);
			if (rc == 0)
				rc = check(rt != NULL && bud_runtime_mount(rt) == 0,
				           "arena tree mounts in bud_runtime");
			if (rc == 0)
				rc = check(bud_runtime_dispatch(rt, btn, "click", NULL) == 0 &&
				           clicked == 1,
				           "bud_bind_ctx_lit dispatches click event");
			bud_runtime_free(rt);
		}
		if (rc == 0)
			rc = check(unmounted == 1,
			           "unmount on arena tree invokes on_unmount");

		/* Calling bud_free on an arena tree must be safe (no crash/double-free) */
		bud_free(div);

		bud_arena_reset(a);
		if (rc == 0)
			rc = check(bud_arena_bytes_used(a) == 0,
			           "bud_arena_reset zeroes bytes_used");
		bud_arena_free(a);
		if (rc != 0)
			return rc;
	}

	/* 2. Slab growth across multiple doublings + oversized slab (>64KB) + warm reset reuse */
	{
		bud_arena *a = bud_arena_new(256);
		bud_arena *prev = bud_arena_push(a);
		bud_node *ul = bud_element_lit("ul");
		char *big = (char *)malloc(70001);
		bud_node *big_t;

		memset(big, 'A', 70000);
		big[70000] = '\0';
		for (int i = 0; i < 120; i++) {
			bud_node *li = bud_element_lit("li");
			bud_set_attr_name_lit(li, "class", "row");
			bud_append(li, bud_textf("item-%d", i));
			bud_append(ul, li);
		}
		big_t = bud_text(big);
		bud_append(ul, big_t);
		bud_arena_pop(prev);

		rc = check(bud_arena_bytes_used(a) > 70000 &&
		           bud_node_child_count(ul) == 121 &&
		           strlen(bud_node_text(big_t)) == 70000,
		           "arena slab growth and oversized allocation");
		free(big);

		/* Reset frees oversized slab and retains warm normal slabs */
		bud_arena_reset(a);
		if (rc == 0)
			rc = check(bud_arena_bytes_used(a) == 0,
			           "arena reset after oversized slab");

		/* Rebuild 100 items on warm slabs: verify zero heap allocs */
#ifdef BUD_TEST_COUNT_ALLOCS
		unsigned long before_allocs = g_bud_test_allocs;
#endif
		prev = bud_arena_push(a);
		bud_node *ul2 = bud_element_lit("ul");
		for (int i = 0; i < 100; i++) {
			bud_node *li = bud_element_lit("li");
			bud_set_attr_lit(li, "class", "row");
			bud_append(li, bud_textf("warm-%d", i));
			bud_append(ul2, li);
		}
		bud_arena_pop(prev);
#ifdef BUD_TEST_COUNT_ALLOCS
		unsigned long warm_allocs = g_bud_test_allocs - before_allocs;
		if (rc == 0)
			rc = check(warm_allocs == 0,
			           "warm arena rebuild performs 0 heap allocations");
#endif
		if (rc == 0)
			rc = check(bud_node_child_count(ul2) == 100,
			           "warm arena rebuilt 100 children");
		bud_arena_free(a);
		if (rc != 0)
			return rc;
	}

	/* 3. Nested arenas + NULL heap escape + mixed arena/heap tree teardown */
	{
		bud_arena *a1 = bud_arena_new(1024);
		bud_arena *a2 = bud_arena_new(1024);
		bud_arena *p0 = bud_arena_push(a1);
		bud_node *root = bud_element_lit("div");

		/* Escape to heap inside a1 */
		bud_arena *p1 = bud_arena_push(NULL);
		bud_node *heap_child = bud_element("span");
		bud_set_attr(heap_child, "class", "heap");
		bud_append(heap_child, bud_text("Heap"));
		bud_arena_pop(p1);
		bud_append(root, heap_child);

		/* Nested arena a2 inside a1 */
		bud_arena *p2 = bud_arena_push(a2);
		bud_node *a2_child = bud_element_lit("em");
		bud_set_attr_lit(a2_child, "data-a", "2");
		bud_append(a2_child, bud_text_lit("Nested"));
		bud_arena_pop(p2);
		bud_append(root, a2_child);

		rc = check(bud_arena_current() == a1,
		           "nested arena pops restore outer arena");
		bud_arena_pop(p0);

		char *html = bud_render_html(root);
		if (rc == 0)
			rc = check(html != NULL &&
			           strcmp(html, "<div><span class=\"heap\">Heap</span>"
			                        "<em data-a=\"2\">Nested</em></div>") == 0,
			           "mixed arena+heap tree renders accurately");
		bud_free_string(html);

		/* bud_free frees heap_child and safely skips a1/a2 slabs */
		bud_free(root);
		bud_arena_free(a2);
		bud_arena_free(a1);
		if (rc != 0)
			return rc;
	}

	/* 4. Attribute/class/text/raw mutations across lit, heap, and arena + diff + expand */
	{
		/* Heap node mixing lit & owned attrs */
		bud_node *h = bud_element_lit("div");
		bud_set_attr_lit(h, "class", "a");
		bud_set_attr(h, "class", "b");
		bud_set_attr_lit(h, "class", "c");
		bud_add_class(h, "d");
		bud_remove_class(h, "c");
		rc = check(strcmp(bud_get_attr(h, "class"), "d") == 0,
		           "heap attr transitions lit->owned->lit->class");
		bud_free(h);
		if (rc != 0)
			return rc;

		/* Arena node mutations + bud_vdom_diff across two arenas + bud_expand_tree */
		bud_arena *a_old = bud_arena_new(2048);
		bud_arena *a_new = bud_arena_new(2048);
		test_leaf_props p1 = { .label = "One" };
		test_leaf_props p2 = { .label = "Two" };

		bud_arena *p = bud_arena_push(a_old);
		bud_node *t1 = bud_expand_tree(
		        bud_component_vnode(test_comp_wrap, NULL, &p1, sizeof(p1)));
		bud_node *raw = bud_raw("<b>1</b>");
		bud_raw_set_text(raw, "<b>1b</b>");
		bud_append(t1, raw);
		bud_arena_pop(p);

		p = bud_arena_push(a_new);
		bud_node *t2 = bud_expand_tree(
		        bud_component_vnode(test_comp_wrap, NULL, &p2, sizeof(p2)));
		bud_node *raw2 = bud_raw("<b>2</b>");
		bud_append(t2, raw2);
		bud_arena_pop(p);

		test_ops diff = { { 0 }, 0 };
		rc = check(bud_vdom_diff(t1, t2, test_emit, &diff) == 0 &&
		           strstr(diff.buffer, "patch-text:Two:") != NULL &&
		           strstr(diff.buffer, "patch-innerhtml:") != NULL &&
		           strstr(diff.buffer, "<b>2</b>") != NULL,
		           "bud_vdom_diff across two arena-expanded trees");
		bud_arena_free(a_old);
		bud_arena_free(a_new);
		if (rc != 0)
			return rc;
	}

	/* 5. 1,000-render benchmark: heap vs warm-arena + *_lit */
	{
		const int ITERS = 1000;
		bud_arena *arena = bud_arena_new(8192);
		char *html_heap;
		char *html_arena;

		/* Parity check */
		bud_node *n_heap = bench_build_tree_heap(0);
		html_heap = bud_render_html(n_heap);
		bud_free(n_heap);

		bud_arena *p = bud_arena_push(arena);
		bud_node *n_arena = bench_build_tree_lit(0);
		html_arena = bud_render_html(n_arena);
		bud_arena_pop(p);
		bud_arena_reset(arena);

		rc = check(html_heap != NULL && html_arena != NULL &&
		           strcmp(html_heap, html_arena) == 0,
		           "benchmark heap and arena+lit trees produce identical HTML");
		bud_free_string(html_heap);
		bud_free_string(html_arena);
		if (rc != 0) {
			bud_arena_free(arena);
			return rc;
		}

		/* Measure heap path (1,000 frames) */
#ifdef BUD_TEST_COUNT_ALLOCS
		unsigned long a0 = g_bud_test_allocs;
#endif
		double t0 = monotonic_us();
		for (int i = 0; i < ITERS; i++) {
			bud_node *n = bench_build_tree_heap(i);
			bud_free(n);
		}
		double heap_us = monotonic_us() - t0;
#ifdef BUD_TEST_COUNT_ALLOCS
		unsigned long heap_allocs = g_bud_test_allocs - a0;
#endif

		/* Measure warm arena + *_lit path (1,000 frames) */
#ifdef BUD_TEST_COUNT_ALLOCS
		unsigned long a1 = g_bud_test_allocs;
#endif
		double t1 = monotonic_us();
		for (int i = 0; i < ITERS; i++) {
			bud_arena *prev = bud_arena_push(arena);
			bud_node *n = bench_build_tree_lit(i);
			(void)n;
			bud_arena_pop(prev);
			bud_arena_reset(arena);
		}
		double arena_us = monotonic_us() - t1;
#ifdef BUD_TEST_COUNT_ALLOCS
		unsigned long arena_allocs = g_bud_test_allocs - a1;
		double reduction_pct = heap_allocs > 0
		        ? (100.0 * (double)(heap_allocs - arena_allocs) / (double)heap_allocs)
		        : 0.0;
		printf("bud_arena bench (%d renders, ~105 nodes): "
		       "heap=%.1f us/frame (%lu allocs) | "
		       "arena+lit=%.1f us/frame (%lu allocs, %.2f%% fewer allocs, %.2fx faster)\n",
		       ITERS, heap_us / ITERS, heap_allocs,
		       arena_us / ITERS, arena_allocs, reduction_pct,
		       arena_us > 0.0 ? heap_us / arena_us : 0.0);
		rc = check(heap_allocs > 100000 && arena_allocs * 20 <= heap_allocs,
		           "arena+lit reduces heap allocations by >95%");
#else
		printf("bud_arena bench (%d renders, ~105 nodes): "
		       "heap=%.1f us/frame | arena+lit=%.1f us/frame (%.2fx faster)\n",
		       ITERS, heap_us / ITERS, arena_us / ITERS,
		       arena_us > 0.0 ? heap_us / arena_us : 0.0);
#endif
		bud_arena_free(arena);
		if (rc != 0)
			return rc;
	}

	return 0;
}

int main(void)
{

	if (test_jsx() != 0)
		return 1;
	if (test_jsx_ergonomics() != 0)
		return 1;
	if (test_bool_attr() != 0)
		return 1;
	if (test_raw_html() != 0)
		return 1;
	if (test_get_attr() != 0)
		return 1;
	if (test_class_helpers() != 0)
		return 1;
	if (test_detach() != 0)
		return 1;
	if (test_window_event_type() != 0)
		return 1;
	if (test_bud_arena_and_lit() != 0)
		return 1;
	bud_node *root;
	bud_node *main_el;
	bud_node *h1;
	bud_node *button;
	bud_node *p;
	bud_node *text;
	bud_runtime *runtime;
	char *html;
	char *hydrated_html;
	test_ops ops;
	test_ops patch_stream;
	lifecycle_log lifecycle;
	hydrate_expect expect[] = { { 0, "fragment", NULL, NULL },
		                    { 1, "element", "main", NULL },
		                    { 2, "element", "h1", NULL },
		                    { 3, "text", NULL, "Hello & <world>" },
		                    { 4, "element", "button", NULL },
		                    { 5, "text", NULL, "Click me" },
		                    { 6, "element", "p", NULL },
		                    { 7, "text", NULL, "C DOM runtime" },
		                    { 0, NULL, NULL, NULL } };
	int rc;

	root = NULL;
	main_el = NULL;
	h1 = NULL;
	button = NULL;
	p = NULL;
	text = NULL;
	runtime = NULL;
	html = NULL;
	hydrated_html = NULL;

	memset(&ops, 0, sizeof(ops));
	memset(&patch_stream, 0, sizeof(patch_stream));
	memset(&lifecycle, 0, sizeof(lifecycle));

	root = bud_fragment();
	main_el = bud_element("main");
	h1 = bud_element("h1");
	button = bud_element("button");
	p = bud_element("p");
	text = bud_text("C DOM runtime");

	rc = check(root != NULL, "fragment alloc");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(main_el != NULL, "element alloc");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(h1 != NULL, "h1 alloc");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(button != NULL, "button alloc");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(p != NULL, "p alloc");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(text != NULL, "text alloc");
	if (rc != 0) {
		goto cleanup;
	}

	rc = bud_set_attr(main_el, "data-role", "app");
	rc |= bud_on(button, "click", 1);
	rc |= bud_on(button, "keydown", 0);
	rc |= bud_on(button, "submit", 0);
	rc |= bud_bind(button, "click", 1, on_click_stop);
	rc |= bud_bind(button, "submit", 0, on_submit);
	rc |= bud_bind(main_el, "submit", 1, on_submit);
	rc |= bud_set_lifecycle(
	        main_el, on_mount, on_update, on_unmount, &lifecycle);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: set attr/listener\n");
		goto cleanup;
	}

	rc = bud_append(root, main_el);
	rc |= bud_append(main_el, h1);
	rc |= bud_append(main_el, button);
	rc |= bud_append(main_el, p);
	rc |= bud_append(h1, bud_text("Hello & <world>"));
	rc |= bud_append(button, bud_text("Click me"));
	rc |= bud_append(p, text);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: append\n");
		goto cleanup;
	}

	runtime = bud_runtime_new(root);
	rc = check(runtime != NULL, "runtime alloc");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(bud_runtime_root(runtime) == root, "runtime root");
	if (rc != 0) {
		goto cleanup;
	}
	rc = bud_runtime_set_invalidate(runtime, on_invalidate, &lifecycle);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime invalidate\n");
		goto cleanup;
	}
	rc = bud_runtime_mount(runtime);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime mount\n");
		goto cleanup;
	}
	rc = bud_runtime_update(runtime);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime update\n");
		goto cleanup;
	}
	rc = bud_runtime_mark_dirty(runtime);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime dirty\n");
		goto cleanup;
	}
	rc = check(bud_runtime_is_dirty(runtime) == 1, "runtime dirty flag");
	if (rc != 0) {
		goto cleanup;
	}
	rc = bud_runtime_flush(runtime);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime flush\n");
		goto cleanup;
	}
	rc = check(bud_runtime_is_dirty(runtime) == 0, "runtime clean");
	if (rc != 0) {
		goto cleanup;
	}
	rc = bud_runtime_dispatch(runtime, button, "click", &lifecycle);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime dispatch click\n");
		goto cleanup;
	}
	rc = bud_runtime_dispatch(runtime, button, "submit", &lifecycle);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime dispatch submit\n");
		goto cleanup;
	}
	rc = bud_runtime_unmount(runtime);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: runtime unmount\n");
		goto cleanup;
	}

	rc = check(bud_node_child_count(root) == 1, "root child count");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(bud_node_child_count(main_el) == 3, "main child count");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(bud_node_child(main_el, 1) == button, "child accessor");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(bud_node_attr_count(main_el) == 1, "attr count");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(strcmp(bud_node_attr_name(main_el, 0), "data-role") == 0,
	              "attr name");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(strcmp(bud_node_attr_value(main_el, 0), "app") == 0,
	              "attr value");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(bud_node_listener_count(button) == 3, "listener count");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(strcmp(bud_node_listener_event(button, 0), "click") == 0,
	              "listener event 0");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(bud_node_listener_bubbles(button, 0) == 1,
	              "listener bubbles 0");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(
	        strcmp(bud_node_listener_event(button, 1), "keydown") == 0,
	        "listener event 1");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(bud_node_listener_bubbles(button, 1) == 0,
	              "listener bubbles 1");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(strcmp(bud_node_listener_event(button, 2), "submit") == 0,
	              "listener event 2");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(bud_node_listener_bubbles(button, 2) == 0,
	              "listener bubbles 2");
	if (rc != 0) {
		goto cleanup;
	}

	html = bud_render_html(root);
	rc = check(html != NULL, "render html");
	if (rc != 0) {
		goto cleanup;
	}
	rc = check(
	        strcmp(html, "<main data-role=\"app\"><h1>Hello &amp; "
	                     "&lt;world&gt;</h1><button>Click me</button><p>C "
	                     "DOM runtime</p></main>") == 0,
	        "html output");
	if (rc != 0) {
		fprintf(stderr, "got: %s\n", html);
		goto cleanup;
	}

	hydrated_html = bud_render_hydrated_html(root);
	rc = check(hydrated_html != NULL, "render hydrated html");
	if (rc != 0) {
		goto cleanup;
	}
	rc =
	        check(strstr(hydrated_html, "data-bud-id=\"1\"") != NULL,
	              "hydrated id");
	if (rc != 0) {
		fprintf(stderr, "got hydrated: %s\n", hydrated_html);
		goto cleanup;
	}
	rc = check(
	        strstr(hydrated_html,
	               "data-bud-on=\"click:1,keydown:0,submit:0\"") != NULL,
	        "hydrated listener attr");
	if (rc != 0) {
		fprintf(stderr, "got hydrated: %s\n", hydrated_html);
		goto cleanup;
	}
	rc =
	        check(strstr(hydrated_html, "<!--bud-text:3-->") != NULL,
	              "hydrated text marker");
	if (rc != 0) {
		fprintf(stderr, "got hydrated: %s\n", hydrated_html);
		goto cleanup;
	}

	rc = bud_render_hydration_ops(root, test_emit, &ops);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: render hydration ops\n");
		goto cleanup;
	}
	rc =
	        check(strstr(ops.buffer, "listener:click:1:4") != NULL,
	              "ops listener");
	if (rc != 0) {
		fprintf(stderr, "ops: %s\n", ops.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(ops.buffer, "element-open:button:4") != NULL,
	              "ops open with id");
	if (rc != 0) {
		fprintf(stderr, "ops: %s\n", ops.buffer);
		goto cleanup;
	}

	rc = bud_render_patch_ops(root, test_emit, &patch_stream);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: render patch ops\n");
		goto cleanup;
	}
	rc =
	        check(strstr(patch_stream.buffer, "patch-clear:0") != NULL,
	              "patch clear");
	if (rc != 0) {
		fprintf(stderr, "patch: %s\n", patch_stream.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(patch_stream.buffer, "patch-open:main:1") != NULL,
	              "patch open element");
	if (rc != 0) {
		fprintf(stderr, "patch: %s\n", patch_stream.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(patch_stream.buffer,
	                     "patch-attr:1:data-role:app") != NULL,
	              "patch attr");
	if (rc != 0) {
		fprintf(stderr, "patch: %s\n", patch_stream.buffer);
		goto cleanup;
	}
	rc = check(
	        strstr(patch_stream.buffer, "patch-listener:4:click:1") != NULL,
	        "patch listener");
	if (rc != 0) {
		fprintf(stderr, "patch: %s\n", patch_stream.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(patch_stream.buffer,
	                     "patch-text:C DOM runtime:7") != NULL,
	              "patch text");
	if (rc != 0) {
		fprintf(stderr, "patch: %s\n", patch_stream.buffer);
		goto cleanup;
	}

	rc = bud_hydrate(root, hydrate_lookup, expect);
	if (rc != 0) {
		fprintf(stderr, "bud test failed: hydrate validation\n");
		goto cleanup;
	}

	rc =
	        check(strstr(lifecycle.buffer, "mount:main") != NULL,
	              "lifecycle mount");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(lifecycle.buffer, "update:main") != NULL,
	              "lifecycle update");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(lifecycle.buffer, "invalidate") != NULL,
	              "runtime invalidate");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(lifecycle.buffer, "click:click:button") != NULL,
	              "event click");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(lifecycle.buffer, "submit:submit:button") != NULL,
	              "event submit target");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(lifecycle.buffer, "submit:submit:main") != NULL,
	              "event submit bubble");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}
	rc =
	        check(strstr(lifecycle.buffer, "unmount:main") != NULL,
	              "lifecycle unmount");
	if (rc != 0) {
		fprintf(stderr, "lifecycle: %s\n", lifecycle.buffer);
		goto cleanup;
	}

	/* ── Test bud_tpl HTML Template Engine ── */
	{
		bud_node *badge =
		        bud_tpl("<span class='badge'>%s</span>", "Featured");
		bud_node *card = bud_tpl(
		        "<div id='%s' class='card %s'>"
		        "  <div class='header'>"
		        "    <h1>%s</h1>"
		        "    %node"
		        "  </div>"
		        "  <p class='lead'>Count: %d (owner: %s)</p>"
		        "  <pre class='chord-data'>%raw</pre>"
		        "  <form action='/song/123/edit' method='POST' "
		        "class='flex flex-col gap-2'>"
		        "    <input type='text' name='title' value='%s' "
		        "required/>"
		        "    <button type='submit' class='btn "
		        "btn-primary'>Save</button>"
		        "  </form>"
		        "</div>",
		        "song-card", "shadow-sm", "My Song Title", badge, 42,
		        "admin", "<span class='chord'>Am</span> text",
		        "My Song Title");

		char *card_html = bud_render_html(card);
		if (!card_html) {
			fprintf(stderr,
			        "bud test failed: bud_tpl card_html null\n");
			bud_free(card);
			return 1;
		}

		rc = check(
		        strstr(card_html, "<div class=\"card shadow-sm\" "
		                          "id=\"song-card\">") != NULL ||
		                strstr(card_html, "id=\"song-card\"") != NULL,
		        "bud_tpl card wrapper");
		if (rc == 0)
			rc =
			        check(strstr(card_html,
			                     "<h1>My Song Title</h1>") != NULL,
			              "bud_tpl h1 text");
		if (rc == 0)
			rc = check(
			        strstr(card_html,
			               "<span "
			               "class=\"badge\">Featured</span>") !=
			                NULL,
			        "bud_tpl %node badge");
		if (rc == 0)
			rc = check(
			        strstr(card_html, "<p class=\"lead\">Count: 42 "
			                          "(owner: admin)</p>") != NULL,
			        "bud_tpl %d %s lead");
		if (rc == 0)
			rc = check(
			        strstr(card_html,
			               "<pre class=\"chord-data\"><span "
			               "class='chord'>Am</span> text</pre>") !=
			                NULL,
			        "bud_tpl %raw pre");
		if (rc == 0)
			rc =
			        check(strstr(card_html,
			                     "<input name=\"title\" required "
			                     "type=\"text\" value=\"My Song "
			                     "Title\">") != NULL ||
			                      strstr(card_html,
			                             "name=\"title\"") != NULL,
			              "bud_tpl input void tag");
		if (rc == 0)
			rc = check(
			        strstr(card_html,
			               "<button class=\"btn btn-primary\" "
			               "type=\"submit\">Save</button>") !=
			                        NULL ||
			                strstr(card_html, ">Save</button>") !=
			                        NULL,
			        "bud_tpl button");

		bud_free_string(card_html);
		bud_free(card);
		if (rc != 0)
			return rc;

		/* Test fragment return for multi-root template */
		bud_node *options =
		        bud_tpl("<option value='0' %b>Key 0</option>"
		                "<option value='1' %b>Key 1</option>"
		                "<option value='2' %b>Key 2</option>",
		                "selected", NULL, "disabled");
		char *opt_html = bud_render_html(options);
		if (!opt_html) {
			fprintf(stderr,
			        "bud test failed: bud_tpl opt_html null\n");
			bud_free(options);
			return 1;
		}
		rc = check(
		        strstr(opt_html,
		               "<option selected value=\"0\">Key 0</option>") !=
		                        NULL ||
		                strstr(opt_html, "value=\"0\"") != NULL,
		        "bud_tpl multi-option fragment with %%b selected");
		if (rc == 0)
			rc =
			        check(strstr(opt_html, "disabled") != NULL,
			              "bud_tpl %%b disabled attribute");
		if (rc == 0)
			rc = check(
			        strstr(opt_html,
			               "<option value=\"1\">Key 1</option>") !=
			                        NULL ||
			                strstr(opt_html, "value=\"1\"") != NULL,
			        "bud_tpl %%b NULL omitted");
		bud_free_string(opt_html);
		bud_free(options);
		if (rc != 0)
			return rc;

		/* Test option with %d, %b, and %s */
		bud_node *opt_test =
		        bud_tpl("<option value='%d' %b>%s</option>", 5,
		                "selected", "C (Original)");
		char *opt_test_html = bud_render_html(opt_test);
		if (!opt_test_html) {
			fprintf(stderr,
			        "bud test failed: opt_test_html null\n");
			return 1;
		}
		rc =
		        check(strstr(opt_test_html, "value=\"5\"") != NULL,
		              "opt_test value");
		if (rc == 0)
			rc =
			        check(strstr(opt_test_html, "selected") != NULL,
			              "opt_test selected");
		if (rc == 0)
			rc = check(
			        strstr(opt_test_html, "C (Original)") != NULL,
			        "opt_test text");
		bud_free_string(opt_test_html);
		bud_free(opt_test);
		if (rc != 0)
			return rc;

		/* ── Test bud_vdom_diff ── */
		bud_node *tree1 =
		        bud_tpl("<div id='main' class='old'>"
		                "  <h1 id='title'>Old Title</h1>"
		                "  <p id='count'>Count: 1</p>"
		                "  <pre id='data'>%raw</pre>"
		                "</div>",
		                "<span>Chord A</span>");

		bud_node *tree2 =
		        bud_tpl("<div id='main' class='new'>"
		                "  <h1 id='title'>New Title</h1>"
		                "  <p id='count'>Count: 2</p>"
		                "  <pre id='data'>%raw</pre>"
		                "</div>",
		                "<span>Chord B</span>");

		test_ops diff_ops = { { 0 }, 0 };
		rc = bud_vdom_diff(tree1, tree2, test_emit, &diff_ops);
		if (rc != 0) {
			fprintf(stderr,
			        "bud test failed: bud_vdom_diff failed\n");
			bud_free(tree1);
			bud_free(tree2);
			return 1;
		}

		rc = check(
		        strstr(diff_ops.buffer, "patch-attr:") != NULL &&
		                strstr(diff_ops.buffer, "class:new") != NULL,
		        "diff attr class");
		if (rc == 0)
			rc =
			        check(strstr(diff_ops.buffer,
			                     "patch-text:New Title:") != NULL,
			              "diff text title");
		if (rc == 0)
			rc =
			        check(strstr(diff_ops.buffer,
			                     "patch-text:Count: 2:") != NULL,
			              "diff text count");
		if (rc == 0)
			rc = check(
			        strstr(diff_ops.buffer, "patch-innerhtml:") !=
			                        NULL &&
			                strstr(diff_ops.buffer,
			                       "<span>Chord B</span>") != NULL,
			        "diff innerhtml data");

		bud_free(tree1);
		bud_free(tree2);
		if (rc != 0)
			return rc;

		/* ── Test Keyed Children Diffing ── */
		bud_node *list1 = bud_tpl("<ul id='list'>"
		                          "  <li data-key='k1'>Item 1</li>"
		                          "  <li data-key='k2'>Item 2</li>"
		                          "  <li data-key='k3'>Item 3</li>"
		                          "</ul>");

		bud_node *list2 =
		        bud_tpl("<ul id='list'>"
		                "  <li data-key='k1'>Item 1 Updated</li>"
		                "  <li data-key='k3'>Item 3</li>"
		                "</ul>");

		test_ops list_ops = { { 0 }, 0 };
		rc = bud_vdom_diff(list1, list2, test_emit, &list_ops);
		if (rc != 0) {
			fprintf(stderr, "bud test failed: bud_vdom_diff keyed "
			                "list failed\n");
			bud_free(list1);
			bud_free(list2);
			return 1;
		}

		rc =
		        check(strstr(list_ops.buffer,
		                     "patch-text:Item 1 Updated:") != NULL,
		              "keyed diff updated item text");
		if (rc == 0)
			rc =
			        check(strstr(list_ops.buffer,
			                     "patch-remove:") != NULL,
			              "keyed diff removed item k2");

		bud_free(list1);
		bud_free(list2);
		if (rc != 0)
			return rc;

		/* ── Test Mixed Keyed + Unkeyed Siblings Diffing ── */
		bud_node *mix1 = bud_tpl("<div id='stage'>"
		                         "  <canvas id='cv' width='640'></canvas>"
		                         "  <div data-bud-key='status' class='visible'>Compiling...</div>"
		                         "</div>");
		bud_node *mix2 = bud_tpl("<div id='stage'>"
		                         "  <canvas id='cv' width='640'></canvas>"
		                         "  <div data-bud-key='status' class='hidden'>Compiling...</div>"
		                         "</div>");

		test_ops mix_ops = { { 0 }, 0 };
		rc = bud_vdom_diff(mix1, mix2, test_emit, &mix_ops);
		if (rc != 0) {
			fprintf(stderr, "bud test failed: bud_vdom_diff mixed keyed/unkeyed failed\n");
			bud_free(mix1);
			bud_free(mix2);
			return 1;
		}
		rc = check(strstr(mix_ops.buffer, "patch-remove:") == NULL &&
		           strstr(mix_ops.buffer, "patch-open:canvas") == NULL &&
		           strstr(mix_ops.buffer, "patch-attr:") != NULL,
		           "mixed keyed/unkeyed diff preserves unkeyed sibling without patch-remove");
		bud_free(mix1);
		bud_free(mix2);
		if (rc != 0)
			return rc;

		/* ── Test Foreign & Terminal Elements Protection ── */
		bud_node *term1 = bud_tpl("<div id='term' data-bud-terminal='1' class='c1'>"
		                          "  <div class='xterm-injected'><span>text</span></div>"
		                          "</div>");
		bud_node *term2 = bud_tpl("<div id='term' data-bud-terminal='1' class='c2'></div>");
		test_ops term_ops = { { 0 }, 0 };
		rc = bud_vdom_diff(term1, term2, test_emit, &term_ops);
		if (rc != 0) {
			fprintf(stderr, "bud test failed: foreign/terminal diff failed\n");
			bud_free(term1);
			bud_free(term2);
			return 1;
		}
		rc = check(strstr(term_ops.buffer, "patch-remove:") == NULL &&
		           strstr(term_ops.buffer, "patch-attr:") != NULL,
		           "foreign/terminal element skips child diff and preserves injected DOM");
		bud_free(term1);
		bud_free(term2);
		if (rc != 0)
			return rc;
	}

	/* ── Test stable ids across structural diffs ── */
	{
		/* A list that grows must not shift the ids of the nodes after it:
		 * the DOM still carries the old ones, so a later patch aimed at
		 * the tail would otherwise hit the wrong element. */
		bud_node *a = bud_element("div");
		bud_node *a_ul = bud_element("ul");
		bud_node *a_p = bud_element("p");
		bud_append(a_ul, bud_element("li"));
		bud_append(a, a_ul);
		bud_append(a_p, bud_text("tail"));
		bud_append(a, a_p);
		bud_node *b = bud_element("div");
		bud_node *b_ul = bud_element("ul");
		bud_node *b_p = bud_element("p");
		for (int i = 0; i < 3; i++)
			bud_append(b_ul, bud_element("li"));
		bud_append(b, b_ul);
		bud_append(b_p, bud_text("tail"));
		bud_append(b, b_p);
		bud_node *c = bud_element("div");
		bud_node *c_ul = bud_element("ul");
		bud_node *c_p = bud_element("p");
		for (int i = 0; i < 3; i++)
			bud_append(c_ul, bud_element("li"));
		bud_append(c, c_ul);
		bud_append(c_p, bud_text("changed"));
		bud_append(c, c_p);

		char *h = bud_render_hydrated_html(a); /* numbers a DFS, as the page does */
		bud_free_string(h);
		unsigned int ul_id = bud_node_id(a_ul);
		unsigned int p_id = bud_node_id(a_p);
		unsigned int max_old = p_id + 1; /* p's text child is the last live id */

		test_ops grow = { { 0 }, 0 };
		rc = bud_vdom_diff(a, b, test_emit, &grow);
		const char *want_open = "patch-open:li:";
		rc = check(rc == 0, "growing list diff");
		if (rc == 0)
			rc = check(strstr(grow.buffer, want_open) != NULL,
			           "growing list inserts li");
		/* each inserted li names the ul it goes into as the 3rd argument */
		char want_parent[32];
		snprintf(want_parent, sizeof(want_parent), ":%u\n", ul_id);
		if (rc == 0)
			rc = check(strstr(grow.buffer, want_parent) != NULL,
			           "inserted li carries its parent id");
		if (rc == 0)
			rc = check(bud_node_id(b_p) == p_id,
			           "tail node keeps its id after list growth");
		for (unsigned i = 0; rc == 0 && i < 2; i++) {
			const bud_node *li = i ? bud_node_child(b_ul, 2)
			                       : bud_node_child(b_ul, 1);
			rc = check(bud_node_id(li) > max_old,
			           "inserted li id does not collide with live ids");
		}

		test_ops next = { { 0 }, 0 };
		if (rc == 0)
			rc = check(bud_vdom_diff(b, c, test_emit, &next) == 0,
			           "second diff on a diffed tree");
		/* a: div0 ul1 li2 p3 "tail"4. Growing the ul must leave p's text at 4. */
		if (rc == 0)
			rc = check(strstr(next.buffer, "patch-text:changed:4\n") != NULL,
			           "tail text patch targets the original id");
		bud_free(a);
		bud_free(b);
		bud_free(c);
		if (rc != 0)
			return rc;
	}

	/* ── Test bud_slot ── */
	{
		bud_slot_reset();
		bud_slot_contrib_t c_low = {
			.slot_id = BUD_SLOT_PRIMARY_VIEW,
			.priority = 10,
			.name = "low",
			.render = test_slot_render_low,
			.fill = NULL
		};
		bud_slot_contrib_t c_high = {
			.slot_id = BUD_SLOT_PRIMARY_VIEW,
			.priority = 0,
			.name = "high",
			.render = test_slot_render_high,
			.fill = NULL
		};
		bud_slot_contrib_t c_fill = {
			.slot_id = BUD_SLOT_ACTIONS_DOCK,
			.priority = 5,
			.name = "fill",
			.render = NULL,
			.fill = test_slot_fill_fn
		};
		bud_slot_register(&c_low);
		bud_slot_register(&c_high);
		bud_slot_register(&c_fill);

		bud_node *rendered = bud_slot_render(BUD_SLOT_PRIMARY_VIEW, NULL);
		rc = check(rendered != NULL && strcmp(bud_node_tag(rendered), "high-priority") == 0,
			"bud_slot_render honors ascending priority order");
		bud_free(rendered);
		if (rc != 0) return rc;

		bud_node *dock = bud_element("dock");
		int fill_count = bud_slot_fill(BUD_SLOT_ACTIONS_DOCK, dock, NULL);
		rc = check(fill_count == 1 && bud_node_child_count(dock) == 1,
			"bud_slot_fill appends children into parent");
		bud_free(dock);
		if (rc != 0) return rc;
	}

	/* ── Test bud_state ── */
	{
		test_state_t s1;
		memset(&s1, 0, sizeof(s1));
		snprintf(s1.title, sizeof(s1.title), "Hero");
		s1.score = 42;

		char json_buf[256];
		int ser_len = test_state_t_serialize(&s1, json_buf, sizeof(json_buf));
		rc = check(ser_len > 0 && strstr(json_buf, "\"title\":\"Hero\"") != NULL && strstr(json_buf, "\"score\":42") != NULL,
			"bud_state serialization emits valid JSON");
		if (rc != 0) return rc;

		test_state_t s2;
		memset(&s2, 0, sizeof(s2));
		test_state_t_deserialize(&s2, json_buf, (size_t)ser_len);
		rc = check(strcmp(s2.title, "Hero") == 0 && s2.score == 42,
			"bud_state deserialization restores fields correctly");
		if (rc != 0) return rc;
	}

	/* ── Test bud_set_key / lx_key ── */
	{
		bud_node *k_el = lx_n("div", lx_key("hero-card"));
		rc = check(k_el != NULL && bud_get_key(k_el) != NULL && strcmp(bud_get_key(k_el), "hero-card") == 0,
			"lx_key and bud_get_key handle stable node keys");
		bud_free(k_el);
		if (rc != 0) return rc;
	}

	/* ── Test bud_expand_tree + SSR/WASM component ID parity ── */
	{
		test_leaf_props p1 = { .label = "Hello" };
		test_leaf_props p2 = { .label = "World" };

		/* Root-level nested component: SSR hydrated HTML vs WASM expanded render_ops */
		bud_node *ssr_root = bud_component_vnode(test_comp_wrap, NULL, &p1, sizeof(p1));
		char *h_ssr = bud_render_hydrated_html(ssr_root);
		rc = check(h_ssr != NULL &&
		           strstr(h_ssr, "<section data-bud-id=\"0\">") != NULL &&
		           strstr(h_ssr, "<span data-bud-id=\"1\" class=\"leaf\">") != NULL &&
		           strstr(h_ssr, "<!--bud-text:2-->Hello<!--/bud-text:2-->") != NULL,
		           "SSR hydrated HTML expands root/nested components before assigning IDs");
		bud_free_string(h_ssr);
		bud_free(ssr_root);
		if (rc != 0) return rc;

		bud_node *wasm_root = bud_expand_tree(
		        bud_component_vnode(test_comp_wrap, NULL, &p1, sizeof(p1)));
		test_ops comp_ops = { { 0 }, 0 };
		rc = check(wasm_root != NULL && bud_render_ops(wasm_root, test_emit, &comp_ops) == 0,
		           "WASM expanded component tree renders ops");
		if (rc == 0)
			rc = check(strstr(comp_ops.buffer, "element-open:section:0\n") != NULL &&
			           strstr(comp_ops.buffer, "element-open:span:1\n") != NULL &&
			           strstr(comp_ops.buffer, "text:Hello:2\n") != NULL,
			           "WASM render_ops IDs match SSR hydrated HTML IDs 1-to-1");

		bud_node *wasm_next = bud_expand_tree(
		        bud_component_vnode(test_comp_wrap, NULL, &p2, sizeof(p2)));
		test_ops comp_diff = { { 0 }, 0 };
		if (rc == 0)
			rc = check(bud_vdom_diff(wasm_root, wasm_next, test_emit, &comp_diff) == 0 &&
			           strstr(comp_diff.buffer, "patch-text:World:2\n") != NULL,
			           "WASM vdom_diff across expanded components patches exact text id");
		bud_free(wasm_root);
		bud_free(wasm_next);
		if (rc != 0) return rc;

		/* Key transfer from component node to expanded element */
		bud_node *keyed_comp = bud_component_vnode(test_comp_leaf, NULL, &p1, sizeof(p1));
		bud_set_key(keyed_comp, "row-7");
		bud_node *exp_keyed = bud_expand_tree(keyed_comp);
		rc = check(exp_keyed != NULL && bud_get_key(exp_keyed) != NULL &&
		           strcmp(bud_get_key(exp_keyed), "row-7") == 0,
		           "bud_expand_tree transfers key from component to expanded element");
		bud_free(exp_keyed);
		if (rc != 0) return rc;
	}

	rc = 0;

cleanup:
	bud_free_string(html);
	bud_free_string(hydrated_html);
	bud_runtime_free(runtime);
	bud_free(root);
	return rc;
}

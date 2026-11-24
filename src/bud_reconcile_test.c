#include "../include/bud/bud.h"
#include "../include/bud/bud_reconcile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Counting Mock Host (proves exact host ops & zero leaks) ──────────── */

typedef struct mock_attr {
	char *k;
	char *v;
	struct mock_attr *next;
} mock_attr;

typedef struct mock_ev {
	char *name;
	int bubbles;
	bud_event_handler_fn fn;
	void *ud;
	struct mock_ev *next;
} mock_ev;

typedef struct mock_node {
	int is_text;
	char *tag;
	char *text;
	mock_attr *attrs;
	mock_ev *events;
	struct mock_node *parent;
	struct mock_node *first_child;
	struct mock_node *next_sibling;
} mock_node;

typedef struct mock_stats {
	int create_el;
	int create_text;
	int set_text;
	int set_attr;
	int remove_attr;
	int insert_before;
	int remove_child;
	int destroy_node;
	int set_event;
	int clear_children;
	int live_nodes;
} mock_stats;

static char *mstrdup(const char *s)
{
	size_t n = strlen(s ? s : "");
	char *p = (char *)malloc(n + 1);
	memcpy(p, s ? s : "", n + 1);
	return p;
}

static void mock_reset_counts(mock_stats *st)
{
	int live = st->live_nodes;
	memset(st, 0, sizeof(*st));
	st->live_nodes = live;
}

static void mock_detach(mock_node *child)
{
	if (!child || !child->parent)
		return;
	mock_node *p = child->parent;
	mock_node **pp = &p->first_child;
	while (*pp) {
		if (*pp == child) {
			*pp = child->next_sibling;
			break;
		}
		pp = &(*pp)->next_sibling;
	}
	child->parent = NULL;
	child->next_sibling = NULL;
}

static void mock_destroy_rec(mock_stats *st, mock_node *n)
{
	if (!n)
		return;
	mock_node *c = n->first_child;
	while (c) {
		mock_node *nx = c->next_sibling;
		mock_destroy_rec(st, c);
		c = nx;
	}
	mock_attr *a = n->attrs;
	while (a) {
		mock_attr *nx = a->next;
		free(a->k);
		free(a->v);
		free(a);
		a = nx;
	}
	mock_ev *e = n->events;
	while (e) {
		mock_ev *nx = e->next;
		free(e->name);
		free(e);
		e = nx;
	}
	free(n->tag);
	free(n->text);
	free(n);
	st->live_nodes--;
}

static void *mh_create_el(void *ctx, const char *tag)
{
	mock_stats *st = (mock_stats *)ctx;
	mock_node *n = (mock_node *)calloc(1, sizeof(*n));
	n->tag = mstrdup(tag);
	st->create_el++;
	st->live_nodes++;
	return n;
}

static void *mh_create_text(void *ctx, const char *text, size_t len)
{
	(void)len;
	mock_stats *st = (mock_stats *)ctx;
	mock_node *n = (mock_node *)calloc(1, sizeof(*n));
	n->is_text = 1;
	n->text = mstrdup(text);
	st->create_text++;
	st->live_nodes++;
	return n;
}

static void mh_set_text(void *ctx, void *hn, const char *text, size_t len)
{
	(void)len;
	mock_stats *st = (mock_stats *)ctx;
	mock_node *n = (mock_node *)hn;
	free(n->text);
	n->text = mstrdup(text);
	st->set_text++;
}

static void mh_set_attr(void *ctx, void *hn, const char *k, const char *v)
{
	mock_stats *st = (mock_stats *)ctx;
	mock_node *n = (mock_node *)hn;
	st->set_attr++;
	for (mock_attr *a = n->attrs; a; a = a->next) {
		if (strcmp(a->k, k) == 0) {
			free(a->v);
			a->v = mstrdup(v);
			return;
		}
	}
	mock_attr *a = (mock_attr *)calloc(1, sizeof(*a));
	a->k = mstrdup(k);
	a->v = mstrdup(v);
	a->next = n->attrs;
	n->attrs = a;
}

static void mh_remove_attr(void *ctx, void *hn, const char *k)
{
	mock_stats *st = (mock_stats *)ctx;
	mock_node *n = (mock_node *)hn;
	st->remove_attr++;
	mock_attr **pp = &n->attrs;
	while (*pp) {
		if (strcmp((*pp)->k, k) == 0) {
			mock_attr *rm = *pp;
			*pp = rm->next;
			free(rm->k);
			free(rm->v);
			free(rm);
			return;
		}
		pp = &(*pp)->next;
	}
}

static void mh_insert_before(void *ctx, void *hp, void *hc, void *hb)
{
	mock_stats *st = (mock_stats *)ctx;
	mock_node *p = (mock_node *)hp;
	mock_node *c = (mock_node *)hc;
	mock_node *b = (mock_node *)hb;
	st->insert_before++;
	if (c->parent)
		mock_detach(c);
	c->parent = p;
	mock_node **pp = &p->first_child;
	while (*pp && *pp != b)
		pp = &(*pp)->next_sibling;
	c->next_sibling = *pp;
	*pp = c;
}

static void mh_remove_child(void *ctx, void *hp, void *hc)
{
	(void)hp;
	mock_stats *st = (mock_stats *)ctx;
	st->remove_child++;
	mock_detach((mock_node *)hc);
}

static void mh_destroy_node(void *ctx, void *hn)
{
	mock_stats *st = (mock_stats *)ctx;
	st->destroy_node++;
	mock_destroy_rec(st, (mock_node *)hn);
}

static void mh_set_event(
        void *ctx, void *hn, const char *ev, int bubbles,
        bud_event_handler_fn fn, void *ud)
{
	mock_stats *st = (mock_stats *)ctx;
	mock_node *n = (mock_node *)hn;
	st->set_event++;
	mock_ev **pp = &n->events;
	while (*pp) {
		if (strcmp((*pp)->name, ev) == 0) {
			if (!fn) {
				mock_ev *rm = *pp;
				*pp = rm->next;
				free(rm->name);
				free(rm);
			} else {
				(*pp)->bubbles = bubbles;
				(*pp)->fn = fn;
				(*pp)->ud = ud;
			}
			return;
		}
		pp = &(*pp)->next;
	}
	if (fn) {
		mock_ev *e = (mock_ev *)calloc(1, sizeof(*e));
		e->name = mstrdup(ev);
		e->bubbles = bubbles;
		e->fn = fn;
		e->ud = ud;
		e->next = n->events;
		n->events = e;
	}
}

static void mh_clear_children(void *ctx, void *hp)
{
	mock_stats *st = (mock_stats *)ctx;
	mock_node *p = (mock_node *)hp;
	st->clear_children++;
	mock_node *c = p->first_child;
	while (c) {
		mock_node *nx = c->next_sibling;
		mock_destroy_rec(st, c);
		c = nx;
	}
	p->first_child = NULL;
}

static const bud_host_ops MOCK_OPS = {
	.create_el      = mh_create_el,
	.create_text    = mh_create_text,
	.set_text       = mh_set_text,
	.set_attr       = mh_set_attr,
	.remove_attr    = mh_remove_attr,
	.insert_before  = mh_insert_before,
	.remove_child   = mh_remove_child,
	.destroy_node   = mh_destroy_node,
	.set_event      = mh_set_event,
	.clear_children = mh_clear_children,
};

static const char *mock_get_attr(const mock_node *n, const char *k)
{
	for (const mock_attr *a = n->attrs; a; a = a->next)
		if (strcmp(a->k, k) == 0)
			return a->v;
	return NULL;
}

static int mock_fire_click(mock_node *n)
{
	for (mock_ev *e = n->events; e; e = e->next) {
		if (strcmp(e->name, "click") == 0 && e->fn) {
			bud_event ev;
			memset(&ev, 0, sizeof(ev));
			ev.type = "click";
			ev.user = e->ud;
			ev.bubbles = e->bubbles;
			return e->fn(&ev);
		}
	}
	return -1;
}

/* ── Test 1: Independent dual stateful components + SSR parity ────────── */

typedef struct {
	const char *label;
} like_props_t;

static int g_like_renders = 0;

static int on_like_click(bud_event *ev)
{
	int *likes = (int *)ev->user;
	bud_set(likes, *likes + 1);
	return 0;
}

static bud_node *LikeButton(void *ctx, const void *raw_props)
{
	(void)ctx;
	const like_props_t *p = (const like_props_t *)raw_props;
	int *likes = bud_use_state(int, 0);
	g_like_renders++;

	bud_node *btn = bud_element("button");
	bud_set_attr(btn, "class", "like-btn");
	bud_bind_ctx(btn, "click", 1, on_like_click, likes, 0);
	bud_append(btn, bud_textf("%s:%d", p->label, *likes));
	return btn;
}

static void test_dual_stateful_components_and_ssr(void)
{
	mock_stats st = { 0 };
	mock_node root = { .tag = "root" };
	bud_mount *m = bud_mount_new(&MOCK_OPS, &st, &root);

	g_like_renders = 0;
	bud_node *tree = bud_element("div");
	bud_append(tree, bud_comp(LikeButton, like_props_t, .label = "A"));
	bud_append(tree, bud_comp(LikeButton, like_props_t, .label = "B"));

	/* Verify SSR works on the exact same reactive component tree! */
	char *html = bud_render_html(tree);
	assert(html != NULL);
	assert(strcmp(html, "<div><button class=\"like-btn\">A:0</button><button class=\"like-btn\">B:0</button></div>") == 0);
	free(html);

	g_like_renders = 0;
	assert(bud_mount_update(m, tree) == 0);
	bud_free(tree);
	assert(g_like_renders == 2);

	mock_node *div = root.first_child;
	assert(div && strcmp(div->tag, "div") == 0);
	mock_node *btn_a = div->first_child;
	mock_node *btn_b = btn_a->next_sibling;
	assert(btn_a && btn_b);
	assert(strcmp(btn_a->first_child->text, "A:0") == 0);
	assert(strcmp(btn_b->first_child->text, "B:0") == 0);

	/* Click ONLY button A -> only LikeButton(A) re-renders, 1 set_text, 0 create_el! */
	mock_reset_counts(&st);
	g_like_renders = 0;
	assert(mock_fire_click(btn_a) == 0);
	assert(bud_mount_is_dirty(m) == 1);
	assert(bud_mount_flush(m) == 1);
	assert(g_like_renders == 1);
	assert(st.create_el == 0);
	assert(st.create_text == 0);
	assert(st.set_text == 1);
	assert(st.set_event == 0); /* same likes ptr -> 0 rebind churn */
	assert(strcmp(btn_a->first_child->text, "A:1") == 0);
	assert(strcmp(btn_b->first_child->text, "B:0") == 0);

	/* Click button B twice */
	assert(mock_fire_click(btn_b) == 0);
	assert(mock_fire_click(btn_b) == 0);
	assert(bud_mount_flush(m) == 1);
	assert(strcmp(btn_a->first_child->text, "A:1") == 0);
	assert(strcmp(btn_b->first_child->text, "B:2") == 0);

	/* Equality bailout: setting same value does not mark dirty */
	int *likes_a = (int *)btn_a->events->ud;
	assert(bud_set(likes_a, 1) == 0);
	assert(bud_mount_is_dirty(m) == 0);

	bud_mount_free(m);
	assert(st.live_nodes == 0);
	printf("  test_dual_stateful_components_and_ssr: PASS\n");
}

/* ── Test 2: Keyed LIS swap (100 items -> exactly 2 moves) + O(1) clear ─ */

static bud_node *make_keyed_list(const int *ids, size_t n)
{
	bud_node *ul = bud_element("ul");
	for (size_t i = 0; i < n; i++) {
		char kbuf[32];
		snprintf(kbuf, sizeof(kbuf), "k%d", ids[i]);
		bud_node *li = bud_element("li");
		bud_set_key(li, kbuf);
		bud_append(li, bud_child_any(ids[i]));
		bud_append(ul, li);
	}
	return ul;
}

static void test_keyed_lis_swap_and_bulk_clear(void)
{
	mock_stats st = { 0 };
	mock_node root = { .tag = "root" };
	bud_mount *m = bud_mount_new(&MOCK_OPS, &st, &root);

	int ids[100];
	for (int i = 0; i < 100; i++)
		ids[i] = i + 1;

	bud_node *t1 = make_keyed_list(ids, 100);
	assert(bud_mount_update(m, t1) == 0);
	bud_free(t1);

	mock_node *ul = root.first_child;
	mock_node *row1 = ul->first_child->next_sibling; /* index 1 -> id 2 */
	mock_node *cur = ul->first_child;
	for (int i = 0; i < 98; i++)
		cur = cur->next_sibling;
	mock_node *row98 = cur; /* index 98 -> id 99 */

	/* Swap index 1 <-> index 98 */
	int tmp = ids[1];
	ids[1] = ids[98];
	ids[98] = tmp;

	mock_reset_counts(&st);
	bud_node *t2 = make_keyed_list(ids, 100);
	assert(bud_mount_update(m, t2) == 0);
	bud_free(t2);

	/* Prove exact LIS optimality: 0 create, 0 destroy, 0 text, EXACTLY 2 moves! */
	assert(st.create_el == 0);
	assert(st.create_text == 0);
	assert(st.destroy_node == 0);
	assert(st.set_text == 0);
	assert(st.insert_before == 2);

	/* Verify pointer identity & exact resulting DOM order */
	cur = ul->first_child;
	for (int i = 0; i < 100; i++) {
		char exp[32];
		snprintf(exp, sizeof(exp), "%d", ids[i]);
		assert(strcmp(cur->first_child->text, exp) == 0);
		if (i == 1)
			assert(cur == row98);
		if (i == 98)
			assert(cur == row1);
		cur = cur->next_sibling;
	}
	assert(cur == NULL);

	/* Clear all 100 rows -> O(1) clear_children = 1, remove_child = 0! */
	mock_reset_counts(&st);
	bud_node *t3 = make_keyed_list(ids, 0);
	assert(bud_mount_update(m, t3) == 0);
	bud_free(t3);
	assert(st.clear_children == 1);
	assert(st.remove_child == 0);
	assert(ul->first_child == NULL);

	bud_mount_free(m);
	assert(st.live_nodes == 0);
	printf("  test_keyed_lis_swap_and_bulk_clear: PASS\n");
}

/* ── Test 3: use_effect lifecycle, cleanups & HAS_CLEANUP gating ──────── */

static int g_eff_runs = 0;
static int g_eff_cleanups = 0;

static void sample_cleanup(void *ud)
{
	(void)ud;
	g_eff_cleanups++;
}

static bud_effect_cleanup_fn sample_effect(void *ud)
{
	(void)ud;
	g_eff_runs++;
	return sample_cleanup;
}

typedef struct {
	int dep;
} eff_props_t;

static bud_node *EffectComp(void *ctx, const void *raw_props)
{
	(void)ctx;
	const eff_props_t *p = (const eff_props_t *)raw_props;
	int *ref = bud_use_ref(int, 7);
	(*ref)++;
	bud_use_effect(sample_effect, NULL, &p->dep, sizeof(p->dep));
	bud_node *span = bud_element("span");
	bud_append(span, bud_child_any(p->dep));
	return span;
}

static void test_effects_and_cleanup_gating(void)
{
	mock_stats st = { 0 };
	mock_node root = { .tag = "root" };
	bud_mount *m = bud_mount_new(&MOCK_OPS, &st, &root);

	g_eff_runs = 0;
	g_eff_cleanups = 0;

	bud_node *t1 = bud_element("section");
	bud_append(t1, bud_comp(EffectComp, eff_props_t, .dep = 1));
	assert(bud_mount_update(m, t1) == 0);
	bud_free(t1);
	assert(g_eff_runs == 1);
	assert(g_eff_cleanups == 0);

	/* Re-render with SAME dep -> effect must NOT re-run */
	bud_node *t2 = bud_element("section");
	bud_append(t2, bud_comp(EffectComp, eff_props_t, .dep = 1));
	assert(bud_mount_update(m, t2) == 0);
	bud_free(t2);
	assert(g_eff_runs == 1);
	assert(g_eff_cleanups == 0);

	/* Re-render with NEW dep -> cleanup runs, then effect runs */
	bud_node *t3 = bud_element("section");
	bud_append(t3, bud_comp(EffectComp, eff_props_t, .dep = 2));
	assert(bud_mount_update(m, t3) == 0);
	bud_free(t3);
	assert(g_eff_runs == 2);
	assert(g_eff_cleanups == 1);

	/* Clear parent while child has cleanup -> MUST NOT use O(1) clear_children;
	 * MUST run unmount cleanup! */
	mock_reset_counts(&st);
	bud_node *t4 = bud_element("section");
	assert(bud_mount_update(m, t4) == 0);
	bud_free(t4);
	assert(st.clear_children == 0);
	assert(g_eff_cleanups == 2);

	bud_mount_free(m);
	assert(st.live_nodes == 0);
	printf("  test_effects_and_cleanup_gating: PASS\n");
}

/* ── Test 4: Conditional NULL <-> Element + Fragment right-sibling anchor */

typedef struct {
	int show;
} cond_props_t;

static bud_node *CondBanner(void *ctx, const void *raw_props)
{
	(void)ctx;
	const cond_props_t *p = (const cond_props_t *)raw_props;
	if (!p->show)
		return NULL;
	bud_node *frag = bud_fragment();
	bud_node *h = bud_element("h1");
	bud_append(h, bud_text("Banner"));
	bud_node *p2 = bud_element("p");
	bud_append(p2, bud_text("Sub"));
	bud_append(frag, h);
	bud_append(frag, p2);
	return frag;
}

static void test_null_to_fragment_sibling_anchor(void)
{
	mock_stats st = { 0 };
	mock_node root = { .tag = "root" };
	bud_mount *m = bud_mount_new(&MOCK_OPS, &st, &root);

	/* Render 1: CondBanner(false) followed by <footer>End</footer> */
	bud_node *t1 = bud_element("main");
	bud_append(t1, bud_comp(CondBanner, cond_props_t, .show = 0));
	bud_node *f1 = bud_element("footer");
	bud_append(f1, bud_text("End"));
	bud_append(t1, f1);
	assert(bud_mount_update(m, t1) == 0);
	bud_free(t1);

	mock_node *main_el = root.first_child;
	assert(main_el->first_child && strcmp(main_el->first_child->tag, "footer") == 0);
	mock_node *footer_ptr = main_el->first_child;

	/* Render 2: CondBanner(true) -> <h1> and <p> MUST land BEFORE <footer>! */
	bud_node *t2 = bud_element("main");
	bud_append(t2, bud_comp(CondBanner, cond_props_t, .show = 1));
	bud_node *f2 = bud_element("footer");
	bud_append(f2, bud_text("End"));
	bud_append(t2, f2);
	assert(bud_mount_update(m, t2) == 0);
	bud_free(t2);

	mock_node *c0 = main_el->first_child;
	mock_node *c1 = c0 ? c0->next_sibling : NULL;
	mock_node *c2 = c1 ? c1->next_sibling : NULL;
	assert(c0 && strcmp(c0->tag, "h1") == 0);
	assert(c1 && strcmp(c1->tag, "p") == 0);
	assert(c2 == footer_ptr);
	assert(c2->next_sibling == NULL);

	bud_mount_free(m);
	assert(st.live_nodes == 0);
	printf("  test_null_to_fragment_sibling_anchor: PASS\n");
}

/* ── Test 5: Inline-copied closure payload via bud_bind_ctx + attr diff ─ */

typedef struct {
	int *out_target;
	int  song_id;
} del_cap_t;

static int on_delete_song(bud_event *ev)
{
	del_cap_t *c = (del_cap_t *)ev->user;
	*c->out_target = c->song_id;
	return 0;
}

static void test_bind_ctx_inline_closure_and_attrs(void)
{
	mock_stats st = { 0 };
	mock_node root = { .tag = "root" };
	bud_mount *m = bud_mount_new(&MOCK_OPS, &st, &root);

	int deleted_id = 0;

	/* Build with stack-local del_cap_t, then FREE the vnode immediately! */
	bud_node *b1 = bud_element("button");
	bud_set_attr(b1, "class", "btn");
	bud_set_attr(b1, "title", "Delete 42");
	del_cap_t cap1 = { .out_target = &deleted_id, .song_id = 42 };
	bud_bind_ctx(b1, "click", 1, on_delete_song, &cap1, sizeof(cap1));
	assert(bud_mount_update(m, b1) == 0);
	bud_free(b1);

	/* Fire event after vnode was freed — owned copy in bud_cell must still work! */
	mock_node *btn = root.first_child;
	assert(mock_fire_click(btn) == 0);
	assert(deleted_id == 42);
	assert(strcmp(mock_get_attr(btn, "title"), "Delete 42") == 0);

	/* Update with identical capture bytes + removed title attr -> 0 set_event, 1 remove_attr */
	mock_reset_counts(&st);
	bud_node *b2 = bud_element("button");
	bud_set_attr(b2, "class", "btn");
	bud_bind_ctx(b2, "click", 1, on_delete_song, &cap1, sizeof(cap1));
	assert(bud_mount_update(m, b2) == 0);
	bud_free(b2);
	assert(st.set_event == 0);
	assert(st.set_attr == 0);
	assert(st.remove_attr == 1);
	assert(mock_get_attr(btn, "title") == NULL);

	/* Update capture song_id -> 99 -> 1 set_event, fires 99 */
	mock_reset_counts(&st);
	bud_node *b3 = bud_element("button");
	bud_set_attr(b3, "class", "btn");
	del_cap_t cap2 = { .out_target = &deleted_id, .song_id = 99 };
	bud_bind_ctx(b3, "click", 1, on_delete_song, &cap2, sizeof(cap2));
	assert(bud_mount_update(m, b3) == 0);
	bud_free(b3);
	assert(st.set_event == 1);
	assert(mock_fire_click(btn) == 0);
	assert(deleted_id == 99);

	bud_mount_free(m);
	assert(st.live_nodes == 0);
	printf("  test_bind_ctx_inline_closure_and_attrs: PASS\n");
}

int main(void)
{
	printf("bud_reconcile_test:\n");
	test_dual_stateful_components_and_ssr();
	test_keyed_lis_swap_and_bulk_clear();
	test_effects_and_cleanup_gating();
	test_null_to_fragment_sibling_anchor();
	test_bind_ctx_inline_closure_and_attrs();
	printf("bud_reconcile_test: ALL TESTS PASSED\n");
	return 0;
}

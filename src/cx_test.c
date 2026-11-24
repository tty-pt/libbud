/**
 * @file cx_test.c
 * @brief Unit tests for the `.cx` to C11 transpiler (`cx_transpile.c`).
 */

#include <bud/cx.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void assert_spans_in_bounds(const char *src, const cx_result *res)
{
	size_t slen = strlen(src);
	for (size_t i = 0; i < res->span_count; i++) {
		const cx_span *sp = &res->spans[i];
		assert(sp->cx_off <= slen);
		assert(sp->cx_off + sp->cx_len <= slen);
		assert(sp->c_off <= res->c_len);
		assert(sp->c_off + sp->c_len <= res->c_len);
	}
}

static void test_plain_c_preserved(void)
{
	const char *src =
		"#include <stdio.h>\n"
		"#include <bud/bud_reconcile.h>\n"
		"static int cmp(int a, int b) {\n"
		"    const char *s = \"<div>not jsx</div>\";\n"
		"    /* <span>comment</span> */\n"
		"    // <p>line comment</p>\n"
		"    int x = (1 << 4);\n"
		"    for (int i = 0; i < b; i++) {\n"
		"        if (a < b && a <= b && a != b) x += i;\n"
		"    }\n"
		"    return (a < b) ? x : (int)s[0];\n"
		"}\n";

	cx_options opt = { .filename = "plain.cx", .emit_lines = 0, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);
	assert(res.diag_count == 0);
	assert(strcmp(res.c_code, src) == 0);
	assert_spans_in_bounds(src, &res);
	cx_result_free(&res);
	printf("  test_plain_c_preserved: PASS\n");
}

static void test_elements_fragments_attrs_text(void)
{
	const char *src =
		"static bud_node *view(const char *cls, int count, int disabled) {\n"
		"    return (\n"
		"        <>\n"
		"            <div class={cls} data-count={count} key=\"main\">\n"
		"                {/* JSX comment ignored */}\n"
		"                <span>Count: {count}</span>\n"
		"                <button disabled>Go</button>\n"
		"                <input type=\"text\" />\n"
		"            </div>\n"
		"        </>\n"
		"    );\n"
		"}\n";

	cx_options opt = { .filename = "view.cx", .emit_lines = 1, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);
	assert(strstr(res.c_code, "bud_fragment()") != NULL);
	assert(strstr(res.c_code, "bud_element(\"div\")") != NULL);
	assert(strstr(res.c_code, "bud_set_attr_any(") != NULL);
	assert(strstr(res.c_code, "bud_set_key(") != NULL);
	assert(strstr(res.c_code, "bud_text(\"Count: \")") != NULL);
	assert(strstr(res.c_code, "bud_child_any((count))") != NULL);
	assert(strstr(res.c_code, "bud_set_attr(") != NULL);
	assert(strstr(res.c_code, "\"disabled\", \"\"") != NULL);
	assert(strstr(res.c_code, "bud_element(\"input\")") != NULL);
	assert(strstr(res.c_code, "JSX comment ignored") == NULL);
	assert(strstr(res.c_code, "#line ") != NULL);
	assert_spans_in_bounds(src, &res);
	cx_result_free(&res);
	printf("  test_elements_fragments_attrs_text: PASS\n");
}

static void test_component_and_children(void)
{
	const char *src =
		"typedef struct { int v; } Ctx;\n"
		"component Card(const char *title, bud_node *children);\n"
		"static component Badge(const char *label, void (*cb)(int)) {\n"
		"    return <span class=\"badge\">{label}</span>;\n"
		"}\n"
		"static component Empty(void) {\n"
		"    return <hr />;\n"
		"}\n"
		"static bud_node *render_root(void) {\n"
		"    return <Card key={42} title=\"Songs\"><Badge label=\"New\" cb={NULL} /><Empty /></Card>;\n"
		"}\n";

	cx_options opt = { .filename = "comp.cx", .emit_lines = 0, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);

	/* Ensure typedef Ctx precedes hoisted Card_props */
	const char *p_ctx = strstr(res.c_code, "typedef struct { int v; } Ctx;");
	const char *p_card_props = strstr(res.c_code, "typedef struct Card_props");
	const char *p_badge_props = strstr(res.c_code, "typedef struct Badge_props");
	const char *p_badge_fn = strstr(res.c_code, "static bud_node *Badge_render(");
	assert(p_ctx && p_card_props && p_badge_props && p_badge_fn);
	assert(p_ctx < p_card_props);
	assert(p_badge_props < p_badge_fn);
	assert(strstr(res.c_code, "void (*cb)(int);") != NULL);
	assert(strstr(res.c_code, "void (*cb)(int) = __cx_p->cb;") != NULL);
	assert(strstr(res.c_code, "bud_component_vnode(Card_render, NULL, &(Card_props){ .title = \"Songs\", .children = ") != NULL);
	assert(strstr(res.c_code, "bud_set_key_any(") != NULL);
	assert(strstr(res.c_code, "bud_component_vnode(Empty_render, NULL, &(Empty_props){ 0 }, sizeof(Empty_props))") != NULL);
	assert_spans_in_bounds(src, &res);
	cx_result_free(&res);
	printf("  test_component_and_children: PASS\n");
}

static void test_closures_and_shorthand_captures(void)
{
	const char *src =
		"typedef struct { int deleted; } App;\n"
		"static component Row(App *app, const char *title, int idx) {\n"
		"    int *likes = bud_use_state(int, 0);\n"
		"    return (\n"
		"        <div class=\"row\">\n"
		"            <button onClick={[int *likes](bud_event *e) { bud_set(likes, *likes + 1); }}>{*likes}</button>\n"
		"            <button onClick={[app, int row_id = idx]() { app->deleted = row_id; }}>Del</button>\n"
		"            <button onClick={[]() { }}>Noop</button>\n"
		"        </div>\n"
		"    );\n"
		"}\n";

	cx_options opt = { .filename = "row.cx", .emit_lines = 0, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);

	/* Hoisted closures must appear AFTER typedef App and BEFORE Row_render */
	const char *p_app = strstr(res.c_code, "typedef struct { int deleted; } App;");
	const char *p_cap1 = strstr(res.c_code, "typedef struct __cx_cap_1");
	const char *p_cap2 = strstr(res.c_code, "typedef struct __cx_cap_2");
	const char *p_fn3 = strstr(res.c_code, "static int __cx_fn_3(");
	const char *p_row = strstr(res.c_code, "static bud_node *Row_render(");
	assert(p_app && p_cap1 && p_cap2 && p_fn3 && p_row);
	assert(p_app < p_cap1 && p_cap1 < p_cap2 && p_cap2 < p_fn3 && p_fn3 < p_row);

	/* Shorthand `app` resolved to `App *app` from component Row params */
	assert(strstr(p_cap2, "App *app;") != NULL);
	assert(strstr(p_cap2, "int row_id;") != NULL);
	assert(strstr(res.c_code, ".app = (app), .row_id = (idx)") != NULL);
	assert(strstr(res.c_code, "__cx_fn_3, NULL, 0)") != NULL);
	assert_spans_in_bounds(src, &res);
	cx_result_free(&res);
	printf("  test_closures_and_shorthand_captures: PASS\n");
}

static void test_loops_and_conditionals(void)
{
	const char *src =
		"static bud_node *list_view(const char **items, size_t n, int show_empty) {\n"
		"    return (\n"
		"        <ul>\n"
		"            {for (size_t i = 0; i < n; i++) <li key={items[i]}>{items[i]}</li>}\n"
		"            {if (n == 0 && show_empty) <li class=\"empty\">None</li> else <li class=\"done\">End</li>}\n"
		"            {for (size_t j = 0; j < n; j++) {\n"
		"                const char *it = items[j];\n"
		"                <span>{it}</span>;\n"
		"            }}\n"
		"        </ul>\n"
		"    );\n"
		"}\n";

	cx_options opt = { .filename = "list.cx", .emit_lines = 0, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);
	assert(strstr(res.c_code, "for (size_t i = 0; i < n; i++)") != NULL);
	assert(strstr(res.c_code, "if (n == 0 && show_empty)") != NULL);
	assert(strstr(res.c_code, "else") != NULL);
	assert(strstr(res.c_code, "const char *it = items[j];") != NULL);
	assert_spans_in_bounds(src, &res);
	cx_result_free(&res);
	printf("  test_loops_and_conditionals: PASS\n");
}

static void test_span_mapping_roundtrip(void)
{
	const char *src =
		"static component Demo(const char *title) {\n"
		"    return <section class=\"hero\"><h1>{title}</h1></section>;\n"
		"}\n";

	cx_options opt = { .filename = "demo.cx", .emit_lines = 1, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);
	assert_spans_in_bounds(src, &res);

	/* Map `{title}` expression in .cx -> .c and back */
	const char *expr_ptr = strstr(src, "{title}");
	assert(expr_ptr != NULL);
	uint32_t title_cx_off = (uint32_t)((expr_ptr + 1) - src); /* 't' of title */

	uint32_t c_off = 0, back_cx = 0;
	uint8_t kind = 0, back_kind = 0;
	assert(cx_map_cx_to_c(&res, title_cx_off, &c_off, &kind) == 1);
	assert(kind == CX_SPAN_EXPR);
	assert(memcmp(res.c_code + c_off, "title", 5) == 0);

	assert(cx_map_c_to_cx(&res, c_off, &back_cx, &back_kind) == 1);
	assert(back_cx == title_cx_off);
	assert(back_kind == CX_SPAN_EXPR);

	/* Map `<section` tag span */
	const char *tag_ptr = strstr(src, "section");
	uint32_t tag_off = (uint32_t)(tag_ptr - src);
	assert(cx_map_cx_to_c(&res, tag_off, &c_off, &kind) == 1);
	assert(kind == CX_SPAN_TAG);
	assert(memcmp(res.c_code + c_off, "section", 7) == 0);

	cx_result_free(&res);
	printf("  test_span_mapping_roundtrip: PASS\n");
}

static void test_diagnostics_and_recovery(void)
{
	/* 1. Mismatched closing tag */
	const char *bad_tag = "static bud_node *f(void) { return <div></span>; }\n";
	cx_options opt = { .filename = "bad.cx", .emit_lines = 0, .recover = 0 };
	cx_result res;
	assert(cx_transpile(&opt, bad_tag, strlen(bad_tag), &res) == 0);
	assert(res.ok == 0);
	assert(res.diag_count >= 1);
	assert(strstr(res.diags[0].msg, "mismatched closing tag") != NULL);
	cx_result_free(&res);

	/* 2. Unknown shorthand capture */
	const char *bad_cap =
		"static bud_node *g(void) { int x = 1; return <button onClick={[x]() {}}>Hi</button>; }\n";
	assert(cx_transpile(&opt, bad_cap, strlen(bad_cap), &res) == 0);
	assert(res.ok == 0);
	assert(res.diag_count >= 1);
	assert(strstr(res.diags[0].msg, "requires an explicit type") != NULL);
	cx_result_free(&res);

	/* 3. Incomplete edit recovery for cx-lsp */
	const char *partial =
		"static bud_node *h(struct Foo *foo) {\n"
		"    return <div class={foo.";
	cx_options ropt = { .filename = "edit.cx", .emit_lines = 0, .recover = 1 };
	assert(cx_transpile(&ropt, partial, strlen(partial), &res) == 0);
	assert(res.ok == 0);
	assert(strstr(res.c_code, "foo.") != NULL);
	assert_spans_in_bounds(partial, &res);

	const char *foo_ptr = strstr(partial, "foo.");
	uint32_t c_off = 0;
	uint8_t kind = 0;
	assert(cx_map_cx_to_c(&res, (uint32_t)(foo_ptr - partial), &c_off, &kind) == 1);
	assert(memcmp(res.c_code + c_off, "foo", 3) == 0);
	cx_result_free(&res);

	printf("  test_diagnostics_and_recovery: PASS\n");
}

static int slice_eq(const char *src, cx_slice sl, const char *lit)
{
	size_t n = strlen(lit);
	return sl.len == n && memcmp(src + sl.off, lit, n) == 0;
}

static void test_metadata_index(void)
{
	const char *src =
		"#include <bud/bud_reconcile.h>\n"
		"#include \"button.cxh\"\n"
		"/** A clickable badge component. */\n"
		"component Badge(const char *label, int count) {\n"
		"\treturn <div class=\"badge\">\n"
		"\t\t<button onClick={[label](bud_event *e) { (void)e; }}>\n"
		"\t\t\t{for (int i = 0; i < count; i++) <span key={i}>{label}</span>}\n"
		"\t\t\t{for (int j = 0; j < count; j++) <em>{j}</em>}\n"
		"\t\t</button>\n"
		"\t</div>;\n"
		"}\n"
		"static component Helper(void);\n";

	cx_options opt = { .filename = "idx.cx", .emit_lines = 0, .recover = 0, .build_index = 1 };
	cx_result res;
	assert(cx_transpile(&opt, src, strlen(src), &res) == 0);
	assert(res.ok == 1);
	assert_spans_in_bounds(src, &res);

	assert(res.include_count == 2);
	assert(res.includes[0].is_system == 1);
	assert(slice_eq(src, res.includes[0].path, "bud/bud_reconcile.h"));
	assert(res.includes[1].is_system == 0);
	assert(slice_eq(src, res.includes[1].path, "button.cxh"));

	assert(res.comp_count == 2);
	assert(slice_eq(src, res.comps[0].name, "Badge"));
	assert(res.comps[0].is_static == 0);
	assert(res.comps[0].is_forward == 0);
	assert(res.comps[0].doc.len > 0);
	assert(strstr(src + res.comps[0].doc.off, "clickable badge") != NULL);
	assert(res.comps[0].body.len > 0);
	assert(src[res.comps[0].body.off] == '{');
	assert(src[res.comps[0].body.off + res.comps[0].body.len - 1] == '}');
	assert(res.comps[0].param_count == 2);
	assert(slice_eq(src, res.comp_params[res.comps[0].param_start + 0].name, "label"));
	assert(slice_eq(src, res.comp_params[res.comps[0].param_start + 0].decl, "const char *label"));
	assert(slice_eq(src, res.comp_params[res.comps[0].param_start + 1].name, "count"));

	assert(slice_eq(src, res.comps[1].name, "Helper"));
	assert(res.comps[1].is_static == 1);
	assert(res.comps[1].is_forward == 1);
	assert(res.comps[1].body.len == 0);

	assert(res.elem_count == 4);
	assert(slice_eq(src, res.elems[0].tag, "div"));
	assert(slice_eq(src, res.elems[0].close_tag, "div"));
	assert(res.elems[0].parent == -1);
	assert(res.elems[0].comp_idx == 0);
	assert(res.elems[0].attr_count == 1);
	assert(slice_eq(src, res.attrs[res.elems[0].attr_start].name, "class"));
	assert(slice_eq(src, res.attrs[res.elems[0].attr_start].val, "badge"));
	assert(res.attrs[res.elems[0].attr_start].val_kind == CX_ATTR_STRING);

	assert(slice_eq(src, res.elems[1].tag, "button"));
	assert(res.elems[1].parent == 0);
	assert(res.elems[1].attr_count == 1);
	assert(res.attrs[res.elems[1].attr_start].val_kind == CX_ATTR_CLOSURE);

	assert(slice_eq(src, res.elems[2].tag, "span"));
	assert(res.elems[2].in_loop_direct == 1);
	assert(res.elems[2].has_key_attr == 1);

	assert(slice_eq(src, res.elems[3].tag, "em"));
	assert(res.elems[3].in_loop_direct == 1);
	assert(res.elems[3].has_key_attr == 0);

	assert(res.closure_count == 1);
	assert(slice_eq(src, res.closures[0].captures, "label"));
	assert(slice_eq(src, res.closures[0].params, "bud_event *e"));
	assert(res.closures[0].comp_idx == 0);

	const char *hdr = "/tmp/cx_test_emit_hdr.h";
	assert(cx_write_header(&res, src, hdr) == 0);
	FILE *fp = fopen(hdr, "rb");
	assert(fp != NULL);
	char hbuf[1024];
	size_t hn = fread(hbuf, 1, sizeof(hbuf) - 1, fp);
	fclose(fp);
	hbuf[hn] = '\0';
	remove(hdr);
	assert(strstr(hbuf, "typedef struct Badge_props") != NULL);
	assert(strstr(hbuf, "bud_node *Badge_render(") != NULL);
	assert(strstr(hbuf, "Helper_props") == NULL);

	cx_result_free(&res);
	printf("  test_metadata_index: PASS\n");
}

int main(void)
{
	printf("cx_test:\n");
	test_plain_c_preserved();
	test_elements_fragments_attrs_text();
	test_component_and_children();
	test_closures_and_shorthand_captures();
	test_loops_and_conditionals();
	test_span_mapping_roundtrip();
	test_diagnostics_and_recovery();
	test_metadata_index();
	printf("cx_test: ALL TESTS PASSED\n");
	return 0;
}

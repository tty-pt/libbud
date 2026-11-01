#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bud/bud.h>

/* Patch Collector context */
typedef struct {
    napi_env env;
    napi_value arr;
    uint32_t count;
} PatchContext;

static int CollectPatch(void *user, const char *op, const char *a, const char *b, const char *c) {
    PatchContext *ctx = (PatchContext *)user;
    napi_value item;
    napi_create_object(ctx->env, &item);

    napi_value op_v, a_v, b_v, c_v;
    napi_create_string_utf8(ctx->env, op ? op : "", NAPI_AUTO_LENGTH, &op_v);
    napi_create_string_utf8(ctx->env, a ? a : "", NAPI_AUTO_LENGTH, &a_v);
    napi_create_string_utf8(ctx->env, b ? b : "", NAPI_AUTO_LENGTH, &b_v);
    napi_create_string_utf8(ctx->env, c ? c : "", NAPI_AUTO_LENGTH, &c_v);

    napi_set_named_property(ctx->env, item, "op", op_v);
    napi_set_named_property(ctx->env, item, "type", op_v);
    napi_set_named_property(ctx->env, item, "a", a_v);
    napi_set_named_property(ctx->env, item, "b", b_v);
    napi_set_named_property(ctx->env, item, "c", c_v);

    napi_set_element(ctx->env, ctx->arr, ctx->count++, item);
    return 0;
}

/* Recursive helper to convert JS BudNode object to C bud_node */
static bud_node *BuildCNode(napi_env env, napi_value js_node) {
    napi_valuetype vt;
    napi_typeof(env, js_node, &vt);
    if (vt == napi_string) {
        size_t len = 0;
        napi_get_value_string_utf8(env, js_node, NULL, 0, &len);
        char *txt = (char *)malloc(len + 1);
        napi_get_value_string_utf8(env, js_node, txt, len + 1, &len);
        bud_node *t = bud_text(txt);
        free(txt);
        return t;
    }
    if (vt != napi_object) return NULL;

    napi_value tag_val;
    napi_get_named_property(env, js_node, "tag", &tag_val);
    char tag_buf[64] = "div";
    size_t tag_len = 0;
    napi_get_value_string_utf8(env, tag_val, tag_buf, sizeof(tag_buf) - 1, &tag_len);

    bud_node *root = bud_element(tag_buf);

    /* Attributes */
    napi_value attrs_val;
    napi_get_named_property(env, js_node, "attrs", &attrs_val);
    napi_typeof(env, attrs_val, &vt);
    if (vt == napi_object) {
        napi_value prop_names;
        napi_get_property_names(env, attrs_val, &prop_names);
        uint32_t num_props = 0;
        napi_get_array_length(env, prop_names, &num_props);
        for (uint32_t i = 0; i < num_props; i++) {
            napi_value key_v, val_v;
            napi_get_element(env, prop_names, i, &key_v);
            napi_get_property(env, attrs_val, key_v, &val_v);

            char key_str[128] = {0};
            char val_str[1024] = {0};
            size_t klen = 0, vlen = 0;
            napi_get_value_string_utf8(env, key_v, key_str, sizeof(key_str) - 1, &klen);
            napi_get_value_string_utf8(env, val_v, val_str, sizeof(val_str) - 1, &vlen);
            bud_set_attr(root, key_str, val_str);
        }
    }

    /* Text */
    napi_value text_val;
    napi_get_named_property(env, js_node, "text", &text_val);
    napi_typeof(env, text_val, &vt);
    if (vt == napi_string) {
        size_t tlen = 0;
        napi_get_value_string_utf8(env, text_val, NULL, 0, &tlen);
        if (tlen > 0) {
            char *txt = (char *)malloc(tlen + 1);
            napi_get_value_string_utf8(env, text_val, txt, tlen + 1, &tlen);
            bud_append(root, bud_text(txt));
            free(txt);
        }
    }

    /* Children */
    napi_value children_val;
    napi_get_named_property(env, js_node, "children", &children_val);
    bool is_arr = false;
    napi_is_array(env, children_val, &is_arr);
    if (is_arr) {
        uint32_t num_children = 0;
        napi_get_array_length(env, children_val, &num_children);
        for (uint32_t i = 0; i < num_children; i++) {
            napi_value child_v;
            napi_get_element(env, children_val, i, &child_v);
            bud_node *child_c = BuildCNode(env, child_v);
            if (child_c) {
                bud_append(root, child_c);
            }
        }
    }

    return root;
}

/* diffVdom(oldTreeObj, newTreeObj) -> Array<Patch> */
static napi_value BudDiffVdom(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    PatchContext ctx;
    ctx.env = env;
    ctx.count = 0;
    napi_create_array(env, &ctx.arr);

    if (argc < 2) return ctx.arr;

    bud_node *old_root = BuildCNode(env, args[0]);
    bud_node *new_root = BuildCNode(env, args[1]);

    if (old_root && new_root) {
        bud_vdom_diff(old_root, new_root, CollectPatch, &ctx);
    }

    if (old_root) bud_free(old_root);
    if (new_root) bud_free(new_root);

    return ctx.arr;
}

static napi_value BudVersion(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value res;
    napi_create_string_utf8(env, "0.1.0", NAPI_AUTO_LENGTH, &res);
    return res;
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"diffVdom", NULL, BudDiffVdom, NULL, NULL, NULL, napi_default, NULL},
        {"version", NULL, BudVersion, NULL, NULL, NULL, napi_default, NULL}
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <bud/bud.h>
#include <ttypt/iso.h>

__attribute__((import_module("env"), import_name("bud_host_emit_patch")))
extern void bud_host_emit_patch(
    const char *op, size_t op_len,
    const char *a, size_t a_len,
    const char *b, size_t b_len,
    const char *c, size_t c_len);

static int wasm_emit_patch(void *user, const char *op, const char *a, const char *b, const char *c) {
    (void)user;
    bud_host_emit_patch(
        op, op ? strlen(op) : 0,
        a, a ? strlen(a) : 0,
        b, b ? strlen(b) : 0,
        c, c ? strlen(c) : 0
    );
    return 0;
}

ISO_EXPORT(int, bud_wasm_diff, bud_node *old_root, bud_node *new_root) {
    return bud_vdom_diff(old_root, new_root, wasm_emit_patch, NULL);
}

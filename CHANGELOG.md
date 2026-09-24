## 1.0.0

- **First stable release** — a small C DOM scaffold for building isomorphic user interfaces: renders HTML on the server (SSR) and, through a WASM bridge, hydrates and enhances the same DOM in the browser from the **same C source**. One renderer, both sides.
- **Node tree**: fragments, elements, text, and raw HTML, with attribute storage, class-name helpers, and child/attribute/listener accessors.
- **HTML templates**: `bud_tpl(fmt, ...)` / `bud_vtpl(fmt, ap)` parse an HTML fragment string into a node tree (tags, attributes, void elements, comments, whitespace-collapsed text) — `lx_tpl` wraps it in JSX; `bud_raw_set_text` swaps raw HTML on an existing node.
- **JSX convenience macros** (`bud_jsx.h`): `lx_el`/`lx_n`, `lx_frag`/`lx_frag_n`, `lx_text`/`lx_textf`, `lx_attr`, `lx_on`/`lx_bind`, `lx_none`, `lx_hidden`/`lx_hidden_int`, `lx_submit`, `lx_link`, `lx_raw` (with `BUD_DEBUG` source tracking via `bud_sprint_tree`).
- **SSR serialization**: `bud_render_html`, plus `bud_render_hydrated_html` with hydration markers for DOM reattachment.
- **Patch stream**: `bud_render_patch_ops`, `bud_render_ops`, `bud_vdom_diff`, `bud_hydrate` for browser-side DOM apply over the WASM bridge.
- **Runtime**: `bud_runtime_new`/`_free`/`_root`/`_set_invalidate`/`_mark_dirty`/`_is_dirty`/`_flush`/`_mount`/`_update`/`_unmount`/`_dispatch`, with lifecycle hooks (on-mount/on-update/on-unmount) and event dispatch — bubbling with stop-propagation and a universal action dispatcher.
- **Components**: light render-function registry — `bud_component`/`bud_component_fn`/`bud_component_render` — so sibling nodes share a single instance/update path.
- **JSON field extraction**: vendored `jsmn.h`, no `json-c` — `bud_json_str`/`_int`/`_data`/`_array_for_each`.
- **Table-driven UI state**: pure 5-field binder `bud_field_desc_t` (`key, offset, size, is_int, kind`) via `bud_state_apply(_len/_stride/_stride_len)` — zero database or storage concepts.
- **WASM host bridge** (`bud_app.h`): `bud_app_render`, `BUD_APP_ROUTE`, `bud_host_emit_patch`, `bud_patch_attr`/`bud_patch_text`, `bud_app_set_state`, `wasm_get_runtime`, plus `bud_host_*` function pointers in `bud.h`.
- **Zero dependencies**: links `libc` only — the only vendored code is `jsmn.h`.

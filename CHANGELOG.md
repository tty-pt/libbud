## [Unreleased] (target: v1.3.3)

### Added
- **Host-Agnostic Reactive Reconciler (`include/bud/bud_reconcile.h`, `src/bud_reconcile.c`, `src/bud_reconcile_test.c`)**:
  - Added a pluggable `bud_host_ops` reactive reconciler (`bud_mount_new()`, `bud_mount_update()`, `bud_mount_flush()`, `bud_mount_unmount()`, `bud_mount_free()`) with hooks (`bud_use_state_raw()`, `bud_set_state_raw()`, `bud_use_ref_raw()`, `bud_use_effect()`) and typed text/attribute/key builders.
  - Added `bud_component_vnode()`, `bud_expand_tree()` component ID parity, and target-scoped event binding (`bud_bind_ctx()`, `bud_bind_ctx_lit()`).
- **Browser DOM Host Adapter & Event Propagation (`include/bud/bud_dom_host.h`, `src/bud_dom_host.c`, `htdocs/bud-cap.js`, `npm/dist/bud-cap.js`)**:
  - Added `bud_dom_host_ops()`, `BUD_DOM_ROOT_HANDLE`, `bud_dom_dispatch()`, `bud_dom_event_payload()`, `bud_dom_set_event_payload()`, and `bud_dom_host_reset()` bridging `bud_reconcile` onto browser DOM nodes through `bud-cap.js`.
  - Upstreamed `{ passive: false }` wheel listener and `button`/`buttons` payload propagation in `bud-cap.js`.
- **Chunked `bud_arena` Bump Allocator & Zero-Copy `*_lit` APIs (`include/bud/bud.h`, `src/bud_internal.h`, `src/libbud.c`, `src/bud_test.c`)**:
  - Added chunked bump-allocator arenas (`bud_arena_new()`, `bud_arena_push()`, `bud_arena_pop()`, `bud_arena_current()`, `bud_arena_reset()`, `bud_arena_free()`) and zero-copy string-literal node/attribute constructors (`bud_element_lit()`, `bud_text_lit()`, `bud_set_attr_lit()`, `bud_set_attr_name_lit()`, `bud_set_bool_attr_lit()`, `bud_bind_ctx_lit()`).
- **`.cx` Transpiler, `cx-lsp` Language Server & `tree-sitter-cx` Grammar (`include/bud/cx.h`, `src/cx*.c`, `tree-sitter-cx/`)**:
  - Added the `.cx` JSX-in-C transpiler CLI (`bin/cx`) with `on*Self` (`bubbles=0`) and `on:<literal>[:self]` custom event support, source map JSON output (`cx_write_map_json`), and header emission (`cx_write_header`).
  - Added `cx-lsp` (`bin/cx-lsp`, `src/cx-lsp.c`, `src/cx_lsp_index.c`, `src/cx_lsp_features.c`) with flat metadata index, cross-file resolution, native UX, and code actions (`src/cx_lsp_test.c`, `src/cx_test.c`).
  - Added `tree-sitter-cx/` (`grammar.js`, `queries/highlights.scm`, `test/corpus/cx.txt`, `tree-sitter.json`) syntax highlighting and grammar corpus.
- **JavaScript / TypeScript Automatic JSX Runtime (`@tty-pt/bud/jsx-runtime`, `npm/jsx-runtime/`, `npm/dist/node.js`, `npm/examples/jsx-app/`)**:
  - Added `npm/jsx-runtime/{index.js,jsx-runtime.js,reconciler.js,*.d.ts}` providing `jsx`, `jsxs`, `jsxDEV`, `Fragment`, React-style hooks (`useState`, `useReducer`, `useRef`, `useMemo`, `useCallback`, `useEffect`, `memo`, `shallowEqual`), and DOM reconciliation (`render`, `flushSync`).
  - Extracted `npm/dist/node.js`, added `npm/examples/jsx-app/` + `npm/examples/{demo.mjs,index.html}`, and added 7 test and benchmark suites (`jsx-runtime.test.mjs`, `hooks.test.mjs`, `events.test.mjs`, `reconciler.test.mjs`, `dom-stub.mjs`, `bench-vs-react.mjs`, `bench-browser.mjs`).

### Changed
- **Modular Source Decomposition (`src/bud_internal.h`, `src/bud_json.c`, `src/bud_render.c`, `src/bud_runtime.c`, `src/bud_tpl.c`, `src/libbud.c`)**:
  - Split monolithic `src/libbud.c` into focused compilation units (`bud_tpl.c`, `bud_render.c`, `bud_runtime.c`, `bud_json.c`, `bud_reconcile.c`, `bud_dom_host.c`) sharing `src/bud_internal.h`.
- **Removed Legacy `bud-hydrate.js` (`htdocs/bud-cap.js`, `npm/dist/bud-cap.js`, `include/bud/bud_app.h`)**:
  - Removed deleted legacy `htdocs/bud-hydrate.js` and `npm/dist/bud-hydrate.js`, consolidating browser hydration and DOM host capabilities into `htdocs/bud-cap.js` and `npm/dist/bud-cap.js`.

### Fixed
- **Recursive DOM Fallback Dispatch & Default `localStorage` Seeding (`htdocs/bud-cap.js`)**:
  - Guarded against recursive DOM fallback event dispatch and seeded default `localStorage` settings.
- **Inactive Input `.value` Property Sync (`htdocs/bud-cap.js`, `npm/dist/bud-cap.js`)**:
  - Updated attribute patch replay (`op: "attr"`, `name === "value"`) to synchronize the live DOM `.value` property on `<input>` elements when `document.activeElement !== el`, ensuring programmatic state updates and resets reflect immediately in the UI without clobbering active user typing focus.

## [1.3.2] - 2026-11-16

### Fixed
- **Windows DLL Exports (`src/libbud.c`)**: Guarded `__attribute__((weak))` under `#ifndef _WIN32` on `bud_patch_attr`, `bud_patch_text`, and `bud_app_set_state` so MinGW-w64 PE/COFF exports them into `libbud.dll.a` for downstream consumers (`libhyle-bud`).

## [1.3.1] - 2026-11-16

### Packaging
- Upgraded `@tty-pt/stele` `peerDependencies` (`>=1.7.0`) and `devDependencies` (`^1.7.0`), and rebuilt `bud.wasm` against the single-allocator `--wrap` shim.

## [1.3.0] - 2026-11-13

### Packaging
- Requires `@tty-pt/stele` >=1.6.2 (`^1.6.2`).
- The npm tarball is built once by CI, with a self-contained native addon per platform next to `bud.wasm`, and published as verified.

### Added
- **Client Hydration Engine & Capability Bridge (`htdocs/bud-hydrate.js`, `htdocs/bud-cap.js`, `npm/dist/bud-hydrate.js`, `npm/dist/bud-cap.js`)**:
  - Implemented full client-side HTML hydration runtime with fine-grained DOM attachment and Web HAL capability registration (`bud-cap.js`).
  - Enhanced `include/bud/bud_state.h` state management and serialization helpers.
  - Batched capability bridge remounting via microtask scheduling (`queueMicrotask` / `setTimeout`) across patch replay boundaries to prevent bridge thrashing.

### Changed
- Refactored `src/libbud.c` VDOM diffing and tree traversal operations.

## [1.2.2] - 2026-11-03

- **Decoupled Pure UI Tree & VDOM Diff Engine**:
  - Restored `external/libbud` to a pure, framework-neutral UI tree builder and VDOM differ without runtime slot management, browser execution assumptions, or host patch emissions.
  - Purified `<bud/bud_app.h>`: eliminated `#if defined(__wasm__)` preprocessor branching, host patch emission imports, and runtime slot mounting declarations.
  - Removed `src/bud_wasm_app.c` from the library build (`libbud.so`, `Makefile`, and `objects-set.mk`).
  - Added pure C implementations of `bud_patch_attr`, `bud_patch_text`, and `bud_patch_innerhtml` directly in `libbud.c`.
- **Standalone `@tty-pt/bud` Package & Pure WebAssembly Differ**:
  - Published `@tty-pt/bud` on npm with zero external dependencies and strict `peerDependencies` (`@tty-pt/stele: ">=1.5.0"`).
  - Bundled standalone `bud.wasm` compiled directly from `src/libbud.c` and `wasm/bud_wasm.c`.
  - Migrated build to centralized `make wasm` rule in `external/mk/include.mk` with `-ffreestanding -flto -O3` and post-link `wasm-opt -O3` optimization (reducing binary size from 167 KB to 138 KB).
  - Uses `@tty-pt/stele`'s unified memory substrate for string marshalling and patch emissions.

## [1.1.0]

- **Namespaced WASM Imports & Capability Boundary**:
  - Standardized namespaced `bud.*` host imports for DOM patching, runtime invalidation, and state synchronization.
  - Hardened WebAssembly guest/host capability allowlist (`wasm-allowed-imports.lst`).
- **Universal UI Slot Engine & Keyed Hydration (`<bud/bud_slot.h>`, `<bud/bud_state.h>`)**:
  - Introduced `BUD_SLOT_DEFINE` and `BUD_SLOT_CONTRIBUTE` macros for multi-tenant and decoupled UI slot registration across SSR and WASM.
  - Added declarative `BUD_DEFINE_STATE` schema generator and `bud_state_hydrate()` for zero-copy binary state synchronization and DOM reattachment.
  - Centralized global slot registry in `libbud.c`.
- **Web HAL Continuous Frame Loop Contract (`wasm_tick`)**:
  - Exported `wasm_tick(float dt, int width, int height)` in `src/bud_wasm_app.c` enabling 60fps hardware-accelerated rendering loops without DOM thrashing.

## [1.0.0]

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

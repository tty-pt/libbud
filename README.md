<p align="center">
  <img src="assets/logo.png" alt="bud logo" width="220" />
</p>

# libbud

[![npm](https://img.shields.io/badge/npm-@tty--pt/bud-CB3837?logo=npm)](npm/README.md)
[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![No dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)](#)
[![Isomorphic SSR+WASM](https://img.shields.io/badge/UI-SSR%20%2B%20WASM-4B8BBE)](#)

> One renderer for native SSR and browser WASM. An isomorphic, zero-dependency C DOM scaffold and reactive state binder with JSX ergonomics. Canonical header is `<bud/bud.h>`.

---

## 1. Quickstart: Declarative UI

Build and render a declarative HTML component in pure C99:

```c
#include <stdio.h>
#include <bud/bud.h>
#include <bud/bud_jsx.h>

int main(void) {
    // Declarative JSX-style macro tree in pure C
    bud_node *doc = lx_frag_n(
        lx_el("h1", lx_text("Pure C UI")),
        lx_el("p", lx_attr("class", "lead"), lx_text("One renderer for SSR and browser WASM."))
    );

    // Serialize to clean, standards-compliant HTML string
    char *html = bud_render_html(doc);
    printf("%s\n", html);

    bud_free_string(html);
    bud_free(doc);
    return 0;
}
```

Compile and run:
```sh
cc ui_demo.c $(pkg-config --cflags --libs bud) -o ui_demo
./ui_demo
```

### Dual-Target Compilation Pipeline
The exact same C file compiles to a native `.so` module for server SSR and a browser `.wasm` module for hydration:

```sh
# 1. Native compile for Server-Side Rendering (SSR):
cc -shared -fPIC -O2 -o mod.so mod.c $(pkg-config --cflags --libs bud)

# 2. WebAssembly compile for browser hydration:
clang --target=wasm32-wasi -O2 -nostdlib \
  -Wl,--no-entry -Wl,--export=bud_app_render \
  -I/usr/include/bud -o htdocs/mod.wasm mod.c -lbud
```

---

## 2. The 5-Field State Binding Contract

`libbud` uses a declarative 5-field descriptor (`bud_field_desc_t`) to unpack JSON state into C memory without manual parsing:

```c
static const bud_field_desc_t state_fields[] = {
    { "query",   BUD_TYPE_STR, offsetof(state_t, query),   sizeof(((state_t*)0)->query), "" },
    { "page",    BUD_TYPE_INT, offsetof(state_t, page),    sizeof(int), "1" },
    { "active",  BUD_TYPE_BOOL, offsetof(state_t, active), sizeof(int), "0" },
};
```

| Field | Type | Description |
| :--- | :--- | :--- |
| `name` | `const char *` | JSON key name |
| `type` | `bud_type_t` | Type identifier (`BUD_TYPE_STR`, `BUD_TYPE_INT`, `BUD_TYPE_BOOL`) |
| `offset` | `size_t` | Byte offset in C state struct (`offsetof(state_t, member)`) |
| `size` | `size_t` | Buffer capacity (strings) or scalar byte size |
| `def_val` | `const char *` | Fallback default value if absent from JSON |

---

## 3. Core API & Subsystems

| Header / Tool | Key APIs / Features | Description |
| :--- | :--- | :--- |
| `<bud/bud.h>` | `bud_element_lit`, `bud_text_lit`, `bud_render_html`, `bud_vdom_diff`, `bud_expand_tree`, `bud_arena_*` | Core VDOM tree, SSR HTML serializer, VDOM differ, chunked bump-allocator arenas, and zero-copy `*_lit` builders |
| `<bud/bud_jsx.h>` | `lx_el`, `lx_n`, `lx_frag`, `lx_text`, `lx_attr`, `lx_on` | Declarative JSX-style C99 macro builders |
| `<bud/bud_reconcile.h>` | `bud_host_ops`, `bud_mount_new`, `bud_mount_update`, `bud_mount_flush`, `bud_use_state_raw`, `bud_use_ref_raw`, `bud_use_effect` | Host-agnostic reactive reconciler and hooks usable with browser DOM, `liblamina`, or custom hosts |
| `<bud/bud_dom_host.h>` | `bud_dom_host_ops`, `BUD_DOM_ROOT_HANDLE`, `bud_dom_dispatch`, `bud_dom_event_payload` | Browser DOM `bud_host_ops` adapter backed by `htdocs/bud-cap.js` |
| `<bud/bud_state.h>` / `<bud/bud_app.h>` | `bud_state_apply`, `bud_app_set_state` | 5-field table-driven state binder and isomorphic app hooks |
| `<bud/cx.h>`, `bin/cx`, `bin/cx-lsp` | `cx_transpile`, `cx_write_map_json`, `cx_write_header`, `on*Self`, `on:<literal>[:self]` | `.cx` JSX-in-C transpiler, `cx-lsp` language server (flat metadata index, cross-file resolution, code actions), and `tree-sitter-cx/` grammar |

👉 *Source is organized into focused compilation units (`src/libbud.c`, `src/bud_tpl.c`, `src/bud_render.c`, `src/bud_runtime.c`, `src/bud_json.c`, `src/bud_reconcile.c`, `src/bud_dom_host.c`).*

---

## 4. Isomorphic Architecture & Browser Capability Bridge

1. **Server-Side Render (SSR)**: The server executes `bud_app_render(state)`, serializing the AST to plain HTML strings. State is injected into `<script type="application/json" id="bud-state">` alongside `#bud-root`.
2. **Instant Paint**: The browser renders clean HTML immediately with zero layout shift—even if JavaScript or WASM is disabled.
3. **WASM Capability Bridge (`htdocs/bud-cap.js`)**: Loads alongside `@tty-pt/stele`, providing DOM host operations (`bud_dom_host_ops`), event payload propagation, `{ passive: false }` wheel listeners, and inactive `<input>.value` patch synchronization.

---

## 5. Installation & Build

### Prebuilt Packages
Prebuilt packages for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD are distributed via [tty.pt packages](https://github.com/tty-pt/ci/blob/main/docs/install.md). Use **libbud** as the package name.

### Build from Source
```sh
make                  # builds lib/libbud.so, bin/cx, bin/cx-lsp, and unit test binaries
make test             # runs bud_test, bud_reconcile_test, cx_test, and cx_lsp_test
make npm-test         # builds and runs @tty-pt/bud + JSX runtime tests
sudo make install     # installs headers, libbud, bin/cx, bin/cx-lsp, and bud.pc
```

---

## 6. JavaScript, TypeScript & JSX Bindings (@tty-pt/bud)

Official Node.js and Browser WebAssembly bindings are colocated in [`npm/`](npm/README.md):
- **Universal VDOM & Diffing (`@tty-pt/bud`)**: Virtual DOM builder (`h`), compiled C/WASM diffing (`diffVdom`), and `npm/dist/bud-cap.js` browser capability module (`@tty-pt/bud/cap`).
- **Automatic JSX Runtime (`@tty-pt/bud/jsx-runtime`)**: React-compatible `jsx`/`jsxs`/`Fragment`, hooks (`useState`, `useReducer`, `useRef`, `useMemo`, `useCallback`, `useEffect`, `memo`), and reconciler (`render`, `flushSync`), with a working TypeScript example in [`npm/examples/jsx-app/`](npm/examples/jsx-app/package.json).
- **C Development Headers**: Install via `npm install @tty-pt/bud` and consume headers from `node_modules/@tty-pt/bud/include`.

👉 *For complete TypeScript usage, JSX runtime setup, and API reference, see [npm/README.md](npm/README.md).*

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `LICENSE`.

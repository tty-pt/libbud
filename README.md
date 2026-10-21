<p align="center">
  <img src="assets/logo.png" alt="bud logo" width="220" />
</p>

# libbud

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![No dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)](#)
[![Isomorphic SSR+WASM](https://img.shields.io/badge/UI-SSR%20%2B%20WASM-4B8BBE)](#)

> One renderer for native SSR and browser WASM. An isomorphic, zero-dependency C DOM scaffold and reactive state binder with JSX ergonomics.

`libbud` provides an isomorphic UI model for C: the exact same component function renders static, SEO-friendly HTML on the server and runs client-side inside WebAssembly to hydrate event listeners and apply minimal DOM patch streams.

---

## Contents

- [Isomorphic Architecture & Hydration](#isomorphic-architecture--hydration)
- [The 5-Field State Binding Contract](#the-5-field-state-binding-contract)
- [Dual-Target Compilation Pipeline](#dual-target-compilation-pipeline)
- [Quickstart: Declarative UI](#quickstart-declarative-ui)
- [Core API Reference](#core-api-reference)
- [Design Philosophy: Truly Zero Dependencies](#design-philosophy-truly-zero-dependencies)
- [Installation & Build](#installation--build)
- [License](#license)

---

## Isomorphic Architecture & Hydration

1. **Server-Side Render (SSR)**: The server executes `bud_app_render(state)`, serializing the AST to plain HTML strings. State is injected into a `<script type="application/json" id="bud-state">` tag alongside `#bud-root`.
2. **Instant Paint**: The browser renders clean HTML immediately with zero layout shift—even if JavaScript or WASM is disabled.
3. **WASM Hydration**: When `htdocs/*.wasm` loads, `bud_hydrate()` reads `#bud-state`, unpacks the JSON into C structs, and attaches event listeners to existing DOM nodes without re-rendering the tree.

---

## The 5-Field State Binding Contract

`libbud` uses a declarative 5-field descriptor (`bud_field_desc_t`) to unpack JSON state into C memory without manual parsing:

| Field | Type | Description |
| :--- | :--- | :--- |
| `name` | `const char *` | JSON key name |
| `type` | `bud_type_t` | Type identifier (`BUD_TYPE_STR`, `BUD_TYPE_INT`, `BUD_TYPE_BOOL`) |
| `offset` | `size_t` | Byte offset in C state struct (`offsetof(state_t, member)`) |
| `size` | `size_t` | Buffer capacity (strings) or scalar byte size |
| `def_val` | `const char *` | Fallback default value if absent from JSON |

```c
static const bud_field_desc_t state_fields[] = {
    { "query",   BUD_TYPE_STR, offsetof(state_t, query),   sizeof(((state_t*)0)->query), "" },
    { "page",    BUD_TYPE_INT, offsetof(state_t, page),    sizeof(int), "1" },
    { "active",  BUD_TYPE_BOOL, offsetof(state_t, active), sizeof(int), "0" },
};
```

---

## Dual-Target Compilation Pipeline

The exact same C file compiles to a native `.so` module and a browser `.wasm` binary:

```sh
# 1. Native compile for Server-Side Rendering (SSR):
cc -shared -fPIC -O2 -o mod.so mod.c $(pkg-config --cflags --libs bud)

# 2. WASI/WebAssembly compile for browser hydration:
clang --target=wasm32-wasi -O2 -nostdlib \
  -Wl,--no-entry -Wl,--export=bud_app_render \
  -I/usr/include/bud -o htdocs/mod.wasm mod.c -lbud
```

---

## Quickstart: Declarative UI

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

Compile and link:
```sh
cc ui_demo.c $(pkg-config --cflags --libs bud)
```

---

## Core API Reference

| Function / Macro | Signature | Description |
| :--- | :--- | :--- |
| `lx_el` / `lx_n` | `lx_el(tag, ...)` | Declarative element node constructor |
| `lx_text` / `lx_attr` | `lx_text(str)` / `lx_attr(k, v)` | Text and attribute builders |
| `bud_render_html` | `char *bud_render_html(node)` | Server-side plain HTML string serializer |
| `bud_render_patch_ops`| `void bud_render_patch_ops(...)` | Emit browser DOM patch stream over WASM |
| `bud_state_apply` | `int bud_state_apply(state, json, fields, n)` | 5-field table-driven UI state binder |
| `bud_hydrate` | `void bud_hydrate(root, state)` | Reattach browser event listeners to SSR'd HTML |

👉 *For lifecycle hooks (`on-mount`/`on-unmount`), VDOM diffing, and WASM host bridge details, see `include/bud/bud.h`.*

---

## Design Philosophy: Truly Zero Dependencies

* **Libc Only**: `bud` links strictly to `libc` (`ldd libbud.so` → `libc.so.6`). No Node, no npm, no React runtime, and no heavy JSON library (vendors a tiny, embedded `jsmn.h` tokenizer).
* **Single Source of Truth**: The exact same C source code generates the initial SSR HTML payload on the server and runs client-side inside the browser WASM engine.
* **Progressive Enhancement Contract**: Plain HTML renders immediately on the client even if WebAssembly or JavaScript is completely disabled.

---

## Installation & Build

### Prebuilt Packages
Prebuilt packages are distributed on tty.pt for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD. Follow the [installation instructions](https://github.com/tty-pt/ci/blob/main/docs/install.md) and use **libbud** as the package name.

### Build from Source
```sh
make                  # builds lib/libbud.so + bin/bud_test
make test             # run in-tree unit and serialization tests
sudo make install     # install headers and library to system
```

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `LICENSE`.

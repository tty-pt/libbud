<p align="center">
  <img src="assets/logo.png" alt="bud logo" width="220" />
</p>

# libbud

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![No dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)](#)
[![Isomorphic SSR+WASM](https://img.shields.io/badge/UI-SSR%20%2B%20WASM-4B8BBE)](#)

> One renderer for native SSR and browser WASM. An isomorphic, zero-dependency C DOM scaffold and reactive state binder with JSX ergonomics.

---

## Isomorphic Architecture

`libbud` provides the user interface layer for native servers and the browser:
- **Dual-Target Compilation**: Compiles natively as a `.so` for server-side HTML rendering (SSR), and to WebAssembly (`.wasm`) for progressive browser hydration from the same C source.
- **Data Schema Binding**: Bridges to [libhyle](https://github.com/tty-pt/hyle) via `libhyle-bud` to bind schema fields directly to UI forms, tables, and filters.
- **Web Host**: Static `.wasm` bundles and server endpoints are served by [axil](https://github.com/tty-pt/axil).

---

## Quickstart

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

## Core API (The Essential 20%)

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

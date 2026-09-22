# bud

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![No dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)](#)

A small C DOM scaffold for building isomorphic user interfaces: it renders HTML
on the server (SSR) and, through a WASM bridge, hydrates and enhances the
same DOM in the browser — from the **same C source**. One renderer, both sides.

---

## Contents

- [Features](#features)
- [Zero dependencies](#zero-dependencies)
- [Build & install](#build--install)
- [Quickstart](#quickstart)
- [JSX convenience macros](#jsx-convenience-macros)
- [API overview](#api-overview)
- [Isomorphic SSR](#isomorphic-ssr)
- [Testing](#testing)
- [License](#license)

## Features

- **Node tree** for fragments, elements, text, and raw HTML
- **Attribute storage** and class-name helpers
- **Child, attribute, and listener accessors** for host-side hydration
- **SSR serialization** — `bud_render_html`, plus hydrated markers for DOM reattachment
- **Patch stream** — browser-side DOM apply via `bud_render_patch_ops`
- **Runtime wrapper** — dirty/flush state management and mount/update/unmount lifecycle hooks
- **Event dispatch** — bubbling with stop-propagation, and a universal action dispatcher
- **JSON field extraction** — vendored `jsmn.h`, no `json-c` dependency
- **Table-driven UI state** — a pure 5-field binder (`bud_field_desc_t`: key, offset, size, is_int, kind) with zero database or storage concepts
- **WASM host bridge** — patch emission and event forwarding for the browser bundle

## Zero dependencies

bud is pure C and links **libc only** (`ldd libbud.so` → `libc.so.6`). It brings
in no framework, no database, no storage layer, and no third-party library — the
only vendored code is the self-contained `jsmn.h` JSON tokenizer.

## Build & install

The library builds with a plain `make` (repo's shared `mk/include.mk`):

```sh
cd external/libbud
make          # lib/libbud.so + bin/bud_test
make test     # run the in-tree self-test suite

sudo make install   # lib, headers, and bud.pc → $(PREFIX), default /usr/local
```

Link it from your own C code:

```sh
cc my_app.c $(pkg-config --cflags --libs bud)
```

## Quickstart

```c
#include <bud/bud.h>
#include <bud/bud_jsx.h>
#include <stdio.h>

int main(void)
{
	/* Build a small tree with the JSX-style lx_* macros. */
	bud_node *doc = lx_frag_n(
	    lx_el("h1", lx_text("Hello, bud!")),
	    lx_el("p", lx_attr("class", "lead"), lx_text("SSR-first.")));

	/* Serialize to plain HTML. */
	char *html = bud_render_html(doc);
	printf("%s\n", html);

	bud_free_string(html);
	bud_free(doc);
	return 0;
}
```

`lx_el`, `lx_frag`, and friends live in `include/bud/bud_jsx.h` and build
`bud_node` trees without hand-writing the node API. For a tree with no children,
pass `lx_none()` — C99 `__VA_ARGS__` cannot be empty.

## JSX convenience macros

`include/bud/bud_jsx.h` maps a JSX-like syntax onto the tree API:

| Macro | Meaning |
|-------|---------|
| `lx_el(tag, ...)` / `lx_n(tag, ...)` | element node (statement / expression form) |
| `lx_frag(...)` / `lx_frag_n(...)` | fragment node |
| `lx_text(str)` / `lx_textf(fmt, ...)` | text node |
| `lx_attr(key, value)` / `lx_attr(key, fmt, ...)` | attribute argument |
| `lx_on(event, bubbles)` / `lx_bind(event, bubbles, handler)` | event listener / bound listener |
| `lx_none()` | empty-argument placeholder |
| `lx_hidden(name, val)` / `lx_hidden_int(...)` | hidden form input |
| `lx_submit(label, cls)` / `lx_link(href, text, cls)` | form controls |
| `lx_raw(html)` (with `BUD_DEBUG`) | raw HTML with source tracking |

Defining `BUD_DEBUG` makes every `lx_*` creation capture `__FILE__`/`__LINE__`
on the node; dump it with `bud_sprint_tree(root, buf, size)`.

## API overview

Full signatures live in `include/bud/bud.h`.

**Tree construction** — `bud_fragment`, `bud_element`, `bud_text`, `bud_textf`,
`bud_hidden_input(_int)`, `bud_submit_btn`, `bud_link`, `bud_raw`, `bud_tpl`,
`bud_vtpl`, `bud_component_render`.

**Attributes & events** — `bud_set_attr(_fmt)`, `bud_set_bool_attr`,
`bud_get_attr`, `bud_add_class` / `bud_remove_class` / `bud_toggle_class`,
`bud_detach`, `bud_append`, `bud_on` / `bud_bind`, `bud_api_action_handler`,
`bud_set_lifecycle` (on-mount / on-update / on-unmount).

**Introspection** — `bud_node_kind_of`, `bud_node_tag`, `bud_node_text`,
`bud_node_id`, `bud_node_child_count` / `bud_node_child`, `bud_node_parent`,
`bud_node_attr_count` / `bud_node_attr_name` / `bud_node_attr_value`,
`bud_node_listener_count` / `bud_node_listener_event` / `bud_node_listener_bubbles`.

**Rendering** — `bud_render_html`, `bud_render_hydrated_html`,
`bud_render_ops`, `bud_render_hydration_ops`, `bud_render_patch_ops`,
`bud_vdom_diff`, `bud_hydrate`.

**Runtime** — `bud_runtime_new` / `bud_runtime_free` / `bud_runtime_root`,
`bud_runtime_set_invalidate`, `bud_runtime_mark_dirty` / `bud_runtime_is_dirty`,
`bud_runtime_flush`, `bud_runtime_mount` / `bud_runtime_update` /
`bud_runtime_unmount`, `bud_runtime_dispatch`, `bud_event_stop_propagation` /
`bud_event_prevent_default`.

**JSON helpers** (jsmn-backed, no `json-c`) — `bud_json_str(_len)`,
`bud_json_int(_len)`, `bud_json_data(_len)`, `bud_json_array_for_each(_len)`,
`bud_json_array_for_each_key_len`.

**Table-driven UI state** — `bud_state_apply(_len/_stride/_stride_len)`,
`bud_state_apply_array(_len/_stride_len)` with `bud_field_desc_t`. A single
field definition drives `wasm_init`/`wasm_set_*` and UI hydration; the binder
knows only key/offset/size/is_int/kind.

**Debug** — `bud_sprint_tree`, `bud_node_set_src` / `bud_node_get_src`.

**Memory** — `bud_free_string` for rendered strings, `bud_free` for node trees.

**WASM host bridge** (`include/bud/bud_app.h`) — `bud_app_render` /
`BUD_APP_ROUTE`, `bud_host_emit_patch`, `bud_patch_attr` / `bud_patch_text`,
`bud_app_set_state`, `wasm_get_runtime`, plus the `bud_host_*` function
pointers in `bud.h`.

## Isomorphic SSR

bud is a single component framework: it renders HTML natively for SSR and, via
its WASM bridge driven by `htdocs/bud-client.js` / `bud-hydrate.js`, hydrates
and enhances the SSR'd DOM in the browser — the same C source on both sides.
It is neutral and self-contained (pure C, no framework/database/storage
dependencies); other frameworks can implement the same SSR contract with their
own client runtime.

See `../../docs/C-ISOMORPHIC-BUD.md`, `../../docs/WASM-BRIDGE.md`, and
`../../docs/SSR-CONTRACT.md` for the full contract.

## Testing

```sh
make test       # LD_LIBRARY_PATH=./lib ./bin/bud_test
```

From the repository root, `make boundary-check` runs the module-layer gates,
and `make test` runs the full platform suite.

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `../../LICENSE`.
<p align="center">
  <img src="https://raw.githubusercontent.com/tty-pt/libbud/main/assets/logo.png" alt="@tty-pt/bud logo" width="220" />
</p>

# @tty-pt/bud

[![npm version](https://img.shields.io/badge/npm-v1.3.2-CB3837?logo=npm)](#)
[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![TypeScript Ready](https://img.shields.io/badge/TypeScript-Ready-3178C6?logo=typescript)](#)
[![Node.js Native](https://img.shields.io/badge/Node.js-N--API-339933?logo=node.js)](#)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Reactor-654FF0?logo=webassembly)](#)
[![Zero Dependencies](https://img.shields.io/badge/dependencies-0-success)](#)

> Universal Node.js and Browser bindings for **`libbud`**, the pure-C declarative 5-field UI binder, virtual DOM diffing engine, host-agnostic reactive reconciler, and automatic JSX runtime. Canonical C headers: `<bud/bud.h>`, `<bud/bud_jsx.h>`, `<bud/bud_app.h>`, `<bud/bud_reconcile.h>`, `<bud/bud_dom_host.h>`, and `<bud/cx.h>`.

📖 **Full C & UI Engine Documentation**: See the main [**libbud GitHub README**](https://github.com/tty-pt/libbud#readme) for C JSX macro builders, `bud_arena` bump allocators, `bud_dom_host`, the `.cx` transpiler / `cx-lsp` / `tree-sitter-cx` toolchain, SSR HTML rendering, and VDOM diffing internals.

---

## 1. Quickstart & Installation

```bash
npm install @tty-pt/bud
```

**Native OS Packages (`tty.pt`)**: Prebuilt native C packages (`libbud`) for Linux (`apt`, `apk`, `pacman`, `dnf`), macOS (`brew`), Windows (`winget`, MSYS2), and OpenBSD (`pkg_add`) are available via the [**tty.pt package repositories**](https://github.com/tty-pt/ci/blob/main/docs/install.md).

```typescript
import { h, diffVdom } from "@tty-pt/bud";

// 1. Build virtual DOM node tree
const vdom = h("div", { class: "card" }, [
  h("h2", {}, ["Member Card"]),
  h("p", {}, ["Status: Active"])
]);

console.log(vdom.render());
// <div class="card"><h2>Member Card</h2><p>Status: Active</p></div>

// 2. VDOM Diffing
const updatedVdom = h("div", { class: "card" }, [
  h("h2", {}, ["Member Card"]),
  h("p", {}, ["Status: Suspended"])
]);

const patches = diffVdom(vdom, updatedVdom);
console.log(patches);
// [ { type: 'patch-text', op: 'patch-text', a: 'Status: Suspended', text: 'Status: Suspended' } ]
```

### Automatic JSX Runtime (`@tty-pt/bud/jsx-runtime`)

Configure `"jsx": "react-jsx"` and `"jsxImportSource": "@tty-pt/bud"` in `tsconfig.json` (see `examples/jsx-app/`):

```tsx
import { useState, render } from "@tty-pt/bud/jsx-runtime";

function Counter() {
  const [count, setCount] = useState(0);
  return <button onClick={() => setCount((c) => c + 1)}>Count: {count}</button>;
}

render(<Counter />, document.getElementById("root")!);
```

---

## 2. JavaScript / TypeScript API Reference

### `h(tag: string, attrs?: Record<string, string>, children?: Array<BudNode | string>): BudNode`
Declarative factory mirroring C `bud_node_create()` (implemented in `dist/node.js`).

### `BudNode`
Represents a virtual DOM node with chainable mutation methods:
* `.attr(key: string, value: string): this`
* `.child(node: BudNode | string): this`
* `.setText(text: string): this`
* `.render(): string` — Emits compliant isomorphic HTML markup.

### `diffVdom(oldTree: BudNode | null, newTree: BudNode | null): VdomPatch[]`
Calculates minimal DOM patch operations between two VDOM trees. **Executes strictly in compiled WebAssembly** (`bud_vdom_diff`) or native Node-API (`bud_node.node`), with zero duplicate JavaScript diffing algorithms.

Returns an array of `VdomPatch` objects:
```typescript
interface VdomPatch {
  type: "MOUNT" | "UNMOUNT" | "REPLACE" | "SET_TEXT" | "patch-text" | string;
  op: string;
  a?: string;
  b?: string;
  c?: string;
  text?: string;
  node?: BudNode;
}
```

### `@tty-pt/bud/jsx-runtime` (`jsx-runtime/jsx-runtime.js`, `jsx-runtime/reconciler.js`)
- Automatic JSX entry points: `jsx`, `jsxs`, `jsxDEV`, `Fragment`.
- Reactive hooks & memoization: `useState`, `useReducer`, `useRef`, `useMemo`, `useCallback`, `useEffect`, `memo`, `shallowEqual`, `setRootScheduler`.
- DOM reconciler: `render(vnode, container)` and `flushSync(fn)`.

### `getBudWasm(): WebAssembly.Instance`
Direct accessor returning the underlying compiled WebAssembly instance (`dist/bud.wasm`) loaded with `bud_vdom_diff`, `bud_element`, `bud_text`, `bud_arena_*`, and memory allocators.

### Browser Capability & DOM Host Module (`@tty-pt/bud/cap`, `dist/bud-cap.js`)
Provides Web HAL `bud.*` capability registration on `@tty-pt/stele`, `bud_dom_host` DOM node handle operations, event payload propagation, `{ passive: false }` wheel listeners, and inactive `<input>.value` patch synchronization.

### `version(): string`
Returns the library semantic version string.

---

## 3. C Development Headers via NPM

```makefile
CFLAGS += -I./node_modules/@tty-pt/bud/include
LDLIBS += -lbud
```

Includes `<bud/bud.h>` (`bud_arena_*`, `*_lit` zero-copy builders), `<bud/bud_jsx.h>`, `<bud/bud_app.h>`, `<bud/bud_slot.h>`, and `<bud/bud_state.h>` in `npm/include/bud/` (the full native C repository additionally ships `<bud/bud_reconcile.h>`, `<bud/bud_dom_host.h>`, `<bud/cx.h>`, the `.cx` transpiler, `cx-lsp`, and `tree-sitter-cx`).

---

## 4. Building & Testing from Source

```bash
make npm-build    # Compiles native addon and WASM reactor
make npm-test     # Runs VDOM, JSX runtime, hooks, events, and reconciler test suites
```

## License

BSD-2-Clause — Copyright (c) 2026, Paulo Quirino <quirinpa@gmail.com>. See [LICENSE](https://github.com/tty-pt/libbud/blob/main/LICENSE).

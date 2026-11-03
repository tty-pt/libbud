<p align="center">
  <img src="https://raw.githubusercontent.com/tty-pt/libbud/main/assets/logo.png" alt="@tty-pt/bud logo" width="220" />
</p>

# @tty-pt/bud

[![npm version](https://img.shields.io/badge/npm-v1.2.2-CB3837?logo=npm)](#)
[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![TypeScript Ready](https://img.shields.io/badge/TypeScript-Ready-3178C6?logo=typescript)](#)
[![Node.js Native](https://img.shields.io/badge/Node.js-N--API-339933?logo=node.js)](#)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Reactor-654FF0?logo=webassembly)](#)
[![Zero Dependencies](https://img.shields.io/badge/dependencies-0-success)](#)

> Universal Node.js and Browser bindings for **`libbud`**, the pure-C declarative 5-field UI binder and virtual DOM diffing engine.

---

## 1. Quickstart

```bash
npm install @tty-pt/bud
```

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

---

## 2. JavaScript / TypeScript API Reference

### `h(tag: string, attrs?: Record<string, string>, children?: Array<BudNode | string>): BudNode`
Declarative factory mirroring C `bud_node_create()`.

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

### `getBudWasm(): WebAssembly.Instance`
Direct accessor returning the underlying compiled WebAssembly instance (`dist/bud.wasm`) loaded with `bud_vdom_diff`, `bud_element`, `bud_text`, and memory allocators.

### `version(): string`
Returns the library semantic version string.

---

## 3. C Development Headers via NPM

```makefile
CFLAGS += -I./node_modules/@tty-pt/bud/include
LDLIBS += -lbud
```

---

## 3. Building from Source

```bash
make npm-build    # Compiles native addon and WASM reactor
make npm-test     # Runs verification test suite
```

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt.

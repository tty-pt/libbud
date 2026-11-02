<p align="center">
  <img src="https://raw.githubusercontent.com/tty-pt/libbud/main/assets/logo.png" alt="@tty-pt/bud logo" width="220" />
</p>

# @tty-pt/bud

[![npm version](https://img.shields.io/badge/npm-v0.1.0-CB3837?logo=npm)](#)
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
```

---

## 2. C Development Headers via NPM

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

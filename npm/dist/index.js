import { loadWasmModule, loadNativeAddon, getSubstrate } from "@tty-pt/stele";

const nativeBinding = loadNativeAddon(import.meta.url, "bud");
let wasmInstance = null;
let collectedPatches = [];

export function getBudWasm() {
  if (wasmInstance) return wasmInstance;
  wasmInstance = loadWasmModule(new URL("./bud.wasm", import.meta.url), {
    env: {
      bud_host_emit_patch: (opPtr, opLen, aPtr, aLen, bPtr, bLen, cPtr, cLen) => {
        const mem = wasmInstance.exports.memory.buffer;
        const dec = new TextDecoder();
        const op = dec.decode(new Uint8Array(mem, opPtr, opLen));
        const a = dec.decode(new Uint8Array(mem, aPtr, aLen));
        const b = dec.decode(new Uint8Array(mem, bPtr, bLen));
        const c = dec.decode(new Uint8Array(mem, cPtr, cLen));
        collectedPatches.push({ type: op, op, a, b, c, text: a });
      }
    }
  });
  return wasmInstance;
}

export class BudNode {
  constructor(tag, attrs = {}, children = []) {
    this.tag = tag;
    this.attrs = attrs;
    this.children = children;
    this.text = "";
  }

  attr(key, val) {
    this.attrs[key] = val;
    return this;
  }

  child(node) {
    this.children.push(node);
    return this;
  }

  setText(text) {
    this.text = text;
    return this;
  }

  render() {
    const attrStr = Object.entries(this.attrs)
      .map(([k, v]) => ` ${k}="${v}"`)
      .join("");
    const childStr = this.children.map((c) => (typeof c === "string" ? c : c.render())).join("");
    return `<${this.tag}${attrStr}>${this.text}${childStr}</${this.tag}>`;
  }
}

/**
 * Creates virtual DOM node (mirrors bud_node_create)
 */
export function h(tag, attrs = {}, children = []) {
  return new BudNode(tag, attrs, children);
}

function buildWasmTree(wasm, node) {
  if (!node) return 0;
  const { malloc, free, bud_element, bud_text, bud_set_attr, bud_append } = wasm.exports;
  const enc = new TextEncoder();

  function allocStr(s) {
    const bytes = enc.encode(s);
    const ptr = malloc(bytes.length + 1);
    new Uint8Array(wasm.exports.memory.buffer, ptr, bytes.length + 1).set([...bytes, 0]);
    return ptr;
  }

  if (typeof node === "string") {
    const sPtr = allocStr(node);
    const t = bud_text(sPtr);
    free(sPtr);
    return t;
  }

  const tagPtr = allocStr(node.tag || "div");
  const cNode = bud_element(tagPtr);
  free(tagPtr);

  if (node.attrs) {
    for (const [k, v] of Object.entries(node.attrs)) {
      const kPtr = allocStr(k);
      const vPtr = allocStr(String(v));
      bud_set_attr(cNode, kPtr, vPtr);
      free(kPtr);
      free(vPtr);
    }
  }

  if (node.text) {
    const tPtr = allocStr(node.text);
    const tNode = bud_text(tPtr);
    bud_append(cNode, tNode);
    free(tPtr);
  }

  if (Array.isArray(node.children)) {
    for (const ch of node.children) {
      const chNode = buildWasmTree(wasm, ch);
      if (chNode) bud_append(cNode, chNode);
    }
  }

  return cNode;
}

/**
 * Computes patch operations between two VDOM trees strictly using C bud_vdom_diff via WebAssembly
 */
export function diffVdom(oldTree, newTree, options = {}) {
  if (!options.wasm && nativeBinding && nativeBinding.diffVdom) {
    return nativeBinding.diffVdom(oldTree, newTree);
  }

  const wasm = getBudWasm();
  const { bud_wasm_diff, bud_free } = wasm.exports;

  collectedPatches = [];
  const oldC = buildWasmTree(wasm, oldTree);
  const newC = buildWasmTree(wasm, newTree);

  try {
    bud_wasm_diff(oldC, newC);
  } finally {
    if (oldC) bud_free(oldC);
    if (newC) bud_free(newC);
  }

  const result = collectedPatches;
  collectedPatches = [];
  return result;
}

export function version() {
  if (nativeBinding && nativeBinding.version) {
    return nativeBinding.version();
  }
  return "1.2.1";
}

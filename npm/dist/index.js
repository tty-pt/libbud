import { loadWasmModule, loadNativeAddon, getSubstrate } from "@tty-pt/stele";

import { BudNode, h } from "./node.js";
export { BudNode, h };

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



function buildWasmTree(wasm, node) {
  if (node == null || typeof node === "boolean") return 0;
  const { malloc, free, bud_element, bud_text, bud_set_attr, bud_append } = wasm.exports;
  const enc = new TextEncoder();

  function allocStr(s) {
    const bytes = enc.encode(s);
    const ptr = malloc(bytes.length + 1);
    new Uint8Array(wasm.exports.memory.buffer, ptr, bytes.length + 1).set([...bytes, 0]);
    return ptr;
  }

  if (typeof node === "string" || typeof node === "number") {
    const sPtr = allocStr(String(node));
    const t = bud_text(sPtr);
    free(sPtr);
    return t;
  }

  if (typeof node.tag === "function") {
    return buildWasmTree(wasm, node.resolve());
  }

  const tagPtr = allocStr(node.isFragment ? "div" : (node.tag || "div"));
  const cNode = bud_element(tagPtr);
  free(tagPtr);

  function setAttr(k, v) {
    const kPtr = allocStr(k);
    const vPtr = allocStr(String(v));
    bud_set_attr(cNode, kPtr, vPtr);
    free(kPtr);
    free(vPtr);
  }

  if (node.props) {
    for (const k in node.props) {
      if (k === "children" || k === "key" || k === "ref" || k === "dangerouslySetInnerHTML") continue;
      const v = node.props[k];
      if (v == null || v === false) continue;
      if (k.charCodeAt(0) === 111 && k.charCodeAt(1) === 110 && typeof v === "function") continue;
      const name = k === "className" ? "class" : k === "htmlFor" ? "for" : k;
      setAttr(name, v === true ? "" : v);
    }
  }

  if (node.attrs) {
    for (const [k, v] of Object.entries(node.attrs)) {
      if (v == null || v === false) continue;
      setAttr(k, v === true ? "" : v);
    }
  }

  if (node.text) {
    const tPtr = allocStr(node.text);
    const tNode = bud_text(tPtr);
    bud_append(cNode, tNode);
    free(tPtr);
  }

  function appendChild(ch) {
    if (ch == null || typeof ch === "boolean") return;
    if (typeof ch === "object" && typeof ch.tag === "function") {
      appendChild(ch.resolve());
      return;
    }
    if (typeof ch === "object" && ch.isFragment) {
      if (ch.text) appendChild(ch.text);
      if (Array.isArray(ch.children)) {
        for (let i = 0; i < ch.children.length; i++) appendChild(ch.children[i]);
      }
      return;
    }
    const chNode = buildWasmTree(wasm, ch);
    if (chNode) bud_append(cNode, chNode);
  }

  if (Array.isArray(node.children)) {
    for (let i = 0; i < node.children.length; i++) {
      appendChild(node.children[i]);
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

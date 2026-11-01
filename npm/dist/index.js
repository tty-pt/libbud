import { createRequire } from "node:module";
import path from "node:path";

let nativeBinding = null;
try {
  const req = createRequire(import.meta.url);
  try {
    nativeBinding = req("../build/Release/bud_node.node");
  } catch {}
  if (!nativeBinding) {
    try {
      nativeBinding = req(path.join(process.cwd(), "build/Release/bud_node.node"));
    } catch {}
  }
} catch {
  // In-memory fallback
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

/**
 * Computes patch operations between two VDOM trees using C bud_vdom_diff
 */
export function diffVdom(oldTree, newTree) {
  if (nativeBinding && nativeBinding.diffVdom) {
    return nativeBinding.diffVdom(oldTree, newTree);
  }

  const patches = [];
  if (!oldTree) {
    patches.push({ type: "MOUNT", op: "MOUNT", node: newTree });
  } else if (!newTree) {
    patches.push({ type: "UNMOUNT", op: "UNMOUNT" });
  } else if (oldTree.tag !== newTree.tag) {
    patches.push({ type: "REPLACE", op: "REPLACE", node: newTree });
  } else {
    if (oldTree.text !== newTree.text) {
      patches.push({ type: "SET_TEXT", op: "SET_TEXT", text: newTree.text });
    }
  }
  return patches;
}

export function version() {
  if (nativeBinding && nativeBinding.version) {
    return nativeBinding.version();
  }
  return "0.1.0";
}

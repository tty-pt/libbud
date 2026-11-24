/**
 * @file node.js
 * @brief Core BudNode virtual DOM node descriptor without external package dependencies.
 */

export const EMPTY_ARR = Object.freeze([]);

const VOID_TAGS = new Set([
  "area", "base", "br", "col", "embed", "hr", "img",
  "input", "link", "meta", "param", "source", "track", "wbr"
]);

let ssrEvalComponent = null;

export function _setComponentRenderer(fn) {
  ssrEvalComponent = fn;
}

export function escapeHtml(str) {
  const s = String(str);
  let out = "";
  let last = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    let esc = null;
    if (c === 38) esc = "&amp;";
    else if (c === 60) esc = "&lt;";
    else if (c === 62) esc = "&gt;";
    if (esc !== null) {
      if (last < i) out += s.slice(last, i);
      out += esc;
      last = i + 1;
    }
  }
  return last === 0 ? s : out + s.slice(last);
}

function escapeAttr(str) {
  const s = String(str);
  let out = "";
  let last = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    let esc = null;
    if (c === 38) esc = "&amp;";
    else if (c === 34) esc = "&quot;";
    else if (c === 60) esc = "&lt;";
    else if (c === 62) esc = "&gt;";
    if (esc !== null) {
      if (last < i) out += s.slice(last, i);
      out += esc;
      last = i + 1;
    }
  }
  return last === 0 ? s : out + s.slice(last);
}

function styleObjToString(obj) {
  let out = "";
  for (const k in obj) {
    const v = obj[k];
    if (v == null || v === false) continue;
    const prop = k.startsWith("--") ? k : k.replace(/[A-Z]/g, (m) => "-" + m.toLowerCase());
    out += `${prop}:${v};`;
  }
  return out;
}

function serializeAttrs(props, attrs) {
  let out = "";
  if (props) {
    for (const k in props) {
      if (k === "children" || k === "key" || k === "ref" || k === "dangerouslySetInnerHTML") continue;
      const v = props[k];
      if (v == null || v === false) continue;
      if (k.charCodeAt(0) === 111 && k.charCodeAt(1) === 110 && typeof v === "function") continue;
      const name = k === "className" ? "class" : k === "htmlFor" ? "for" : k;
      if (name === "style" && typeof v === "object") {
        const css = styleObjToString(v);
        if (css) out += ` style="${escapeAttr(css)}"`;
      } else if (v === true) {
        out += ` ${name}=""`;
      } else {
        out += ` ${name}="${escapeAttr(v)}"`;
      }
    }
  }
  if (attrs) {
    for (const k in attrs) {
      const v = attrs[k];
      if (v == null || v === false) continue;
      out += v === true ? ` ${k}=""` : ` ${k}="${escapeAttr(v)}"`;
    }
  }
  return out;
}

export class BudNode {
  constructor(tag, attrs = null, children = EMPTY_ARR, props = null, key = undefined) {
    this.tag = tag;
    this.attrs = attrs;
    this.props = props;
    this.children = children;
    this.text = "";
    this.key = key;
    this.isFragment = false;
    this._resolved = undefined;
  }

  attr(key, val) {
    if (!this.attrs) this.attrs = {};
    this.attrs[key] = val;
    return this;
  }

  child(node) {
    if (this.children === EMPTY_ARR) this.children = [node];
    else this.children.push(node);
    return this;
  }

  setText(text) {
    this.text = String(text);
    return this;
  }

  resolve() {
    if (typeof this.tag !== "function") return this;
    if (this._resolved !== undefined) return this._resolved;
    const p = this.key !== undefined ? { ...this.props, key: this.key } : (this.props || {});
    const out = ssrEvalComponent ? ssrEvalComponent(this.tag, p) : this.tag(p);
    this._resolved = out instanceof BudNode ? out.resolve() : out;
    return this._resolved;
  }

  render() {
    if (typeof this.tag === "function") {
      const resolved = this.resolve();
      if (resolved == null || typeof resolved === "boolean") return "";
      return resolved instanceof BudNode ? resolved.render() : escapeHtml(resolved);
    }
    if (this.isFragment) {
      let out = "";
      if (this.text) out += escapeHtml(this.text);
      const ch = this.children;
      for (let i = 0; i < ch.length; i++) {
        const c = ch[i];
        if (c == null || typeof c === "boolean") continue;
        out += c instanceof BudNode ? c.render() : escapeHtml(c);
      }
      return out;
    }
    const attrStr = serializeAttrs(this.props, this.attrs);
    if (VOID_TAGS.has(this.tag)) {
      return `<${this.tag}${attrStr}>`;
    }
    let body = "";
    const rawHtml = this.props && this.props.dangerouslySetInnerHTML;
    if (rawHtml && rawHtml.__html != null) {
      body = String(rawHtml.__html);
    } else {
      if (this.text) body += escapeHtml(this.text);
      const ch = this.children;
      for (let i = 0; i < ch.length; i++) {
        const c = ch[i];
        if (c == null || typeof c === "boolean") continue;
        body += c instanceof BudNode ? c.render() : escapeHtml(c);
      }
    }
    return `<${this.tag}${attrStr}>${body}</${this.tag}>`;
  }
}

/**
 * Creates virtual DOM node (mirrors bud_node_create)
 */
export function h(tag, attrs = {}, children = []) {
  return new BudNode(tag, attrs, children);
}

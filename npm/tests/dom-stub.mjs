/**
 * @file dom-stub.mjs
 * @brief Lightweight deterministic DOM implementation with mutation & focus/selection tracking for tests & benchmarks.
 */

export function createDomEnv() {
  const stats = {
    createElement: 0,
    createTextNode: 0,
    insertBefore: 0,
    removeChild: 0,
    setAttribute: 0,
    removeAttribute: 0,
    textContentSet: 0,
    nodeValueSet: 0,
    classNameSet: 0,
    addEventListener: 0,
    removeEventListener: 0,
  };

  function resetStats() {
    for (const k in stats) stats[k] = 0;
  }

  const doc = {
    activeElement: null,
    body: null,
    createElement(tag) {
      stats.createElement++;
      return new MockElement(tag.toLowerCase(), null, doc);
    },
    createElementNS(ns, tag) {
      stats.createElement++;
      return new MockElement(tag, ns, doc);
    },
    createTextNode(text) {
      stats.createTextNode++;
      return new MockText(String(text), doc);
    },
  };

  class MockStyle {
    constructor() {
      this._map = Object.create(null);
      this._cssText = "";
    }
    get cssText() {
      return this._cssText;
    }
    set cssText(val) {
      this._cssText = String(val);
      this._map = Object.create(null);
    }
    setProperty(k, v) {
      this._map[k] = String(v);
    }
    removeProperty(k) {
      delete this._map[k];
    }
  }

  class MockNode {
    constructor(nodeType, ownerDocument) {
      this.nodeType = nodeType;
      this.ownerDocument = ownerDocument;
      this.parentNode = null;
      this.childNodes = [];
    }
    get firstChild() {
      return this.childNodes.length > 0 ? this.childNodes[0] : null;
    }
    get nextSibling() {
      if (!this.parentNode) return null;
      const list = this.parentNode.childNodes;
      const idx = list.indexOf(this);
      return idx >= 0 && idx + 1 < list.length ? list[idx + 1] : null;
    }
    contains(other) {
      let cur = other;
      while (cur) {
        if (cur === this) return true;
        cur = cur.parentNode;
      }
      return false;
    }
  }

  class MockText extends MockNode {
    constructor(val, ownerDocument) {
      super(3, ownerDocument);
      this._val = val;
    }
    get nodeValue() {
      return this._val;
    }
    set nodeValue(v) {
      stats.nodeValueSet++;
      this._val = String(v);
    }
    get textContent() {
      return this._val;
    }
    set textContent(v) {
      stats.textContentSet++;
      this._val = String(v);
    }
  }

  class MockElement extends MockNode {
    constructor(tagName, namespaceURI, ownerDocument) {
      super(1, ownerDocument);
      this.tagName = tagName.toUpperCase();
      this.localName = tagName;
      this.namespaceURI = namespaceURI;
      this.attributes = Object.create(null);
      this.style = new MockStyle();
      this._listeners = Object.create(null);
      this._className = "";
      this._value = "";
      this.checked = false;
      this.disabled = false;
      this.selected = false;
      this.readOnly = false;
      this.hidden = false;
      this.required = false;
      this.multiple = false;
      this.open = false;
      this.selectionStart = 0;
      this.selectionEnd = 0;
      this.innerHTML = "";
    }
    get className() {
      return this._className;
    }
    set className(v) {
      stats.classNameSet++;
      this._className = String(v);
      if (this._className) this.attributes["class"] = this._className;
      else delete this.attributes["class"];
    }
    get value() {
      return this._value;
    }
    set value(v) {
      this._value = String(v);
      // Real browsers reset caret to end when el.value is assigned!
      this.selectionStart = this._value.length;
      this.selectionEnd = this._value.length;
    }
    setSelectionRange(start, end) {
      this.selectionStart = start;
      this.selectionEnd = end;
    }
    focus() {
      this.ownerDocument.activeElement = this;
    }
    blur() {
      if (this.ownerDocument.activeElement === this) {
        this.ownerDocument.activeElement = this.ownerDocument.body;
      }
    }
    setAttribute(k, v) {
      stats.setAttribute++;
      this.attributes[k] = String(v);
      if (k === "class") this._className = String(v);
    }
    getAttribute(k) {
      return k in this.attributes ? this.attributes[k] : null;
    }
    removeAttribute(k) {
      stats.removeAttribute++;
      delete this.attributes[k];
      if (k === "class") this._className = "";
    }
    appendChild(child) {
      return this.insertBefore(child, null);
    }
    insertBefore(child, before) {
      stats.insertBefore++;
      // Simulate real browser behavior: moving a focused node blurs it!
      const docRef = this.ownerDocument;
      if (child.parentNode && docRef.activeElement && child.contains(docRef.activeElement)) {
        docRef.activeElement = docRef.body;
      }
      if (child.parentNode) {
        const oldList = child.parentNode.childNodes;
        const oldIdx = oldList.indexOf(child);
        if (oldIdx !== -1) oldList.splice(oldIdx, 1);
      }
      child.parentNode = this;
      if (before == null) {
        this.childNodes.push(child);
      } else {
        const idx = this.childNodes.indexOf(before);
        if (idx === -1) this.childNodes.push(child);
        else this.childNodes.splice(idx, 0, child);
      }
      return child;
    }
    removeChild(child) {
      stats.removeChild++;
      const docRef = this.ownerDocument;
      if (docRef.activeElement && child.contains(docRef.activeElement)) {
        docRef.activeElement = docRef.body;
      }
      const idx = this.childNodes.indexOf(child);
      if (idx !== -1) this.childNodes.splice(idx, 1);
      child.parentNode = null;
      return child;
    }
    get textContent() {
      let out = "";
      for (let i = 0; i < this.childNodes.length; i++) {
        out += this.childNodes[i].textContent;
      }
      return out;
    }
    set textContent(v) {
      stats.textContentSet++;
      const docRef = this.ownerDocument;
      if (docRef.activeElement && this.contains(docRef.activeElement) && docRef.activeElement !== this) {
        docRef.activeElement = docRef.body;
      }
      for (let i = 0; i < this.childNodes.length; i++) this.childNodes[i].parentNode = null;
      this.childNodes = [];
      const str = String(v);
      if (str.length > 0) {
        const t = new MockText(str, this.ownerDocument);
        t.parentNode = this;
        this.childNodes.push(t);
      }
    }
    addEventListener(type, fn) {
      stats.addEventListener++;
      if (!this._listeners[type]) this._listeners[type] = [];
      if (!this._listeners[type].includes(fn)) this._listeners[type].push(fn);
    }
    removeEventListener(type, fn) {
      stats.removeEventListener++;
      const list = this._listeners[type];
      if (!list) return;
      const idx = list.indexOf(fn);
      if (idx !== -1) list.splice(idx, 1);
    }
    dispatchEvent(event) {
      const bubbles = event.bubbles !== false;
      const ev = {
        target: this,
        currentTarget: this,
        cancelBubble: false,
        stopPropagation() {
          this.cancelBubble = true;
        },
        ...event,
      };
      let cur = this;
      while (cur) {
        const list = cur._listeners && cur._listeners[ev.type];
        if (list) {
          ev.currentTarget = cur;
          for (let i = 0; i < list.length; i++) {
            list[i].call(cur, ev);
          }
        }
        if (!bubbles || ev.cancelBubble) break;
        cur = cur.parentNode;
      }
    }
    toHTML() {
      let attrStr = "";
      for (const k in this.attributes) {
        attrStr += ` ${k}="${this.attributes[k]}"`;
      }
      let inner = "";
      for (let i = 0; i < this.childNodes.length; i++) {
        const ch = this.childNodes[i];
        inner += ch.nodeType === 3 ? ch.nodeValue : ch.toHTML();
      }
      return `<${this.localName}${attrStr}>${inner}</${this.localName}>`;
    }
  }

  doc.body = new MockElement("body", null, doc);
  doc.activeElement = doc.body;

  return { doc, stats, resetStats, createContainer: () => doc.createElement("div") };
}

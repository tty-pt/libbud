import { BudNode, EMPTY_ARR } from "../dist/node.js";
import {
  Fragment,
  _runWithSlot,
  _FLAG_DIRTY,
  _FLAG_UNMOUNTED,
  _FLAG_HAS_CLEANUP,
  _FLAG_IS_SVG,
} from "./jsx-runtime.js";

const SVG_NS = "http://www.w3.org/2000/svg";
const EMPTY_OBJ = Object.freeze({});
const MAX_FLUSH_LOOPS = 50;

const BOOL_PROPS = new Set([
  "disabled", "checked", "selected", "readOnly", "hidden",
  "required", "multiple", "open", "muted", "autoFocus", "controls", "loop"
]);

// --- Scheduler State ---
const dirtySlots = new Set();
let pendingEffects = [];
let flushScheduled = false;

function scheduleSlot(slot) {
  if (slot.flags & _FLAG_UNMOUNTED) return;
  slot.flags |= _FLAG_DIRTY;
  dirtySlots.add(slot);
  if (!flushScheduled) {
    flushScheduled = true;
    queueMicrotask(flushDirty);
  }
}

export function flushSync() {
  flushScheduled = false;
  flushDirty();
}

function flushDirty() {
  flushScheduled = false;
  let loops = 0;
  while (dirtySlots.size > 0) {
    if (++loops > MAX_FLUSH_LOOPS) {
      dirtySlots.clear();
      throw new Error("Bud: exceeded maximum nested update depth");
    }
    const batch = Array.from(dirtySlots);
    dirtySlots.clear();
    if (batch.length > 1) batch.sort((a, b) => a.depth - b.depth);
    for (let i = 0; i < batch.length; i++) {
      const slot = batch[i];
      if ((slot.flags & _FLAG_DIRTY) && !(slot.flags & _FLAG_UNMOUNTED)) {
        reconcileComponentSlot(slot);
      }
    }
  }
  flushEffects();
}

function flushEffects() {
  if (pendingEffects.length === 0) return;
  const list = pendingEffects;
  pendingEffects = [];
  for (let i = 0; i < list.length; i++) {
    const hook = list[i];
    if (typeof hook.cleanup === "function") {
      hook.cleanup();
      hook.cleanup = null;
    }
    const res = hook.effect();
    if (typeof res === "function") hook.cleanup = res;
  }
}

// --- Slot Helpers ---
function createSlot(parent, parentDom, root, depth, isSvg) {
  return {
    vnode: null,
    dom: null,
    parent,
    parentDom,
    root,
    children: null,
    hooks: null,
    depth,
    flags: isSvg ? _FLAG_IS_SVG : 0,
  };
}

function markHasCleanup(slot) {
  let s = slot;
  while (s && !(s.flags & _FLAG_HAS_CLEANUP)) {
    s.flags |= _FLAG_HAS_CLEANUP;
    s = s.parent;
  }
}

function getDoc(dom) {
  return (dom && dom.ownerDocument) || (typeof document !== "undefined" ? document : null);
}

function getFirstDom(slot) {
  if (!slot) return null;
  if (slot.dom) return slot.dom;
  const ch = slot.children;
  if (!ch) return null;
  if (Array.isArray(ch)) {
    for (let i = 0; i < ch.length; i++) {
      const d = getFirstDom(ch[i]);
      if (d) return d;
    }
    return null;
  }
  return getFirstDom(ch);
}

function findNextDom(slot) {
  let cur = slot;
  while (cur && cur.parent) {
    const p = cur.parent;
    if (Array.isArray(p.children)) {
      const list = p.children;
      const idx = list.indexOf(cur);
      if (idx !== -1) {
        for (let i = idx + 1; i < list.length; i++) {
          const d = getFirstDom(list[i]);
          if (d) return d;
        }
      }
    }
    // Stop at enclosing Element boundary (its siblings live in an outer parentDom)
    if (p.dom) return null;
    cur = p;
  }
  return null;
}

function moveSlotDom(slot, parentDom, beforeDom) {
  if (!slot) return;
  if (slot.dom) {
    if (slot.dom !== beforeDom && slot.dom.nextSibling !== beforeDom) {
      parentDom.insertBefore(slot.dom, beforeDom);
    }
    return;
  }
  const ch = slot.children;
  if (!ch) return;
  if (Array.isArray(ch)) {
    for (let i = 0; i < ch.length; i++) moveSlotDom(ch[i], parentDom, beforeDom);
  } else {
    moveSlotDom(ch, parentDom, beforeDom);
  }
}

function setRef(ref, value) {
  if (!ref) return;
  if (typeof ref === "function") ref(value);
  else if (typeof ref === "object") ref.current = value;
}

function markSubtreeUnmounted(slot) {
  if (!slot) return;
  slot.flags |= _FLAG_UNMOUNTED;
  slot.flags &= ~_FLAG_DIRTY;
  const ch = slot.children;
  if (!ch) return;
  if (Array.isArray(ch)) {
    for (let i = 0; i < ch.length; i++) markSubtreeUnmounted(ch[i]);
  } else {
    markSubtreeUnmounted(ch);
  }
}

function unmountSlot(slot, skipDomRemove) {
  if (!slot) return;
  slot.flags |= _FLAG_UNMOUNTED;
  slot.flags &= ~_FLAG_DIRTY;

  const vnode = slot.vnode;
  if (vnode && typeof vnode === "object" && vnode.props && vnode.props.ref) {
    setRef(vnode.props.ref, null);
  }

  if (slot.hooks) {
    for (let i = 0; i < slot.hooks.length; i++) {
      const h = slot.hooks[i];
      if (h && h.isEffect && typeof h.cleanup === "function") {
        h.cleanup();
        h.cleanup = null;
      }
    }
  }

  const removeChildDom = skipDomRemove || Boolean(slot.dom);
  const ch = slot.children;
  if (ch) {
    if (Array.isArray(ch)) {
      for (let i = 0; i < ch.length; i++) unmountSlot(ch[i], removeChildDom);
    } else {
      unmountSlot(ch, removeChildDom);
    }
    slot.children = null;
  }

  if (!skipDomRemove && slot.dom && slot.dom.parentNode) {
    slot.dom.parentNode.removeChild(slot.dom);
  }
  slot.dom = null;
}

// --- Root Event Delegation (bubbling) + Direct DOM Event Proxy (non-bubbling) ---
const DELEGATED_EVENTS = new Set([
  "click", "dblclick", "input", "change", "keydown", "keyup", "keypress",
  "mousedown", "mouseup", "mouseover", "mouseout", "mousemove",
  "pointerdown", "pointerup", "submit", "contextmenu",
  "touchstart", "touchend", "focusin", "focusout",
]);

function delegatedHandler(e) {
  const prop = "$$" + e.type;
  let node = e.target;
  const root = this;
  while (node && node !== root) {
    const fn = node[prop];
    if (fn) {
      fn(e);
      if (e.cancelBubble) return;
    }
    node = node.parentNode;
  }
  const rootFn = root[prop];
  if (rootFn) rootFn(e);
}

function eventProxy(e) {
  const evs = this._budEv;
  const fn = evs && evs[e.type];
  if (fn) fn(e);
}

function normalizeEventName(propKey, tag, props) {
  const ev = propKey.slice(2).toLowerCase();
  if (ev === "doubleclick") return "dblclick";
  if (ev === "change") {
    if (tag === "textarea") return "input";
    if (tag === "input") {
      const t = props && props.type;
      if (t !== "checkbox" && t !== "radio" && t !== "file") return "input";
    }
  }
  return ev;
}

function setEvent(dom, evName, fn, root) {
  if (root && DELEGATED_EVENTS.has(evName)) {
    const prop = "$$" + evName;
    if (fn) {
      let reg = root._budDelegated;
      if (!reg) reg = root._budDelegated = new Set();
      if (!reg.has(evName)) {
        reg.add(evName);
        root.addEventListener(evName, delegatedHandler);
      }
      dom[prop] = fn;
    } else if (dom[prop]) {
      dom[prop] = null;
    }
    return;
  }
  let evs = dom._budEv;
  if (fn) {
    if (!evs) {
      evs = dom._budEv = Object.create(null);
    }
    if (!(evName in evs)) {
      dom.addEventListener(evName, eventProxy);
    }
    evs[evName] = fn;
  } else if (evs && evName in evs) {
    delete evs[evName];
    dom.removeEventListener(evName, eventProxy);
  }
}

// --- Props & Style Patching ---
function patchStyle(dom, prev, next) {
  const style = dom.style;
  if (!style) {
    if (next == null) dom.removeAttribute("style");
    else if (typeof next === "string") dom.setAttribute("style", next);
    return;
  }
  if (typeof next === "string") {
    if (prev !== next) style.cssText = next;
    return;
  }
  if (typeof prev === "string") {
    style.cssText = "";
  } else if (prev && typeof prev === "object") {
    for (const k in prev) {
      if (!next || !(k in next)) {
        if (k.startsWith("--")) style.removeProperty(k);
        else style[k] = "";
      }
    }
  }
  if (next && typeof next === "object") {
    for (const k in next) {
      const v = next[k];
      if (!prev || prev[k] !== v) {
        if (k.startsWith("--")) style.setProperty(k, v == null ? "" : String(v));
        else style[k] = v == null ? "" : v;
      }
    }
  }
}

function setProp(dom, tag, k, prev, next, isSvg, nextProps, slot, root) {
  if (k === "children" || k === "key") return;
  if (k === "ref") {
    if (prev !== next) {
      if (prev) setRef(prev, null);
      if (next) {
        setRef(next, dom);
        markHasCleanup(slot);
      }
    }
    return;
  }
  if (k === "dangerouslySetInnerHTML") {
    const nextHtml = next && next.__html != null ? String(next.__html) : "";
    const prevHtml = prev && prev.__html != null ? String(prev.__html) : "";
    if (nextHtml !== prevHtml) dom.innerHTML = nextHtml;
    return;
  }
  if (k.charCodeAt(0) === 111 && k.charCodeAt(1) === 110 && k.length > 2) {
    const c2 = k.charCodeAt(2);
    if (c2 >= 65 && c2 <= 90) {
      const evName = normalizeEventName(k, tag, nextProps || EMPTY_OBJ);
      setEvent(dom, evName, typeof next === "function" ? next : null, root);
      return;
    }
  }
  if (k === "className" || k === "class") {
    const val = next == null || next === false ? "" : String(next);
    if (isSvg) {
      if (val) dom.setAttribute("class", val);
      else dom.removeAttribute("class");
    } else if (dom.className !== val) {
      dom.className = val;
    }
    return;
  }
  if (k === "htmlFor") {
    if (next == null || next === false) dom.removeAttribute("for");
    else dom.setAttribute("for", String(next));
    return;
  }
  if (k === "style") {
    patchStyle(dom, prev, next);
    return;
  }
  if (k === "value") {
    // <select> value is synced after children mount/patch
    if (tag === "select") return;
    if (next == null) {
      if (prev != null && dom.value !== "") dom.value = "";
    } else {
      const str = String(next);
      if (dom.value !== str) dom.value = str;
    }
    return;
  }
  if (BOOL_PROPS.has(k)) {
    const boolVal = Boolean(next);
    const propName = k === "readOnly" ? "readOnly" : k;
    const attrName = k.toLowerCase();
    if (propName in dom) dom[propName] = boolVal;
    if (boolVal) dom.setAttribute(attrName, "");
    else dom.removeAttribute(attrName);
    return;
  }
  if (next == null || next === false) {
    dom.removeAttribute(k);
  } else {
    dom.setAttribute(k, next === true ? "" : String(next));
  }
}

function mountProps(dom, tag, props, isSvg, slot, root) {
  for (const k in props) {
    if (k === "children" || k === "key") continue;
    const nv = props[k];
    if (nv == null || nv === false) continue;
    if (k === "className" || k === "class") {
      if (nv !== "") {
        if (isSvg) dom.setAttribute("class", String(nv));
        else dom.className = String(nv);
      }
      continue;
    }
    if (k.charCodeAt(0) === 111 && k.charCodeAt(1) === 110 && k.length > 2) {
      const c2 = k.charCodeAt(2);
      if (c2 >= 65 && c2 <= 90) {
        if (typeof nv === "function") {
          setEvent(dom, normalizeEventName(k, tag, props), nv, root);
        }
        continue;
      }
    }
    setProp(dom, tag, k, undefined, nv, isSvg, props, slot, root);
  }
}

function patchProps(dom, tag, prevProps, nextProps, isSvg, slot, root) {
  const prev = prevProps || EMPTY_OBJ;
  const next = nextProps || EMPTY_OBJ;
  if (prev === next) return;

  for (const k in prev) {
    if (!(k in next)) {
      setProp(dom, tag, k, prev[k], undefined, isSvg, next, slot, root);
    }
  }
  for (const k in next) {
    const pv = prev[k];
    const nv = next[k];
    if (pv !== nv) {
      setProp(dom, tag, k, pv, nv, isSvg, next, slot, root);
    }
  }
}

// --- VNode Matching & Normalization ---
function normalizeOutput(out) {
  if (out == null || typeof out === "boolean") return null;
  const t = typeof out;
  if (t === "string" || t === "number") return "" + out;
  if (Array.isArray(out)) {
    const frag = new BudNode(Fragment, null, EMPTY_ARR);
    frag.isFragment = true;
    const normalized = new Array(out.length);
    for (let i = 0; i < out.length; i++) normalized[i] = normalizeOutput(out[i]);
    frag.children = normalized;
    return frag;
  }
  return out.constructor === BudNode ? out : null;
}

function sameVNode(a, b) {
  if (a === b) return true;
  if (a == null || b == null) return a == null && b == null;
  if (typeof a === "string" || typeof b === "string") {
    return typeof a === "string" && typeof b === "string";
  }
  return a.tag === b.tag && a.key === b.key;
}

function getEffectiveProps(vnode) {
  if (vnode.attrs) {
    return vnode.props ? { ...vnode.props, ...vnode.attrs } : vnode.attrs;
  }
  return vnode.props || EMPTY_OBJ;
}

function getNormalizedChildren(vnode) {
  if (vnode.text && vnode.children.length > 0) {
    return [vnode.text, ...vnode.children];
  }
  return vnode.children;
}

// --- Core Mount & Patch ---
function mountSlot(vnode, parent, parentDom, beforeDom, depth, isSvg, root, doc) {
  const slot = createSlot(parent, parentDom, root, depth, isSvg);
  slot.vnode = vnode;

  if (vnode == null) {
    return slot;
  }

  if (typeof vnode === "string") {
    const dom = doc.createTextNode(vnode);
    slot.dom = dom;
    if (beforeDom) parentDom.insertBefore(dom, beforeDom);
    else parentDom.appendChild(dom);
    return slot;
  }

  if (typeof vnode.tag === "function") {
    const props = vnode.key !== undefined ? { ...vnode.props, key: vnode.key } : (vnode.props || EMPTY_OBJ);
    const out = normalizeOutput(_runWithSlot(slot, scheduleSlot, pendingEffects, vnode.tag, props));
    slot.children = mountSlot(out, slot, parentDom, beforeDom, depth + 1, isSvg, root, doc);
    return slot;
  }

  if (vnode.isFragment) {
    const ch = getNormalizedChildren(vnode);
    const len = ch.length;
    const childSlots = new Array(len);
    for (let i = 0; i < len; i++) {
      childSlots[i] = mountSlot(ch[i], slot, parentDom, beforeDom, depth + 1, isSvg, root, doc);
    }
    slot.children = childSlots;
    return slot;
  }

  // Intrinsic Element
  const tag = vnode.tag;
  const elIsSvg = isSvg || tag === "svg";
  const childIsSvg = tag === "foreignObject" ? false : elIsSvg;
  const dom = elIsSvg ? doc.createElementNS(SVG_NS, tag) : doc.createElement(tag);
  slot.dom = dom;

  const props = getEffectiveProps(vnode);
  if (props !== EMPTY_OBJ) {
    mountProps(dom, tag, props, elIsSvg, slot, root);
  }

  if (!(props && props.dangerouslySetInnerHTML)) {
    if (vnode.text && vnode.children.length === 0) {
      dom.textContent = vnode.text;
    } else {
      const ch = vnode.text ? [vnode.text, ...vnode.children] : vnode.children;
      const len = ch.length;
      if (len > 0) {
        const childSlots = new Array(len);
        for (let i = 0; i < len; i++) {
          childSlots[i] = mountSlot(ch[i], slot, dom, null, depth + 1, childIsSvg, root, doc);
        }
        slot.children = childSlots;
      }
    }
  }

  if (tag === "select" && props && "value" in props) {
    dom.value = props.value == null ? "" : String(props.value);
  }

  if (beforeDom) parentDom.insertBefore(dom, beforeDom);
  else parentDom.appendChild(dom);
  return slot;
}

function reconcileComponentSlot(slot) {
  slot.flags &= ~_FLAG_DIRTY;
  const vnode = slot.vnode;
  const isSvg = Boolean(slot.flags & _FLAG_IS_SVG);
  const props = vnode.key !== undefined ? { ...vnode.props, key: vnode.key } : (vnode.props || EMPTY_OBJ);
  const out = normalizeOutput(_runWithSlot(slot, scheduleSlot, pendingEffects, vnode.tag, props));
  const oldChild = slot.children;
  if (oldChild) {
    slot.children = patchSlot(oldChild, out, null);
  } else {
    slot.children = mountSlot(
      out,
      slot,
      slot.parentDom,
      findNextDom(slot),
      slot.depth + 1,
      isSvg,
      slot.root,
      getDoc(slot.parentDom)
    );
  }
}

function patchSlot(slot, nextVnode, explicitBeforeDom) {
  const prevVnode = slot.vnode;

  // O(1) reference identity bailout (unless component itself is dirty)
  if (prevVnode === nextVnode && !(slot.flags & _FLAG_DIRTY)) {
    return slot;
  }

  if (!sameVNode(prevVnode, nextVnode)) {
    const beforeDom = explicitBeforeDom !== undefined && explicitBeforeDom !== null
      ? explicitBeforeDom
      : (getFirstDom(slot) || findNextDom(slot));
    const parent = slot.parent;
    const parentDom = slot.parentDom;
    const root = slot.root;
    const depth = slot.depth;
    const isSvg = Boolean(slot.flags & _FLAG_IS_SVG);

    const nextSlot = mountSlot(nextVnode, parent, parentDom, beforeDom, depth, isSvg, root, getDoc(parentDom));
    unmountSlot(slot, false);
    return nextSlot;
  }

  // Both null
  if (nextVnode == null) {
    slot.vnode = null;
    return slot;
  }

  // Both Text
  if (typeof nextVnode === "string") {
    if (prevVnode !== nextVnode) {
      slot.vnode = nextVnode;
      slot.dom.nodeValue = nextVnode;
    }
    return slot;
  }

  // Same Component
  if (typeof nextVnode.tag === "function") {
    const memoCompare = nextVnode.tag._budMemo;
    if (memoCompare && !(slot.flags & _FLAG_DIRTY) && memoCompare(prevVnode.props, nextVnode.props)) {
      slot.vnode = nextVnode;
      return slot;
    }
    slot.vnode = nextVnode;
    reconcileComponentSlot(slot);
    return slot;
  }

  // Same Fragment
  if (nextVnode.isFragment) {
    slot.vnode = nextVnode;
    const oldCh = slot.children || EMPTY_ARR;
    const newCh = getNormalizedChildren(nextVnode);
    slot.children = patchChildren(slot, slot.parentDom, oldCh, newCh, slot.depth + 1, Boolean(slot.flags & _FLAG_IS_SVG));
    return slot;
  }

  // Same Intrinsic Element — mutate in place!
  const dom = slot.dom;
  const tag = nextVnode.tag;
  const elIsSvg = Boolean(slot.flags & _FLAG_IS_SVG) || tag === "svg";
  const childIsSvg = tag === "foreignObject" ? false : elIsSvg;

  const prevProps = getEffectiveProps(prevVnode);
  const nextProps = getEffectiveProps(nextVnode);
  patchProps(dom, tag, prevProps, nextProps, elIsSvg, slot, slot.root);

  if (!(nextProps && nextProps.dangerouslySetInnerHTML)) {
    const oldPureText = prevVnode.text && prevVnode.children.length === 0;
    const newPureText = nextVnode.text && nextVnode.children.length === 0;

    if (oldPureText && newPureText) {
      if (prevVnode.text !== nextVnode.text) {
        if (dom.firstChild && dom.firstChild.nodeType === 3) {
          dom.firstChild.nodeValue = nextVnode.text;
        } else {
          dom.textContent = nextVnode.text;
        }
      }
    } else if (newPureText) {
      if (slot.children && slot.children.length > 0) {
        clearChildrenFast(slot, dom, slot.children);
        slot.children = null;
      }
      dom.textContent = nextVnode.text;
    } else {
      if (oldPureText) {
        dom.textContent = "";
      }
      const oldCh = slot.children || EMPTY_ARR;
      const newCh = nextVnode.text ? [nextVnode.text, ...nextVnode.children] : nextVnode.children;
      if (oldCh.length > 0 || newCh.length > 0) {
        slot.children = patchChildren(slot, dom, oldCh, newCh, slot.depth + 1, childIsSvg);
      }
    }
  }

  if (tag === "select" && nextProps && "value" in nextProps) {
    const str = nextProps.value == null ? "" : String(nextProps.value);
    if (dom.value !== str) dom.value = str;
  }

  slot.vnode = nextVnode;
  return slot;
}

function clearChildrenFast(parentSlot, parentDom, oldChildren) {
  if (!(parentSlot.flags & _FLAG_HAS_CLEANUP) && parentSlot.dom === parentDom) {
    for (let i = 0; i < oldChildren.length; i++) markSubtreeUnmounted(oldChildren[i]);
    parentDom.textContent = "";
    return;
  }
  for (let i = 0; i < oldChildren.length; i++) unmountSlot(oldChildren[i], false);
}

// --- 5-Step Fast-Path + Keyed LIS Child Reconciliation ---
function computeLIS(arr) {
  const n = arr.length;
  const p = new Int32Array(n);
  const result = [];
  for (let i = 0; i < n; i++) {
    const val = arr[i];
    if (val === 0) continue;
    const lastIdx = result.length > 0 ? result[result.length - 1] : -1;
    if (lastIdx === -1 || arr[lastIdx] < val) {
      p[i] = lastIdx;
      result.push(i);
      continue;
    }
    let lo = 0;
    let hi = result.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (arr[result[mid]] < val) lo = mid + 1;
      else hi = mid;
    }
    if (val < arr[result[lo]]) {
      p[i] = lo > 0 ? result[lo - 1] : -1;
      result[lo] = i;
    }
  }
  let len = result.length;
  let u = len > 0 ? result[len - 1] : -1;
  while (len-- > 0) {
    result[len] = u;
    u = p[u];
  }
  return result;
}

function patchChildren(parentSlot, parentDom, oldChildren, newChildren, depth, isSvg) {
  const oldLen = oldChildren.length;
  const newLen = newChildren.length;

  // Step 0a: Bulk clear fast path
  if (newLen === 0) {
    if (oldLen > 0) clearChildrenFast(parentSlot, parentDom, oldChildren);
    return EMPTY_ARR;
  }

  // Step 0b: Bulk mount fast path
  if (oldLen === 0) {
    const beforeDom = parentSlot.dom === parentDom ? null : findNextDom(parentSlot);
    const root = parentSlot.root;
    const doc = getDoc(parentDom);
    const out = new Array(newLen);
    for (let i = 0; i < newLen; i++) {
      out[i] = mountSlot(newChildren[i], parentSlot, parentDom, beforeDom, depth, isSvg, root, doc);
    }
    return out;
  }

  const nextSlots = new Array(newLen);
  let i = 0;
  let oldEnd = oldLen - 1;
  let newEnd = newLen - 1;

  // Step 1: Common prefix sync
  while (i <= oldEnd && i <= newEnd && sameVNode(oldChildren[i].vnode, newChildren[i])) {
    nextSlots[i] = patchSlot(oldChildren[i], newChildren[i], null);
    i++;
  }

  // Step 2: Common suffix sync
  while (oldEnd >= i && newEnd >= i && sameVNode(oldChildren[oldEnd].vnode, newChildren[newEnd])) {
    nextSlots[newEnd] = patchSlot(oldChildren[oldEnd], newChildren[newEnd], null);
    oldEnd--;
    newEnd--;
  }

  // Step 3a: Pure middle insert
  if (i > oldEnd) {
    if (i <= newEnd) {
      let anchor = null;
      for (let k = newEnd + 1; k < newLen; k++) {
        anchor = getFirstDom(nextSlots[k]);
        if (anchor) break;
      }
      if (!anchor && parentSlot.dom !== parentDom) anchor = findNextDom(parentSlot);
      const root = parentSlot.root;
      const doc = getDoc(parentDom);
      for (let k = i; k <= newEnd; k++) {
        nextSlots[k] = mountSlot(newChildren[k], parentSlot, parentDom, anchor, depth, isSvg, root, doc);
      }
    }
    return nextSlots;
  }

  // Step 3b: Pure middle delete
  if (i > newEnd) {
    for (let k = i; k <= oldEnd; k++) unmountSlot(oldChildren[k], false);
    return nextSlots;
  }

  // Step 4: Unknown middle sequence (reorder / swap / mixed keyed+unkeyed)
  const s1 = i;
  const s2 = i;
  const toBePatched = newEnd - s2 + 1;

  const keyToNewIndex = new Map();
  for (let k = s2; k <= newEnd; k++) {
    const nv = newChildren[k];
    if (nv && typeof nv === "object" && nv.key !== undefined) {
      keyToNewIndex.set(nv.key, k);
    }
  }

  // Fast-path full keyed replacement (e.g. replace1k): if 0 old keys match and no cleanups, wipe in 1 textContent="" call!
  if (
    s1 === 0 &&
    oldEnd === oldLen - 1 &&
    !(parentSlot.flags & _FLAG_HAS_CLEANUP) &&
    parentSlot.dom === parentDom &&
    keyToNewIndex.size > 0
  ) {
    let anyMatch = false;
    for (let k = 0; k < oldLen; k++) {
      const ov = oldChildren[k].vnode;
      if (ov && typeof ov === "object" && ov.key !== undefined && keyToNewIndex.has(ov.key)) {
        anyMatch = true;
        break;
      }
    }
    if (!anyMatch) {
      for (let k = 0; k < oldLen; k++) markSubtreeUnmounted(oldChildren[k]);
      parentDom.textContent = "";
      const root = parentSlot.root;
      const doc = getDoc(parentDom);
      for (let k = 0; k < newLen; k++) {
        nextSlots[k] = mountSlot(newChildren[k], parentSlot, parentDom, null, depth, isSvg, root, doc);
      }
      return nextSlots;
    }
  }

  const newIndexToOldIndexPlus1 = new Int32Array(toBePatched);
  let moved = false;
  let maxNewIndexSoFar = 0;

  for (let k = s1; k <= oldEnd; k++) {
    const oldSlot = oldChildren[k];
    const ov = oldSlot.vnode;
    let newIndex;

    if (ov && typeof ov === "object" && ov.key !== undefined) {
      newIndex = keyToNewIndex.get(ov.key);
    } else {
      for (let j = s2; j <= newEnd; j++) {
        if (newIndexToOldIndexPlus1[j - s2] === 0 && sameVNode(ov, newChildren[j])) {
          newIndex = j;
          break;
        }
      }
    }

    if (newIndex === undefined || !sameVNode(ov, newChildren[newIndex])) {
      unmountSlot(oldSlot, false);
    } else {
      newIndexToOldIndexPlus1[newIndex - s2] = k + 1;
      if (newIndex >= maxNewIndexSoFar) maxNewIndexSoFar = newIndex;
      else moved = true;
      nextSlots[newIndex] = patchSlot(oldSlot, newChildren[newIndex], null);
    }
  }

  // Snapshot focused element before any insertBefore moves (beats React on keyed reorders!)
  const doc = getDoc(parentDom);
  const activeEl = moved ? doc && doc.activeElement : null;
  const trackFocus = Boolean(
    moved && activeEl && activeEl !== doc.body &&
    parentDom.contains && parentDom.contains(activeEl)
  );
  let selStart = null;
  let selEnd = null;
  if (trackFocus) {
    try {
      if ("selectionStart" in activeEl) {
        selStart = activeEl.selectionStart;
        selEnd = activeEl.selectionEnd;
      }
    } catch (_) {}
  }

  const lis = moved ? computeLIS(newIndexToOldIndexPlus1) : EMPTY_ARR;
  let lisIdx = lis.length - 1;

  // Find initial right anchor for backward walk
  let anchor = null;
  for (let k = newEnd + 1; k < newLen; k++) {
    anchor = getFirstDom(nextSlots[k]);
    if (anchor) break;
  }
  if (!anchor && parentSlot.dom !== parentDom) anchor = findNextDom(parentSlot);

  const root = parentSlot.root;
  for (let k = toBePatched - 1; k >= 0; k--) {
    const idx = s2 + k;
    if (newIndexToOldIndexPlus1[k] === 0) {
      nextSlots[idx] = mountSlot(newChildren[idx], parentSlot, parentDom, anchor, depth, isSvg, root, doc);
    } else if (moved) {
      if (lisIdx < 0 || k !== lis[lisIdx]) {
        moveSlotDom(nextSlots[idx], parentDom, anchor);
      } else {
        lisIdx--;
      }
    }
    const first = getFirstDom(nextSlots[idx]);
    if (first) anchor = first;
  }

  // Restore focus & selection if keyed reorder blurred the active input
  if (trackFocus && doc.activeElement !== activeEl && parentDom.contains(activeEl)) {
    try {
      activeEl.focus({ preventScroll: true });
    } catch (_) {
      try { activeEl.focus(); } catch (_) {}
    }
    if (selStart !== null && typeof activeEl.setSelectionRange === "function") {
      try { activeEl.setSelectionRange(selStart, selEnd); } catch (_) {}
    }
  }

  return nextSlots;
}

// --- Public Mount / Render Entry Point ---
export function render(vnode, container) {
  if (!container) throw new Error("Bud: render() requires a DOM container");
  const normalized = normalizeOutput(vnode);
  const existing = container._budRoot;

  if (normalized == null) {
    if (existing) {
      unmountSlot(existing, false);
      container._budRoot = null;
    }
    flushEffects();
    return;
  }

  if (!existing) {
    if (container.firstChild) container.textContent = "";
    container._budRoot = mountSlot(normalized, null, container, null, 0, false, container, getDoc(container));
  } else {
    container._budRoot = patchSlot(existing, normalized, null);
  }

  flushEffects();
}

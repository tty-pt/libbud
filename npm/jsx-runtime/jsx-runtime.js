import { BudNode, EMPTY_ARR, _setComponentRenderer } from "../dist/node.js";

const EMPTY_OBJ = Object.freeze({});

export const Fragment = Symbol.for("bud.frag");

export const _FLAG_DIRTY = 1;
export const _FLAG_UNMOUNTED = 2;
export const _FLAG_HAS_CLEANUP = 4;
export const _FLAG_IS_SVG = 8;

// --- Hook Dispatcher (Slot-Scoped for DOM Reconciler + SSR Fallback) ---
let activeSlot = null;
let activeHookIndex = 0;
let activeSchedule = null;
let activePendingEffects = null;

// Legacy/SSR standalone .render() fallback (preserves hooks.test.mjs without DOM container)
let rootScheduler = null;
const ssrInstanceMap = new WeakMap();

export function setRootScheduler(fn) {
  rootScheduler = fn;
}

function scheduleCurrent(slot, sched) {
  if (sched) sched(slot);
  else if (rootScheduler) rootScheduler();
}

export function _runWithSlot(slot, scheduleFn, pendingEffects, fn, props) {
  const prevSlot = activeSlot;
  const prevIdx = activeHookIndex;
  const prevSched = activeSchedule;
  const prevEffects = activePendingEffects;

  activeSlot = slot;
  activeHookIndex = 0;
  activeSchedule = scheduleFn;
  activePendingEffects = pendingEffects;

  try {
    return fn(props);
  } finally {
    activeSlot = prevSlot;
    activeHookIndex = prevIdx;
    activeSchedule = prevSched;
    activePendingEffects = prevEffects;
  }
}

_setComponentRenderer((type, props) => {
  let inst = ssrInstanceMap.get(type);
  if (!inst) {
    inst = { hooks: [], flags: 0, parent: null };
    ssrInstanceMap.set(type, inst);
  }
  return _runWithSlot(inst, null, null, type, props);
});

function getHookSlot() {
  if (!activeSlot) throw new Error("Bud hook called outside component render");
  if (!activeSlot.hooks) activeSlot.hooks = [];
  return activeSlot;
}

function depsChanged(prev, next) {
  if (!prev || !next || prev.length !== next.length) return true;
  for (let i = 0; i < prev.length; i++) {
    if (!Object.is(prev[i], next[i])) return true;
  }
  return false;
}

export function useState(initial) {
  const slot = getHookSlot();
  const idx = activeHookIndex++;
  const hooks = slot.hooks;
  if (idx >= hooks.length) {
    const val = typeof initial === "function" ? initial() : initial;
    const sched = activeSchedule;
    const hook = {
      val,
      set: (act) => {
        if (slot.flags & _FLAG_UNMOUNTED) return;
        const next = typeof act === "function" ? act(hook.val) : act;
        if (!Object.is(hook.val, next)) {
          hook.val = next;
          scheduleCurrent(slot, sched);
        }
      }
    };
    hooks.push(hook);
    return [hook.val, hook.set];
  }
  const hook = hooks[idx];
  return [hook.val, hook.set];
}

export function useReducer(reducer, initialArg, init) {
  const slot = getHookSlot();
  const idx = activeHookIndex++;
  const hooks = slot.hooks;
  if (idx >= hooks.length) {
    const val = init ? init(initialArg) : initialArg;
    const sched = activeSchedule;
    const hook = {
      val,
      reducer,
      dispatch: (action) => {
        if (slot.flags & _FLAG_UNMOUNTED) return;
        const next = hook.reducer(hook.val, action);
        if (!Object.is(hook.val, next)) {
          hook.val = next;
          scheduleCurrent(slot, sched);
        }
      }
    };
    hooks.push(hook);
    return [hook.val, hook.dispatch];
  }
  const hook = hooks[idx];
  hook.reducer = reducer;
  return [hook.val, hook.dispatch];
}

export function useRef(initial) {
  const slot = getHookSlot();
  const idx = activeHookIndex++;
  const hooks = slot.hooks;
  if (idx >= hooks.length) {
    const ref = { current: initial };
    hooks.push(ref);
    return ref;
  }
  return hooks[idx];
}

export function useMemo(factory, deps) {
  const slot = getHookSlot();
  const idx = activeHookIndex++;
  const hooks = slot.hooks;
  if (idx >= hooks.length) {
    const hook = { val: factory(), deps };
    hooks.push(hook);
    return hook.val;
  }
  const hook = hooks[idx];
  if (deps === undefined || depsChanged(hook.deps, deps)) {
    hook.val = factory();
    hook.deps = deps;
  }
  return hook.val;
}

export function useCallback(fn, deps) {
  return useMemo(() => fn, deps);
}

export function useEffect(effect, deps) {
  const slot = getHookSlot();
  const idx = activeHookIndex++;
  const hooks = slot.hooks;
  let hook;
  let shouldRun = false;

  if (idx >= hooks.length) {
    hook = { effect, deps, cleanup: null, isEffect: true };
    hooks.push(hook);
    shouldRun = true;
    // Bubble HAS_CLEANUP bit to ancestors for O(1) bulk-clear safety check
    let s = slot;
    while (s && !(s.flags & _FLAG_HAS_CLEANUP)) {
      s.flags |= _FLAG_HAS_CLEANUP;
      s = s.parent;
    }
  } else {
    hook = hooks[idx];
    if (deps === undefined || depsChanged(hook.deps, deps)) {
      hook.effect = effect;
      hook.deps = deps;
      shouldRun = true;
    }
  }

  if (shouldRun && activePendingEffects) {
    activePendingEffects.push(hook);
  }
}

export function shallowEqual(a, b) {
  if (Object.is(a, b)) return true;
  if (!a || !b || typeof a !== "object" || typeof b !== "object") return false;
  let countA = 0;
  for (const k in a) {
    countA++;
    if (!Object.is(a[k], b[k])) return false;
  }
  let countB = 0;
  for (const k in b) countB++;
  return countA === countB;
}

export function memo(fn, compare = shallowEqual) {
  function MemoComponent(props) {
    return fn(props);
  }
  MemoComponent._budMemo = compare;
  MemoComponent._budInner = fn;
  return MemoComponent;
}

// --- 1-Allocation Lazy JSX Factory ---
function makeImplicitFragment(arr) {
  const frag = new BudNode(Fragment, null, EMPTY_ARR);
  frag.isFragment = true;
  populateChildren(frag, arr, true);
  return frag;
}

function populateChildren(node, raw, isFragment) {
  if (raw == null || typeof raw === "boolean") return;

  const t = typeof raw;
  if (t === "string" || t === "number") {
    const str = "" + raw;
    if (isFragment) node.children = [str];
    else node.text = str;
    return;
  }

  if (raw.constructor === BudNode) {
    node.children = [raw];
    return;
  }

  if (!Array.isArray(raw)) return;

  const len = raw.length;
  if (len === 0) return;

  // Single scan: check if purely primitive (coalesce into 1 text string -> 0 array allocs!)
  // or already normalized BudNode/string/null list (reuse array directly -> 0 copy allocs!)
  let hasComplex = false;
  let needsRewrite = false;
  for (let i = 0; i < len; i++) {
    const item = raw[i];
    if (item === null) continue;
    const it = typeof item;
    if (it === "string") continue;
    if (it === "number" || it === "boolean" || it === "undefined") {
      needsRewrite = true;
      continue;
    }
    hasComplex = true;
    if (item.constructor !== BudNode) {
      needsRewrite = true;
    }
  }

  if (!hasComplex) {
    let text = "";
    for (let i = 0; i < len; i++) {
      const item = raw[i];
      if (item != null && typeof item !== "boolean") text += item;
    }
    if (text.length > 0) {
      if (isFragment) node.children = [text];
      else node.text = text;
    }
    return;
  }

  if (!needsRewrite) {
    node.children = raw;
    return;
  }

  // Preserve static child indices with `null` holes so toggling {cond && <A/>} never shifts sibling state
  const out = new Array(len);
  for (let i = 0; i < len; i++) {
    const item = raw[i];
    if (item == null || typeof item === "boolean") {
      out[i] = null;
    } else if (typeof item === "string" || typeof item === "number") {
      out[i] = "" + item;
    } else if (Array.isArray(item)) {
      out[i] = makeImplicitFragment(item);
    } else if (item.constructor === BudNode) {
      out[i] = item;
    } else {
      out[i] = null;
    }
  }
  node.children = out;
}

export function jsx(type, props, key) {
  const p = props || EMPTY_OBJ;
  const node = new BudNode(type, null, EMPTY_ARR, p, key);

  if (typeof type === "function") {
    return node;
  }

  if (type === Fragment) {
    node.isFragment = true;
    populateChildren(node, p.children, true);
    return node;
  }

  const ch = p.children;
  if (ch !== undefined) populateChildren(node, ch, false);
  return node;
}

export const jsxs = jsx;
export const jsxDEV = jsx;

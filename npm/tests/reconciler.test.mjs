import test from "node:test";
import assert from "node:assert/strict";
import {
  jsx,
  jsxs,
  Fragment,
  render,
  flushSync,
  useState,
  useReducer,
  useRef,
  useMemo,
  useEffect,
  memo,
} from "../jsx-runtime/index.js";
import { createDomEnv } from "./dom-stub.mjs";

test("Multiple instances of the same component maintain isolated hook state", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  function LikeButton({ initial }) {
    const [likes, setLikes] = useState(initial);
    return jsx("button", {
      onClick: () => setLikes((c) => c + 1),
      children: ["♥ ", likes],
    });
  }

  function App() {
    return jsxs("div", {
      children: [
        jsx(LikeButton, { initial: 10 }),
        jsx(LikeButton, { initial: 99 }),
      ],
    });
  }

  render(jsx(App, {}), root);
  const [btnA, btnB] = root.firstChild.childNodes;
  assert.equal(btnA.textContent, "♥ 10");
  assert.equal(btnB.textContent, "♥ 99");

  btnA.dispatchEvent({ type: "click" });
  flushSync();

  assert.equal(btnA.textContent, "♥ 11");
  assert.equal(btnB.textContent, "♥ 99");
  assert.equal(root.firstChild.childNodes[0], btnA);
  assert.equal(root.firstChild.childNodes[1], btnB);
});

test("Subtree-scoped updates: child setState does NOT re-render parent or siblings", () => {
  const { createContainer, stats, resetStats } = createDomEnv();
  const root = createContainer();

  let parentRenders = 0;
  let siblingRenders = 0;
  let leafRenders = 0;

  function Leaf() {
    leafRenders++;
    const [n, setN] = useState(0);
    return jsx("button", {
      onClick: () => setN((x) => x + 1),
      children: ["Count: ", n],
    });
  }

  function Sibling() {
    siblingRenders++;
    return jsx("span", { children: "static" });
  }

  function Parent() {
    parentRenders++;
    return jsxs("div", {
      children: [jsx(Leaf, {}), jsx(Sibling, {})],
    });
  }

  render(jsx(Parent, {}), root);
  assert.equal(parentRenders, 1);
  assert.equal(siblingRenders, 1);
  assert.equal(leafRenders, 1);

  const btn = root.firstChild.childNodes[0];
  resetStats();

  // Multiple setState calls in one tick batch into a single leaf render + 1 text nodeValue write
  btn.dispatchEvent({ type: "click" });
  btn.dispatchEvent({ type: "click" });
  flushSync();

  assert.equal(parentRenders, 1);
  assert.equal(siblingRenders, 1);
  assert.equal(leafRenders, 2);
  assert.equal(btn.textContent, "Count: 2");
  assert.equal(stats.createElement, 0);
  assert.equal(stats.nodeValueSet, 1);
});

test("Conditional {flag && <Banner />} preserves following sibling state & DOM node", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  let toggleBanner;
  function Counter() {
    const [n, setN] = useState(5);
    return jsx("button", {
      onClick: () => setN((x) => x + 1),
      children: String(n),
    });
  }

  function App() {
    const [show, setShow] = useState(false);
    toggleBanner = () => setShow((s) => !s);
    return jsxs("div", {
      children: [
        show && jsx("p", { className: "banner", children: "Hello" }),
        jsx(Counter, {}),
      ],
    });
  }

  render(jsx(App, {}), root);
  const counterBtn = root.firstChild.childNodes[0];
  assert.equal(counterBtn.textContent, "5");
  counterBtn.dispatchEvent({ type: "click" });
  flushSync();
  assert.equal(counterBtn.textContent, "6");

  // Show banner -> inserted BEFORE counterBtn; counterBtn keeps identity and state=6!
  toggleBanner();
  flushSync();
  assert.equal(root.firstChild.childNodes.length, 2);
  assert.equal(root.firstChild.childNodes[0].localName, "p");
  assert.equal(root.firstChild.childNodes[1], counterBtn);
  assert.equal(counterBtn.textContent, "6");

  // Hide banner -> counterBtn still untouched with state=6!
  toggleBanner();
  flushSync();
  assert.equal(root.firstChild.childNodes.length, 1);
  assert.equal(root.firstChild.childNodes[0], counterBtn);
  assert.equal(counterBtn.textContent, "6");
});

test("Component toggling null <-> Element reinserts at exact sibling position", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  let toggleMid;
  function Middle() {
    const [on, setOn] = useState(false);
    toggleMid = () => setOn((v) => !v);
    return on ? jsx("span", { children: "MID" }) : null;
  }

  function App() {
    return jsxs("div", {
      children: [
        jsx("span", { children: "FIRST" }),
        jsx(Middle, {}),
        jsx("span", { children: "LAST" }),
      ],
    });
  }

  render(jsx(App, {}), root);
  assert.equal(root.firstChild.textContent, "FIRSTLAST");

  toggleMid();
  flushSync();
  assert.equal(root.firstChild.childNodes.length, 3);
  assert.equal(root.firstChild.childNodes[0].textContent, "FIRST");
  assert.equal(root.firstChild.childNodes[1].textContent, "MID");
  assert.equal(root.firstChild.childNodes[2].textContent, "LAST");

  toggleMid();
  flushSync();
  assert.equal(root.firstChild.textContent, "FIRSTLAST");
});

test("In-place DOM patching preserves <input> focus, selectionStart/End, and uncontrolled values", () => {
  const { doc, createContainer } = createDomEnv();
  const root = createContainer();

  function SearchBox() {
    const [q, setQ] = useState("bow");
    return jsxs("div", {
      children: [
        jsx("input", {
          type: "text",
          value: q,
          onChange: (e) => setQ(e.target.value),
        }),
        jsx("input", { type: "text", placeholder: "uncontrolled" }),
        jsx("span", { children: q }),
      ],
    });
  }

  render(jsx(SearchBox, {}), root);
  const [controlled, uncontrolled, span] = root.firstChild.childNodes;

  // Type into uncontrolled input
  uncontrolled.value = "keep-me";

  // Focus controlled input and place caret in middle
  controlled.focus();
  controlled._value = "bowie";
  controlled.setSelectionRange(2, 2);
  controlled.dispatchEvent({ type: "input" });
  flushSync();

  // Controlled input kept exact DOM identity, focus, and caret position (because dom.value === "bowie")
  assert.equal(root.firstChild.childNodes[0], controlled);
  assert.equal(doc.activeElement, controlled);
  assert.equal(controlled.selectionStart, 2);
  assert.equal(controlled.selectionEnd, 2);
  assert.equal(span.textContent, "bowie");

  // Uncontrolled input was NOT wiped!
  assert.equal(uncontrolled.value, "keep-me");
});

test("Keyed LIS reorder performs minimal insertBefore moves AND rescues focus & selection", () => {
  const { doc, createContainer, stats, resetStats } = createDomEnv();
  const root = createContainer();

  let setItems;
  function List() {
    const [items, _setItems] = useState([
      { id: 1, label: "A" },
      { id: 2, label: "B" },
      { id: 3, label: "C" },
      { id: 4, label: "D" },
      { id: 5, label: "E" },
    ]);
    setItems = _setItems;
    return jsx("ul", {
      children: items.map((it) =>
        jsxs("li", {
          children: [
            jsx("span", { children: it.label }),
            jsx("input", { type: "text", value: it.label }),
          ],
        }, it.id)
      ),
    });
  }

  render(jsx(List, {}), root);
  const ul = root.firstChild;
  const origNodes = [...ul.childNodes];
  const inputA = origNodes[0].childNodes[1];

  // Focus input inside row A (which will be moved to the end!)
  inputA.focus();
  inputA.setSelectionRange(0, 1);

  // Swap first (1) and last (5): middle rows B, C, D form LIS -> only 2 moves needed!
  resetStats();
  setItems([
    { id: 5, label: "E" },
    { id: 2, label: "B" },
    { id: 3, label: "C" },
    { id: 4, label: "D" },
    { id: 1, label: "A" },
  ]);
  flushSync();

  assert.equal(stats.createElement, 0);
  assert.equal(stats.removeChild, 0);
  assert.equal(stats.insertBefore, 2);
  assert.equal(ul.childNodes[0], origNodes[4]);
  assert.equal(ul.childNodes[1], origNodes[1]);
  assert.equal(ul.childNodes[2], origNodes[2]);
  assert.equal(ul.childNodes[3], origNodes[3]);
  assert.equal(ul.childNodes[4], origNodes[0]);

  // Focus & caret rescued even though row A was moved via insertBefore!
  assert.equal(doc.activeElement, inputA);
  assert.equal(inputA.selectionStart, 0);
  assert.equal(inputA.selectionEnd, 1);
});

test("O(1) bulk clear uses single textContent='' when subtree has no cleanups", () => {
  const { createContainer, stats, resetStats } = createDomEnv();
  const root = createContainer();

  let setRows;
  function Table() {
    const [rows, _setRows] = useState(Array.from({ length: 100 }, (_, i) => i));
    setRows = _setRows;
    return jsx("ul", {
      children: rows.map((r) => jsx("li", { children: `Row ${r}` }, r)),
    });
  }

  render(jsx(Table, {}), root);
  assert.equal(root.firstChild.childNodes.length, 100);

  resetStats();
  setRows([]);
  flushSync();

  assert.equal(root.firstChild.childNodes.length, 0);
  assert.equal(stats.removeChild, 0);
  assert.equal(stats.textContentSet, 1);
});

test("useEffect runs post-commit with cleanups, useRef/ref attaches & detaches, memo bails out", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  const log = [];
  let memoRenders = 0;
  let setVal;

  const PureLabel = memo(function PureLabel({ text }) {
    memoRenders++;
    return jsx("em", { children: text });
  });

  function Demo() {
    const [val, _setVal] = useState(1);
    const [other, setOther] = useState(0);
    const elRef = useRef(null);
    const doubled = useMemo(() => val * 2, [val]);
    const [reduced, dispatch] = useReducer((s, a) => s + a, 10);
    setVal = (v) => {
      _setVal(v);
      dispatch(1);
      setOther((o) => o + 1);
    };

    useEffect(() => {
      log.push(`mount:${val}:${Boolean(elRef.current)}`);
      return () => log.push(`clean:${val}`);
    }, [val]);

    return jsxs("div", {
      ref: elRef,
      children: [
        jsx("span", { children: `${doubled}-${reduced}-${other}` }),
        jsx(PureLabel, { text: "fixed" }),
      ],
    });
  }

  render(jsx(Demo, {}), root);
  assert.deepEqual(log, ["mount:1:true"]);
  assert.equal(memoRenders, 1);

  setVal(2);
  flushSync();
  assert.deepEqual(log, ["mount:1:true", "clean:1", "mount:2:true"]);
  // PureLabel props unchanged -> memo bailed out!
  assert.equal(memoRenders, 1);

  // Unmount via render(null, root)
  render(null, root);
  assert.deepEqual(log, ["mount:1:true", "clean:1", "mount:2:true", "clean:2"]);
  assert.equal(root.childNodes.length, 0);
});

test("<select value> syncs after options mount, SVG uses createElementNS, SSR escapes HTML & void tags", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  const tree = jsxs("div", {
    children: [
      jsxs("select", {
        value: "b",
        children: [
          jsx("option", { value: "a", children: "A" }),
          jsx("option", { value: "b", children: "B" }),
        ],
      }),
      jsx("svg", {
        viewBox: "0 0 10 10",
        children: jsx("path", { d: "M0 0" }),
      }),
      jsx("input", { type: "text", value: 'a"b' }),
      jsx("p", { children: "1 < 2 & 3 > 2" }),
    ],
  });

  render(tree, root);
  const [select, svg] = root.firstChild.childNodes;
  assert.equal(select.value, "b");
  assert.equal(svg.namespaceURI, "http://www.w3.org/2000/svg");
  assert.equal(svg.firstChild.namespaceURI, "http://www.w3.org/2000/svg");

  // SSR escaping & void tag check
  const html = tree.render();
  assert.ok(html.includes('<input type="text" value="a&quot;b">'));
  assert.ok(!html.includes("</input>"));
  assert.ok(html.includes("<p>1 &lt; 2 &amp; 3 &gt; 2</p>"));
});

test("Ancestor + child dirty in same tick renders cleanly once without stale double-render", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  let childRenders = 0;
  let triggerBoth;

  function Child({ prefix }) {
    childRenders++;
    const [n, setN] = useState(0);
    if (!triggerBoth) {
      triggerBoth = (setP) => {
        setN(1);
        setP("B");
      };
    }
    return jsx("span", { children: `${prefix}:${n}` });
  }

  function Parent() {
    const [prefix, setPrefix] = useState("A");
    return jsx("div", {
      onClick: () => triggerBoth(setPrefix),
      children: jsx(Child, { prefix }),
    });
  }

  render(jsx(Parent, {}), root);
  assert.equal(childRenders, 1);
  assert.equal(root.firstChild.textContent, "A:0");

  root.firstChild.dispatchEvent({ type: "click" });
  flushSync();

  assert.equal(childRenders, 2);
  assert.equal(root.firstChild.textContent, "B:1");
});

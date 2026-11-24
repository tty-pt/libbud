import test from "node:test";
import assert from "node:assert/strict";
import { jsx, render } from "../jsx-runtime/index.js";
import { createDomEnv } from "./dom-stub.mjs";

test("JSX dispatches events via direct _budEv proxy with zero data-bid churn or re-binds", () => {
  const { createContainer, stats, resetStats } = createDomEnv();
  const root = createContainer();

  let clicks = 0;
  const node1 = jsx("button", {
    className: "btn",
    onClick: () => { clicks += 1; },
    children: "Click Me"
  });

  // SSR HTML has zero synthetic data-bid noise
  const html = node1.render();
  assert.equal(html, '<button class="btn">Click Me</button>');

  render(node1, root);
  const btn = root.firstChild;
  assert.equal(stats.addEventListener, 1);

  btn.dispatchEvent({ type: "click" });
  assert.equal(clicks, 1);

  // Re-render with a brand-new closure: 0 addEventListener, 0 removeEventListener, same DOM node!
  resetStats();
  render(
    jsx("button", {
      className: "btn",
      onClick: () => { clicks += 10; },
      children: "Click Me"
    }),
    root
  );
  assert.equal(root.firstChild, btn);
  assert.equal(stats.addEventListener, 0);
  assert.equal(stats.removeEventListener, 0);
  assert.equal(stats.setAttribute, 0);

  btn.dispatchEvent({ type: "click" });
  assert.equal(clicks, 11);
});

test("onChange on text input maps to native input event and onDoubleClick maps to dblclick", () => {
  const { createContainer } = createDomEnv();
  const root = createContainer();

  let latest = "";
  let dbl = 0;

  render(
    jsx("div", {
      children: [
        jsx("input", {
          type: "text",
          onChange: (e) => { latest = e.target.value; }
        }),
        jsx("button", {
          onDoubleClick: () => { dbl++; },
          children: "Dbl"
        })
      ]
    }),
    root
  );

  const input = root.firstChild.childNodes[0];
  const btn = root.firstChild.childNodes[1];

  input.value = "bowie";
  input.dispatchEvent({ type: "input" });
  assert.equal(latest, "bowie");

  btn.dispatchEvent({ type: "dblclick" });
  assert.equal(dbl, 1);
});

test("Root event delegation attaches 1 listener for 100 buttons, honors stopPropagation, and keeps non-bubbling events direct", () => {
  const { createContainer, stats } = createDomEnv();
  const root = createContainer();

  const log = [];
  let focused = 0;

  render(
    jsx("div", {
      onClick: () => log.push("parent"),
      children: [
        jsx("input", {
          type: "text",
          onFocus: () => { focused++; },
        }),
        ...Array.from({ length: 100 }, (_, i) =>
          jsx("button", {
            onClick: (e) => {
              log.push(`btn:${i}`);
              if (i === 1) e.stopPropagation();
            },
            children: String(i),
          }, i)
        ),
      ],
    }),
    root
  );

  // 1 delegated 'click' listener on root + 1 direct 'focus' listener on <input> (NOT 101!)
  assert.equal(stats.addEventListener, 2);

  const div = root.firstChild;
  const input = div.childNodes[0];
  const btn0 = div.childNodes[1];
  const btn1 = div.childNodes[2];

  // Normal click bubbles child -> parent
  btn0.dispatchEvent({ type: "click" });
  assert.deepEqual(log, ["btn:0", "parent"]);

  // stopPropagation stops parent handler
  log.length = 0;
  btn1.dispatchEvent({ type: "click" });
  assert.deepEqual(log, ["btn:1"]);

  // Non-bubbling focus fires directly on input
  input.dispatchEvent({ type: "focus", bubbles: false });
  assert.equal(focused, 1);
});

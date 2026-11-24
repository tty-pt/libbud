import test from "node:test";
import assert from "node:assert/strict";
import { jsx, jsxs, Fragment, useState, setRootScheduler } from "../jsx-runtime/index.js";
import { diffVdom, BudNode } from "../dist/index.js";

test("useState maintains state across component re-renders", () => {
  let triggerIncrement = () => {};
  let renderCount = 0;

  function Counter() {
    renderCount++;
    const [count, setCount] = useState(0);
    triggerIncrement = () => setCount((prev) => prev + 1);

    return jsx("button", {
      className: "counter",
      children: `Count: ${count}`
    });
  }

  // 1. Initial Render
  const tree1 = jsx(Counter, {});
  assert.equal(tree1.render(), '<button class="counter">Count: 0</button>');

  // 2. Trigger state change
  let scheduleCount = 0;
  setRootScheduler(() => {
    scheduleCount++;
  });

  triggerIncrement();
  assert.equal(scheduleCount, 1);

  // 3. Re-render
  const tree2 = jsx(Counter, {});
  assert.equal(tree2.render(), '<button class="counter">Count: 1</button>');

  // 4. Verify diff
  const patches = diffVdom(tree1, tree2);
  assert.ok(patches.length > 0);
  assert.equal(renderCount, 2);
});

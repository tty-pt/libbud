import test from "node:test";
import assert from "node:assert/strict";
import { h, diffVdom, version } from "../dist/index.js";

test("BudNode tree creation and HTML rendering", () => {
  const tree = h("div", { class: "container" }, [
    h("h1", {}, ["Title"]),
    h("button", { type: "submit" }, ["Click Me"])
  ]);

  const html = tree.render();
  assert.ok(html.includes('<div class="container">'));
  assert.ok(html.includes("<h1>Title</h1>"));
  assert.ok(html.includes('<button type="submit">Click Me</button>'));
});

test("diffVdom generates patch operations", () => {
  const nodeA = h("p", {}, []).setText("Hello");
  const nodeB = h("p", {}, []).setText("World");

  const patches = diffVdom(nodeA, nodeB);
  assert.equal(patches.length, 1);
  assert.ok(patches[0].type === "patch-text" || patches[0].type === "SET_TEXT");
  assert.equal(patches[0].a, "World");

  assert.ok(version().length > 0);
});

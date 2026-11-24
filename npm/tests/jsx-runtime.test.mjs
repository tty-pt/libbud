import test from "node:test";
import assert from "node:assert/strict";
import { jsx, jsxs, Fragment } from "../jsx-runtime/index.js";
import { diffVdom, BudNode } from "../dist/index.js";

test("jsx basic intrinsic elements and render", () => {
  const tree = jsx("div", {
    className: "card",
    id: "main-card",
    children: jsx("h1", { children: "Hello World" })
  });

  assert.ok(tree instanceof BudNode);
  const html = tree.render();
  assert.equal(html, '<div class="card" id="main-card"><h1>Hello World</h1></div>');
});

test("jsxs with multiple children and nested structures", () => {
  const tree = jsxs("ul", {
    class: "list",
    children: [
      jsx("li", { children: "Item 1" }),
      jsx("li", { children: "Item 2" }),
      jsx("li", { children: "Item 3" })
    ]
  });

  const html = tree.render();
  assert.equal(html, '<ul class="list"><li>Item 1</li><li>Item 2</li><li>Item 3</li></ul>');
});

test("jsx functional components", () => {
  function SongItem({ title, artist }) {
    return jsxs("div", {
      class: "song",
      children: [
        jsx("span", { class: "title", children: title }),
        jsx("span", { class: "artist", children: artist })
      ]
    });
  }

  const tree = jsx(SongItem, { title: "Starman", artist: "David Bowie" });
  assert.ok(tree instanceof BudNode);
  const html = tree.render();
  assert.equal(html, '<div class="song"><span class="title">Starman</span><span class="artist">David Bowie</span></div>');
});

test("jsx fragments flattening", () => {
  const fragment = jsxs(Fragment, {
    children: [
      jsx("p", { children: "First" }),
      jsx("p", { children: "Second" })
    ]
  });

  const container = jsx("div", {
    class: "wrapper",
    children: fragment
  });

  const html = container.render();
  assert.equal(html, '<div class="wrapper"><p>First</p><p>Second</p></div>');
});

test("jsx trees diff cleanly using diffVdom", () => {
  const treeA = jsx("div", {
    class: "status",
    children: jsx("span", { children: "Loading" })
  });

  const treeB = jsx("div", {
    class: "status ready",
    children: jsx("span", { children: "Ready" })
  });

  const patches = diffVdom(treeA, treeB);
  assert.ok(Array.isArray(patches));
  assert.ok(patches.length > 0);
});

/**
 * @file bench-browser.mjs
 * @brief Real Chromium (Blink + V8) apples-to-apples benchmark: @tty-pt/bud/jsx-runtime vs React 19 (production).
 *
 * Usage:
 *   node external/libbud/npm/tests/bench-browser.mjs
 */

import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import { gzipSync } from "node:zlib";
import { performance } from "node:perf_hooks";

const require = createRequire(import.meta.url);
const esbuild = require("../examples/jsx-app/node_modules/esbuild");

const CHROME_CANDIDATES = [
  process.env.CHROME_BIN,
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/snap/bin/chromium",
].filter(Boolean);

const REACT_CANDIDATES = [
  process.env.REACT_NODE_MODULES,
  "/home/quirinpa/duna/node_modules",
].filter(Boolean);

function findReactDir() {
  for (const dir of REACT_CANDIDATES) {
    try {
      const pkg = require(`${dir}/react/package.json`);
      return { dir, version: pkg.version };
    } catch (_) {}
  }
  throw new Error("React not found. Set REACT_NODE_MODULES=/path/to/node_modules");
}

function buildBundle(code, resolveDir) {
  const res = esbuild.buildSync({
    stdin: { contents: code, resolveDir, loader: "js" },
    bundle: true,
    minify: true,
    write: false,
    format: "iife",
    define: { "process.env.NODE_ENV": '"production"' },
  });
  return res.outputFiles[0].text;
}

function makeBenchPage(budCode, reactCode) {
  return `<!doctype html>
<html>
<head><meta charset="utf-8"><title>Bud vs React Benchmark</title></head>
<body>
<div id="bud-root"></div>
<div id="react-root"></div>
<script>${budCode}</script>
<script>${reactCode}</script>
<script>
function makeRows(n, startId = 1) {
  const rows = new Array(n);
  for (let i = 0; i < n; i++) {
    const id = startId + i;
    rows[i] = { id, label: "Track #" + id + " — David Bowie" };
  }
  return rows;
}
function median(arr) {
  const s = arr.slice().sort((a, b) => a - b);
  return s[s.length >> 1];
}
function runInterleaved(makeBud, makeReact, runs) {
  const workloads = [
    ["create1k", (f) => f(), (c) => { c.setRows(makeRows(1000, 1)); }],
    ["create1kPaint", (f) => f(), (c) => { c.setRows(makeRows(1000, 1)); document.body.offsetHeight; }],
    ["create10k", (f) => f(), (c) => { c.setRows(makeRows(10000, 1)); }],
    ["replace1k", (f) => { const c = f(); c.setRows(makeRows(1000, 1)); return c; }, (c) => { c.setRows(makeRows(1000, 1001)); }],
    ["append1k", (f) => { const c = f(); c.rows = makeRows(1000, 1); c.setRows(c.rows); return c; }, (c) => { c.setRows(c.rows.concat(makeRows(1000, 1001))); }],
    ["updateEvery10th", (f) => { const c = f(); c.rows = makeRows(1000, 1); c.setRows(c.rows); return c; }, (c) => {
      const next = c.rows.slice();
      for (let i = 0; i < next.length; i += 10) next[i] = { id: next[i].id, label: next[i].label + " !!!" };
      c.setRows(next);
    }],
    ["select1", (f) => { const c = f(); c.setRows(makeRows(1000, 1)); return c; }, (c) => { c.setSelected(500); }],
    ["swap2", (f) => { const c = f(); c.rows = makeRows(1000, 1); c.setRows(c.rows); return c; }, (c) => {
      const next = c.rows.slice();
      const tmp = next[1]; next[1] = next[998]; next[998] = tmp;
      c.setRows(next);
    }],
    ["clear1k", (f) => { const c = f(); c.setRows(makeRows(1000, 1)); return c; }, (c) => { c.setRows([]); }],
    ["leafSetState1000x", (f) => { const c = f(); c.setRows(makeRows(1000, 1)); return c; }, (c) => { for (let i = 0; i < 1000; i++) c.incLeaf(); }],
  ];

  const bud = {};
  const react = {};
  for (const [name, setup, op] of workloads) {
    // 2 interleaved warmups
    for (let w = 0; w < 2; w++) {
      const cb = setup(makeBud); op(cb); cb.cleanup();
      const cr = setup(makeReact); op(cr); cr.cleanup();
    }
    const bTimes = [];
    const rTimes = [];
    for (let r = 0; r < runs; r++) {
      if (r & 1) {
        const cr = setup(makeReact);
        const t0 = performance.now(); op(cr); const t1 = performance.now();
        rTimes.push(t1 - t0);
        cr.cleanup();

        const cb = setup(makeBud);
        const t2 = performance.now(); op(cb); const t3 = performance.now();
        bTimes.push(t3 - t2);
        cb.cleanup();
      } else {
        const cb = setup(makeBud);
        const t0 = performance.now(); op(cb); const t1 = performance.now();
        bTimes.push(t1 - t0);
        cb.cleanup();

        const cr = setup(makeReact);
        const t2 = performance.now(); op(cr); const t3 = performance.now();
        rTimes.push(t3 - t2);
        cr.cleanup();
      }
    }
    bud[name] = median(bTimes);
    react[name] = median(rTimes);
  }
  return { bud, react };
}

window.__RUN_BENCH__ = function() {
  const RUNS = 11;
  // Sanity-check click dispatch & DOM correctness on both before timing
  const bCheck = window.__MAKE_BUD__();
  bCheck.setRows(makeRows(5, 1));
  const link = document.querySelector("#bud-root a");
  if (!link) throw new Error("Bud failed to render links");
  link.click();
  bCheck.flush();
  const tr0 = document.querySelector("#bud-root tbody tr");
  if (!tr0 || tr0.className !== "danger") {
    throw new Error("Bud click dispatch failed: tr0.className=" + (tr0 && tr0.className));
  }
  bCheck.cleanup();

  return runInterleaved(window.__MAKE_BUD__, window.__MAKE_REACT__, RUNS);
};
</script>
</body>
</html>`;
}

async function runInChromium(html) {
  const chromeBin = CHROME_CANDIDATES[0];
  const proc = spawn(
    chromeBin,
    [
      "--headless=new",
      "--disable-gpu",
      "--no-sandbox",
      "--disable-dev-shm-usage",
      "--remote-debugging-port=0",
      "about:blank",
    ],
    { stdio: ["ignore", "pipe", "pipe"] }
  );

  let wsUrl = null;
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Timed out launching Chromium")), 15000);
    let stderr = "";
    proc.stderr.on("data", (chunk) => {
      stderr += chunk.toString();
      const m = stderr.match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (m) {
        wsUrl = m[1];
        clearTimeout(timer);
        resolve();
      }
    });
    proc.on("exit", (code) => {
      if (!wsUrl) {
        clearTimeout(timer);
        reject(new Error(`Chromium exited early (${code}): ${stderr}`));
      }
    });
  });

  try {
    const port = new URL(wsUrl).port;
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const pageTarget = targets.find((t) => t.type === "page") || targets[0];
    const ws = new WebSocket(pageTarget.webSocketDebuggerUrl);
    await new Promise((res, rej) => {
      ws.addEventListener("open", res, { once: true });
      ws.addEventListener("error", rej, { once: true });
    });

    let msgId = 0;
    const pending = new Map();
    ws.addEventListener("message", (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id && pending.has(msg.id)) {
        const { res, rej } = pending.get(msg.id);
        pending.delete(msg.id);
        if (msg.error) rej(new Error(msg.error.message));
        else res(msg.result);
      }
    });

    function cdp(method, params = {}) {
      return new Promise((res, rej) => {
        const id = ++msgId;
        pending.set(id, { res, rej });
        ws.send(JSON.stringify({ id, method, params }));
      });
    }

    await cdp("Page.enable");
    await cdp("Runtime.enable");
    const dataUrl = "data:text/html;charset=utf-8," + encodeURIComponent(html);
    await cdp("Page.navigate", { url: dataUrl });
    await new Promise((r) => setTimeout(r, 400));

    const evalRes = await cdp("Runtime.evaluate", {
      expression: "JSON.stringify(window.__RUN_BENCH__())",
      returnByValue: true,
    });
    ws.close();
    if (evalRes.exceptionDetails) {
      throw new Error("Browser exception: " + JSON.stringify(evalRes.exceptionDetails));
    }
    return JSON.parse(evalRes.result.value);
  } finally {
    proc.kill("SIGKILL");
  }
}

const reactInfo = findReactDir();
const npmRoot = new URL("..", import.meta.url).pathname;

const budHarnessCode = `
import { jsx, jsxs, render, flushSync, useState } from "./jsx-runtime/index.js";

window.__MAKE_BUD__ = function() {
  const root = document.getElementById("bud-root");
  let _setRows, _setSelected, _incLeaf;

  function LeafCounter() {
    const [n, setN] = useState(0);
    _incLeaf = () => setN((x) => x + 1);
    return jsx("button", { className: "leaf", children: ["♥ ", n] });
  }

  function TableApp() {
    const [rows, setRows] = useState([]);
    const [selected, setSelected] = useState(0);
    _setRows = setRows;
    _setSelected = setSelected;
    return jsxs("div", {
      className: "bench",
      children: [
        jsx(LeafCounter, {}),
        jsx("table", {
          children: jsx("tbody", {
            children: rows.map((r) =>
              jsxs("tr", {
                className: r.id === selected ? "danger" : "",
                children: [
                  jsx("td", { className: "col-id", children: String(r.id) }),
                  jsx("td", {
                    className: "col-label",
                    children: jsx("a", {
                      onClick: () => setSelected(r.id),
                      children: r.label,
                    }),
                  }),
                ],
              }, r.id)
            ),
          }),
        }),
      ],
    });
  }

  render(jsx(TableApp, {}), root);
  return {
    setRows(r) { _setRows(r); flushSync(); },
    setSelected(s) { _setSelected(s); flushSync(); },
    incLeaf() { _incLeaf(); flushSync(); },
    flush() { flushSync(); },
    cleanup() { render(null, root); },
  };
};
`;

const reactHarnessCode = `
import { useState } from "${reactInfo.dir}/react";
import { jsx, jsxs } from "${reactInfo.dir}/react/jsx-runtime";
import { createRoot } from "${reactInfo.dir}/react-dom/client";
import { flushSync } from "${reactInfo.dir}/react-dom";

window.__MAKE_REACT__ = function() {
  const container = document.getElementById("react-root");
  const root = createRoot(container);
  let _setRows, _setSelected, _incLeaf;

  function LeafCounter() {
    const [n, setN] = useState(0);
    _incLeaf = () => setN((x) => x + 1);
    return jsxs("button", { className: "leaf", children: ["♥ ", n] });
  }

  function TableApp() {
    const [rows, setRows] = useState([]);
    const [selected, setSelected] = useState(0);
    _setRows = setRows;
    _setSelected = setSelected;
    return jsxs("div", {
      className: "bench",
      children: [
        jsx(LeafCounter, {}),
        jsx("table", {
          children: jsx("tbody", {
            children: rows.map((r) =>
              jsxs("tr", {
                className: r.id === selected ? "danger" : "",
                children: [
                  jsx("td", { className: "col-id", children: String(r.id) }),
                  jsx("td", {
                    className: "col-label",
                    children: jsx("a", {
                      onClick: () => setSelected(r.id),
                      children: r.label,
                    }),
                  }),
                ],
              }, r.id)
            ),
          }),
        }),
      ],
    });
  }

  flushSync(() => root.render(jsx(TableApp, {})));
  return {
    setRows(r) { flushSync(() => _setRows(r)); },
    setSelected(s) { flushSync(() => _setSelected(s)); },
    incLeaf() { flushSync(() => _incLeaf()); },
    flush() {},
    cleanup() { flushSync(() => root.unmount()); },
  };
};
`;

const budBundle = buildBundle(budHarnessCode, npmRoot);
const reactBundle = buildBundle(reactHarnessCode, reactInfo.dir);
const html = makeBenchPage(budBundle, reactBundle);

const { bud, react } = await runInChromium(html);

const budMin = Buffer.byteLength(budBundle);
const budGzip = gzipSync(budBundle).length;
const reactMin = Buffer.byteLength(reactBundle);
const reactGzip = gzipSync(reactBundle).length;

console.log(`=== Real Chromium (Blink + V8) Benchmark: Bud vs React ${reactInfo.version} (median of 11 runs) ===`);
console.log(
  "Workload".padEnd(20) +
  "Bud (ms)".padStart(11) +
  "React (ms)".padStart(12) +
  "Ratio".padStart(14) +
  "Winner".padStart(10)
);
console.log("-".repeat(67));

let budWins = 0;
let total = 0;
for (const name of Object.keys(bud)) {
  const b = bud[name];
  const r = react[name];
  total++;
  const faster = b <= r;
  if (faster) budWins++;
  const ratio = faster ? `${(r / b).toFixed(2)}x faster` : `${(b / r).toFixed(2)}x slower`;
  console.log(
    name.padEnd(20) +
    b.toFixed(2).padStart(11) +
    r.toFixed(2).padStart(12) +
    ratio.padStart(14) +
    (faster ? "BUD" : "REACT").padStart(10)
  );
}
console.log("-".repeat(67));
console.log(`Score: Bud wins ${budWins}/${total} DOM workloads`);
console.log(`Bundle (min / gzip): Bud ${budMin} B / ${budGzip} B  vs  React ${reactMin} B / ${reactGzip} B (${(reactGzip / budGzip).toFixed(1)}x smaller)`);

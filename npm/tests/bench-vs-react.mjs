/**
 * @file bench-vs-react.mjs
 * @brief Deterministic head-to-head benchmark: @tty-pt/bud/jsx-runtime vs React 19 (production).
 *
 * Usage:
 *   NODE_ENV=production node external/libbud/npm/tests/bench-vs-react.mjs
 */

import { performance } from "node:perf_hooks";
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { gzipSync } from "node:zlib";
import {
  jsx,
  jsxs,
  render as budRender,
  flushSync as budFlushSync,
  useState as budUseState,
} from "../jsx-runtime/index.js";
import { createDomEnv } from "./dom-stub.mjs";

const require = createRequire(import.meta.url);

function makeRows(n, startId = 1) {
  const rows = new Array(n);
  for (let i = 0; i < n; i++) {
    const id = startId + i;
    rows[i] = { id, label: `Track #${id} — David Bowie` };
  }
  return rows;
}

function median(arr) {
  const s = [...arr].sort((a, b) => a - b);
  return s[s.length >> 1];
}

function measure(runs, setupFn, opFn) {
  const times = [];
  let lastOps = null;
  for (let r = 0; r < runs; r++) {
    const ctx = setupFn();
    ctx.resetStats();
    const t0 = performance.now();
    opFn(ctx);
    const t1 = performance.now();
    times.push(t1 - t0);
    lastOps = { ...ctx.stats };
  }
  return { ms: median(times), ops: lastOps };
}

function runBudBenchmarks() {
  const RUNS = 9;

  function makeApp() {
    const env = createDomEnv();
    const root = env.createContainer();
    let api = {};

    function LeafCounter() {
      const [n, setN] = budUseState(0);
      api.incLeaf = () => setN((x) => x + 1);
      return jsx("button", { className: "leaf", children: ["♥ ", n] });
    }

    function TableApp() {
      const [rows, setRows] = budUseState([]);
      const [selected, setSelected] = budUseState(0);
      api.setRows = setRows;
      api.setSelected = setSelected;
      return jsxs("div", {
        className: "bench",
        children: [
          jsx(LeafCounter, {}),
          jsx("table", {
            children: jsx("tbody", {
              children: rows.map((r) =>
                jsxs(
                  "tr",
                  {
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
                  },
                  r.id
                )
              ),
            }),
          }),
        ],
      });
    }

    budRender(jsx(TableApp, {}), root);
    return { ...env, root, api };
  }

  const results = {};

  results.create1k = measure(RUNS, makeApp, ({ api }) => {
    api.setRows(makeRows(1000, 1));
    budFlushSync();
  });

  results.replace1k = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.api.setRows(makeRows(1000, 1));
      budFlushSync();
      return ctx;
    },
    ({ api }) => {
      api.setRows(makeRows(1000, 1001));
      budFlushSync();
    }
  );

  results.updateEvery10th = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.rows = makeRows(1000, 1);
      ctx.api.setRows(ctx.rows);
      budFlushSync();
      return ctx;
    },
    ({ api, rows }) => {
      const next = rows.slice();
      for (let i = 0; i < next.length; i += 10) {
        next[i] = { id: next[i].id, label: next[i].label + " !!!" };
      }
      api.setRows(next);
      budFlushSync();
    }
  );

  results.swap2 = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.rows = makeRows(1000, 1);
      ctx.api.setRows(ctx.rows);
      budFlushSync();
      return ctx;
    },
    ({ api, rows }) => {
      const next = rows.slice();
      const tmp = next[1];
      next[1] = next[998];
      next[998] = tmp;
      api.setRows(next);
      budFlushSync();
    }
  );

  results.select1 = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.api.setRows(makeRows(1000, 1));
      budFlushSync();
      return ctx;
    },
    ({ api }) => {
      api.setSelected(500);
      budFlushSync();
    }
  );

  results.append1k = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.rows = makeRows(1000, 1);
      ctx.api.setRows(ctx.rows);
      budFlushSync();
      return ctx;
    },
    ({ api, rows }) => {
      api.setRows(rows.concat(makeRows(1000, 1001)));
      budFlushSync();
    }
  );

  results.clear1k = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.api.setRows(makeRows(1000, 1));
      budFlushSync();
      return ctx;
    },
    ({ api }) => {
      api.setRows([]);
      budFlushSync();
    }
  );

  results.leafSetStateIn1k = measure(
    RUNS,
    () => {
      const ctx = makeApp();
      ctx.api.setRows(makeRows(1000, 1));
      budFlushSync();
      return ctx;
    },
    ({ api }) => {
      api.incLeaf();
      budFlushSync();
    }
  );

  // SSR 1,000 rows
  const ssrRows = makeRows(1000, 1);
  const ssrTimes = [];
  for (let i = 0; i < RUNS; i++) {
    const t0 = performance.now();
    const html = jsx("table", {
      children: jsx("tbody", {
        children: ssrRows.map((r) =>
          jsxs("tr", {
            children: [
              jsx("td", { children: String(r.id) }),
              jsx("td", { children: r.label }),
            ],
          }, r.id)
        ),
      }),
    }).render();
    const t1 = performance.now();
    if (html.length < 1000) throw new Error("unexpected SSR length");
    ssrTimes.push(t1 - t0);
  }
  results.ssr1k = { ms: median(ssrTimes), ops: {} };

  return results;
}

function tryLoadReact() {
  const candidates = [
    process.env.REACT_NODE_MODULES,
    "/home/quirinpa/duna/node_modules",
  ].filter(Boolean);
  for (const dir of candidates) {
    try {
      const React = require(`${dir}/react`);
      const ReactDOMServer = require(`${dir}/react-dom/server`);
      const reactPkg = require(`${dir}/react/package.json`);
      let reactMinBytes = 0;
      let reactGzipBytes = 0;
      let budRuntimeMinBytes = 0;
      let budRuntimeGzipBytes = 0;
      try {
        const esbuild = require("../examples/jsx-app/node_modules/esbuild");
        const rBuild = esbuild.buildSync({
          stdin: {
            contents: `import React, { useState } from "${dir}/react"; import { createRoot } from "${dir}/react-dom/client"; globalThis.__R = { React, useState, createRoot };`,
            resolveDir: dir,
            loader: "js",
          },
          bundle: true,
          minify: true,
          write: false,
          define: { "process.env.NODE_ENV": '"production"' },
        });
        reactMinBytes = rBuild.outputFiles[0].contents.length;
        reactGzipBytes = gzipSync(rBuild.outputFiles[0].contents).length;

        const bBuild = esbuild.buildSync({
          stdin: {
            contents: `import { jsx, jsxs, render, useState } from "./jsx-runtime/index.js"; globalThis.__B = { jsx, jsxs, render, useState };`,
            resolveDir: new URL("..", import.meta.url).pathname,
            loader: "js",
          },
          bundle: true,
          minify: true,
          write: false,
        });
        budRuntimeMinBytes = bBuild.outputFiles[0].contents.length;
        budRuntimeGzipBytes = gzipSync(bBuild.outputFiles[0].contents).length;
      } catch (_) {}
      return {
        React,
        ReactDOMServer,
        version: reactPkg.version,
        reactMinBytes,
        reactGzipBytes,
        budRuntimeMinBytes,
        budRuntimeGzipBytes,
        dir,
      };
    } catch (_) {}
  }
  return null;
}

function runReactSsrBench(React, ReactDOMServer) {
  const RUNS = 9;
  const rows = makeRows(1000, 1);
  const times = [];
  const ce = React.createElement;
  for (let i = 0; i < RUNS; i++) {
    const t0 = performance.now();
    const html = ReactDOMServer.renderToStaticMarkup(
      ce(
        "table",
        null,
        ce(
          "tbody",
          null,
          rows.map((r) =>
            ce(
              "tr",
              { key: r.id },
              ce("td", null, String(r.id)),
              ce("td", null, r.label)
            )
          )
        )
      )
    );
    const t1 = performance.now();
    if (html.length < 1000) throw new Error("unexpected React SSR length");
    times.push(t1 - t0);
  }
  return median(times);
}

// Measure Bud JSX runtime + app gzipped bundle size
const appBundlePath = new URL("../examples/jsx-app/dist/index.js", import.meta.url);
let budBundleRaw = 0;
let budBundleGzip = 0;
try {
  const buf = readFileSync(appBundlePath);
  budBundleRaw = buf.length;
  budBundleGzip = gzipSync(buf).length;
} catch (_) {}

const budRes = runBudBenchmarks();
const reactInfo = tryLoadReact();

console.log("=== @tty-pt/bud/jsx-runtime Performance Benchmark (median of 9 runs) ===");
console.log(
  "Workload".padEnd(22) +
  "Time (ms)".padStart(11) +
  "createEl".padStart(10) +
  "insert".padStart(9) +
  "remove".padStart(9) +
  "textSet".padStart(9) +
  "nodeVal".padStart(9) +
  "addEv".padStart(8)
);
console.log("-".repeat(87));
for (const [name, { ms, ops }] of Object.entries(budRes)) {
  console.log(
    name.padEnd(22) +
    ms.toFixed(3).padStart(11) +
    String(ops.createElement ?? "-").padStart(10) +
    String(ops.insertBefore ?? "-").padStart(9) +
    String(ops.removeChild ?? "-").padStart(9) +
    String(ops.textContentSet ?? "-").padStart(9) +
    String(ops.nodeValueSet ?? "-").padStart(9) +
    String(ops.addEventListener ?? "-").padStart(8)
  );
}

console.log("\n=== Size & SSR Comparison ===");
if (budBundleRaw > 0) {
  console.log(`Bud JSX (runtime + full MusicApp demo): ${budBundleRaw} B min / ${budBundleGzip} B gzip`);
}
if (reactInfo) {
  const reactSsrMs = runReactSsrBench(reactInfo.React, reactInfo.ReactDOMServer);
  if (reactInfo.budRuntimeGzipBytes > 0 && reactInfo.reactGzipBytes > 0) {
    console.log(
      `Bud JSX runtime only (esbuild min): ${reactInfo.budRuntimeMinBytes} B min / ${reactInfo.budRuntimeGzipBytes} B gzip`
    );
    console.log(
      `React ${reactInfo.version} runtime (esbuild prod min): ${reactInfo.reactMinBytes} B min / ${reactInfo.reactGzipBytes} B gzip`
    );
    console.log(
      `Bundle size ratio: Bud is ${(reactInfo.reactGzipBytes / reactInfo.budRuntimeGzipBytes).toFixed(1)}x smaller gzipped (${(reactInfo.reactMinBytes / reactInfo.budRuntimeMinBytes).toFixed(1)}x smaller minified)`
    );
  }
  console.log(`SSR 1,000 rows: Bud ${budRes.ssr1k.ms.toFixed(3)} ms vs React ${reactInfo.version} ${reactSsrMs.toFixed(3)} ms (${(reactSsrMs / budRes.ssr1k.ms).toFixed(1)}x faster)`);
}

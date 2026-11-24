/**
 * @file demo.jsx
 * @brief Demo showing @tty-pt/bud with JSX syntax, components, props, and VDOM diffing.
 *
 * Run with: node external/libbud/npm/examples/demo.mjs
 */

import { jsx as _jsx, jsxs as _jsxs, Fragment } from "../jsx-runtime/index.js";
import { diffVdom } from "../dist/index.js";

// --- Pure Functional Components (All is Props) ---

function Badge({ status }) {
  const isOnline = status === "online";
  return _jsx("span", {
    className: isOnline ? "badge badge-success" : "badge badge-muted",
    children: isOnline ? "● Live" : "○ Offline"
  });
}

function SongCard({ song, isPlaying }) {
  return _jsxs("div", {
    className: isPlaying ? "song-card playing" : "song-card",
    children: [
      _jsxs("div", {
        className: "song-info",
        children: [
          _jsx("strong", { className: "title", children: song.title }),
          _jsx("span", { className: "artist", children: ` - ${song.artist}` })
        ]
      }),
      _jsx("span", {
        className: "duration",
        children: song.duration
      }),
      isPlaying ? _jsx("span", { className: "indicator", children: " [Playing]" }) : null
    ]
  });
}

function MusicApp({ state }) {
  return _jsxs("main", {
    className: "app-container",
    children: [
      _jsxs("header", {
        className: "header",
        children: [
          _jsx("h1", { children: state.appName }),
          _jsx(Badge, { status: state.status })
        ]
      }),
      _jsx("section", {
        className: "song-list",
        children: state.songs.map((song) =>
          _jsx(SongCard, {
            key: song.id,
            song: song,
            isPlaying: song.id === state.currentPlayingId
          })
        )
      })
    ]
  });
}

// --- Execution & Diff Demonstration ---

console.log("=== 1. Initial State ===");
const state1 = {
  appName: "Bud Music Player",
  status: "online",
  currentPlayingId: 1,
  songs: [
    { id: 1, title: "Space Oddity", artist: "David Bowie", duration: "5:15" },
    { id: 2, title: "Heroes", artist: "David Bowie", duration: "6:07" },
    { id: 3, title: "Starman", artist: "David Bowie", duration: "4:14" }
  ]
};

// Render state 1 to VDOM
const vdom1 = _jsx(MusicApp, { state: state1 });

console.log("\n[SSR / Initial HTML Output]:");
console.log(vdom1.render());

console.log("\n=== 2. State Mutation: Track Changes & Player Goes Offline ===");
const state2 = {
  ...state1,
  status: "offline",
  currentPlayingId: 3 // Starman is now playing instead of Space Oddity
};

// Render state 2 to VDOM
const vdom2 = _jsx(MusicApp, { state: state2 });

console.log("\n=== 3. Libbud VDOM Diffing (via WebAssembly C engine) ===");
const patches = diffVdom(vdom1, vdom2);

console.log(`Generated ${patches.length} patch operation(s):`);
patches.forEach((p, idx) => {
  console.log(`  Patch #${idx + 1}: [${p.type || p.op}]`, {
    a: p.a || undefined,
    b: p.b || undefined,
    c: p.c || undefined
  });
});

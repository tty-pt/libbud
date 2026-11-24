import { useState, useMemo } from "@tty-pt/bud/jsx-runtime";

interface Song {
  id: number;
  title: string;
  artist: string;
  duration: string;
}

const initialSongs: Song[] = [
  { id: 1, title: "Space Oddity", artist: "David Bowie", duration: "5:15" },
  { id: 2, title: "Heroes", artist: "David Bowie", duration: "6:07" },
  { id: 3, title: "Starman", artist: "David Bowie", duration: "4:14" },
  { id: 4, title: "Life on Mars?", artist: "David Bowie", duration: "3:48" },
  { id: 5, title: "Rebel Rebel", artist: "David Bowie", duration: "4:30" },
];

const pool: Omit<Song, "id">[] = [
  { title: "Moonage Daydream", artist: "David Bowie", duration: "4:40" },
  { title: "Changes", artist: "David Bowie", duration: "3:37" },
  { title: "Modern Love", artist: "David Bowie", duration: "4:46" },
  { title: "Ashes to Ashes", artist: "David Bowie", duration: "4:24" },
];

let nextId = 6;

function LikeButton({ icon, initial }: { icon: string; initial: number }) {
  const [likes, setLikes] = useState(initial);

  return (
    <button className="btn" onClick={() => setLikes((c) => c + 1)}>
      {icon} {likes}
    </button>
  );
}

export function MusicApp() {
  const [online, setOnline] = useState(true);
  const [activeId, setActiveId] = useState(1);
  const [query, setQuery] = useState("");
  const [songs, setSongs] = useState(initialSongs);

  const visibleSongs = useMemo(() => {
    const q = query.trim().toLowerCase();
    if (!q) return songs;
    return songs.filter(
      (s) => s.title.toLowerCase().includes(q) || s.artist.toLowerCase().includes(q)
    );
  }, [songs, query]);

  const activeSong = songs.find((s) => s.id === activeId) || songs[0];

  function addTrack() {
    const item = pool[Math.floor(Math.random() * pool.length)];
    setSongs((prev) => [...prev, { id: nextId++, ...item }]);
  }

  function reverseTracks() {
    setSongs((prev) => [...prev].reverse());
  }

  function removeTrack(id: number, e: Event) {
    e.stopPropagation();
    setSongs((prev) => prev.filter((s) => s.id !== id));
  }

  return (
    <div className="app-shell">
      <header className="top-bar">
        <div className="brand">
          <span className="logo">BUD</span>
          <div>
            <h1>Audio Station</h1>
            <p>In-Place Reconciler on Bud</p>
          </div>
        </div>

        <div className="bar-actions">
          <LikeButton icon="♥" initial={42} />
          <LikeButton icon="★" initial={7} />
          <button className="btn" onClick={() => setOnline((o) => !o)}>
            <span className={`dot ${online ? "dot-live" : "dot-offline"}`} />
            {online ? "Synced" : "Offline"}
          </button>
          <button className="btn btn-primary" onClick={addTrack}>
            + Add
          </button>
        </div>
      </header>

      <div className="toolbar">
        <input
          className="search-input"
          type="text"
          placeholder="Filter tracks (focus & caret preserved in-place)..."
          value={query}
          onChange={(e: any) => setQuery(e.target.value)}
        />
        <button className="btn" onClick={reverseTracks}>
          ⇅ Reverse
        </button>
      </div>

      {activeSong ? (
        <div className="now-playing">
          <span className="now-label">Now Playing</span>
          <span className="now-title">
            {activeSong.title} — {activeSong.artist}
          </span>
        </div>
      ) : null}

      <div className="song-list">
        {visibleSongs.map((song, idx) => (
          <div
            key={song.id}
            className={`song-card ${song.id === activeId ? "playing" : ""}`}
            onClick={() => setActiveId(song.id)}
          >
            <div className="song-left">
              <span className="idx">{String(idx + 1).padStart(2, "0")}</span>
              <div>
                <div className="song-title">{song.title}</div>
                <div className="song-artist">{song.artist}</div>
              </div>
            </div>
            <div className="song-right">
              <span className="song-duration">{song.duration}</span>
              <button
                className="btn-del"
                title="Remove"
                onClick={(e: any) => removeTrack(song.id, e)}
              >
                ×
              </button>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

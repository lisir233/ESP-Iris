import { useEffect, useMemo, useRef, useState } from "react";
import { formatTime } from "./api";
import type { GatewayEvent } from "./types";

type Props = { events: GatewayEvent[]; deviceId?: string; compact?: boolean };

export default function LogsPanel({ events, deviceId, compact = false }: Props) {
  const [levels, setLevels] = useState(() => new Set(["E", "W", "I", "D", "V"]));
  const [search, setSearch] = useState("");
  const [tag, setTag] = useState("ALL");
  const [paused, setPaused] = useState(false);
  const [follow, setFollow] = useState(true);
  const [snapshot, setSnapshot] = useState<GatewayEvent[]>([]);
  const body = useRef<HTMLDivElement>(null);
  const logs = events.filter((item) => item.category === "log" && (!deviceId || item.device_id === deviceId));
  const source = paused ? snapshot : logs;
  const tags = Array.from(new Set(logs.map((item) => item.parsed?.tag).filter(Boolean) as string[])).sort();
  const filtered = useMemo(() => source.filter((item) => {
    const level = item.parsed?.level ?? "I";
    const matchesTag = tag === "ALL" || item.parsed?.tag === tag;
    const needle = search.toLowerCase();
    return levels.has(level) && matchesTag && (!needle || item.text?.toLowerCase().includes(needle));
  }), [source, levels, tag, search]);

  useEffect(() => {
    if (follow && !paused && body.current) body.current.scrollTop = body.current.scrollHeight;
  }, [filtered.length, follow, paused]);

  function togglePause() {
    if (!paused) setSnapshot(logs);
    setPaused((value) => !value);
  }

  function toggleLevel(level: string) {
    setLevels((current) => {
      const next = new Set(current);
      if (next.has(level)) next.delete(level); else next.add(level);
      return next;
    });
  }

  function download() {
    const content = filtered.map((item) => item.text ?? JSON.stringify(item)).join("\n") + "\n";
    const link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([content], { type: "text/plain;charset=utf-8" }));
    link.download = `esp-iris-${deviceId || "all"}-logs.txt`;
    link.click();
    URL.revokeObjectURL(link.href);
  }

  return (
    <section className={`logs-panel ${compact ? "compact" : ""}`}>
      <div className="panel-title log-title">
        <span>设备日志</span><small>RAW · {filtered.length}/{logs.length}</small>
        <div className="log-controls">
          {["E", "W", "I", "D", "V"].map((level) => <button key={level} className={`level ${level.toLowerCase()} ${levels.has(level) ? "active" : ""}`} onClick={() => toggleLevel(level)}>{level}</button>)}
          <select aria-label="日志标签" value={tag} onChange={(event) => setTag(event.target.value)}><option>ALL</option>{tags.map((value) => <option key={value}>{value}</option>)}</select>
          <input aria-label="搜索日志" placeholder="搜索原始日志" value={search} onChange={(event) => setSearch(event.target.value)} />
          <button onClick={togglePause}>{paused ? "继续" : "暂停"}</button>
          <button className={follow ? "active-control" : ""} onClick={() => setFollow((value) => !value)}>跟随</button>
          <button onClick={download}>下载</button>
        </div>
      </div>
      <div
        className="log-body"
        ref={body}
        onWheel={() => setFollow(false)}
        onPointerDown={() => setFollow(false)}
      >
        {events.some((item) => item.kind === "history_gap") && <div className="history-gap">— 历史缺口：部分日志已超出保留范围 —</div>}
        {filtered.map((item, index) => {
          const level = item.parsed?.level ?? "I";
          return <div className="log-line" key={`${item.event_id ?? index}-${index}`}><time>{formatTime(item.host_receive_ns || item.host_receive_wall_ns)}</time><b className={level.toLowerCase()}>{level}</b><span className="log-tag">{item.parsed?.tag || "raw"}</span><code>{item.text || JSON.stringify(item)}</code></div>;
        })}
        {!filtered.length && <p className="empty-state">等待设备日志…</p>}
      </div>
    </section>
  );
}

import { useEffect, useRef, useState } from "react";
import { api, formatBytes, jsonBody } from "./api";
import type { Device, FileEntry, FileList, FileVolume } from "./types";

const PAGE_SIZE = 100;

function displayName(device: Device) {
  return device.alias || device.suggested_alias || device.device_id;
}

function fileUrl(deviceId: string, volume: string, path: string) {
  const query = new URLSearchParams({ volume, path });
  return `/v1/devices/${encodeURIComponent(deviceId)}/file?${query}`;
}

export default function Files({ device, mode }: { device?: Device; mode: "develop" | "observe" }) {
  const [volumes, setVolumes] = useState<FileVolume[]>([]);
  const [volume, setVolume] = useState("");
  const [path, setPath] = useState("");
  const [cursor, setCursor] = useState(0);
  const [nextCursor, setNextCursor] = useState<number | null>(null);
  const [entries, setEntries] = useState<FileEntry[]>([]);
  const [loading, setLoading] = useState(false);
  const [mutating, setMutating] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const requestRevision = useRef(0);
  const uploadInput = useRef<HTMLInputElement>(null);

  async function loadDirectory(targetVolume: string, targetPath: string, targetCursor = 0) {
    if (!device) return;
    const revision = ++requestRevision.current;
    setLoading(true);
    setError("");
    try {
      const query = new URLSearchParams({
        volume: targetVolume,
        path: targetPath,
        cursor: String(targetCursor),
        limit: String(PAGE_SIZE),
      });
      const result = await api<FileList>(`/v1/devices/${encodeURIComponent(device.device_id)}/files?${query}`);
      if (revision !== requestRevision.current) return;
      setVolume(targetVolume);
      setPath(targetPath);
      setCursor(result.cursor);
      setNextCursor(result.next_cursor);
      setEntries(result.entries);
    } catch (reason) {
      if (revision === requestRevision.current) setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      if (revision === requestRevision.current) setLoading(false);
    }
  }

  useEffect(() => {
    let current = true;
    const revision = ++requestRevision.current;
    setVolumes([]);
    setVolume("");
    setPath("");
    setEntries([]);
    setError("");
    setNotice("");
    if (!device || !device.connected || mode !== "develop" || !device.capability_names?.includes("files")) return () => { current = false; };
    setLoading(true);
    void api<{ volumes: FileVolume[] }>(`/v1/devices/${encodeURIComponent(device.device_id)}/files/volumes`)
      .then(async (result) => {
        if (!current || revision !== requestRevision.current) return;
        setVolumes(result.volumes);
        if (result.volumes.length) await loadDirectory(result.volumes[0].id, "");
      })
      .catch((reason: unknown) => { if (current && revision === requestRevision.current) setError(reason instanceof Error ? reason.message : String(reason)); })
      .finally(() => { if (current && revision === requestRevision.current) setLoading(false); });
    return () => { current = false; if (requestRevision.current === revision) requestRevision.current += 1; };
  }, [device?.device_id, device?.connected, mode]);

  const selectedVolume = volumes.find((item) => item.id === volume);
  const supports = (capability: string) => selectedVolume?.capability_names.includes(capability) ?? false;

  function childPath(name: string) {
    return path ? `${path}/${name}` : name;
  }

  function checkedLeafName(value: string) {
    const name = value.trim();
    if (!name || name === "." || name === ".." || name.includes("/") || name.includes("\\")) {
      throw new Error("名称必须是当前目录下的单个 UTF-8 文件名");
    }
    return name;
  }

  async function mutate(task: () => Promise<unknown>, message: string) {
    setMutating(true);
    setError("");
    setNotice("");
    try {
      await task();
      await loadDirectory(volume, path, cursor);
      setNotice(message);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setMutating(false);
    }
  }

  async function createDirectory() {
    if (!device) return;
    const prompted = window.prompt("新目录名称");
    if (prompted === null) return;
    let target: string;
    try {
      target = childPath(checkedLeafName(prompted));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
      return;
    }
    await mutate(
      () => api(`/v1/devices/${encodeURIComponent(device.device_id)}/directories`, {
        method: "POST",
        ...jsonBody({ volume, path: target }),
      }),
      `已创建目录 ${target}`,
    );
  }

  async function uploadFile(file: File) {
    if (!device) return;
    let name: string;
    try {
      name = checkedLeafName(file.name);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
      return;
    }
    const existing = entries.find((entry) => entry.name === name);
    if (existing?.kind === "directory") {
      setError(`同名目录 ${name} 已存在`);
      return;
    }
    const overwrite = existing?.kind === "file";
    if (overwrite && !supports("atomic_replace")) {
      setError("该逻辑卷不支持原子替换，不能安全覆盖同名文件");
      return;
    }
    if (overwrite && !window.confirm(`原子替换 ${name}？设备会校验当前 ETag，避免静默覆盖。`)) return;
    const query = new URLSearchParams({ volume, path: childPath(name) });
    if (overwrite) query.set("overwrite", "true");
    await mutate(
      () => api(`/v1/devices/${encodeURIComponent(device.device_id)}/file?${query}`, {
        method: "PUT",
        body: file,
        headers: overwrite ? { "If-Match": `W/"${existing.etag}"` } : undefined,
      }),
      overwrite ? `已原子替换 ${name}` : `已上传 ${name}`,
    );
  }

  async function renameEntry(entry: FileEntry) {
    if (!device) return;
    const prompted = window.prompt("新名称（不能移动到其他逻辑卷）", entry.name);
    if (prompted === null) return;
    let destination: string;
    try {
      destination = childPath(checkedLeafName(prompted));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
      return;
    }
    const source = childPath(entry.name);
    if (source === destination) return;
    await mutate(
      () => api(`/v1/devices/${encodeURIComponent(device.device_id)}/file-rename`, {
        method: "POST",
        ...jsonBody({ volume, source, destination }),
      }),
      `已将 ${source} 重命名为 ${destination}`,
    );
  }

  async function deleteEntry(entry: FileEntry) {
    if (!device) return;
    const target = childPath(entry.name);
    const detail = entry.kind === "directory" ? "仅空目录可删除。" : "此操作不可撤销。";
    if (!window.confirm(`删除 ${target}？${detail}`)) return;
    const query = new URLSearchParams({ volume, path: target });
    await mutate(
      () => api(`/v1/devices/${encodeURIComponent(device.device_id)}/file?${query}`, { method: "DELETE" }),
      `已删除 ${target}`,
    );
  }

  if (!device) return <main className="workspace-empty"><strong>未选择设备</strong><span>从左侧选择一台设备以浏览文件</span></main>;
  if (mode === "observe") return <main className="workspace-empty"><strong>观察模式不访问设备文件</strong><span>切换到开发模式后可访问产品显式导出的逻辑卷</span></main>;
  if (!device.connected) return <main className="workspace-empty"><strong>设备当前离线</strong><span>文件目录不会使用缓存结果</span></main>;
  if (!device.capability_names?.includes("files")) return <main className="workspace-empty"><strong>设备未提供文件服务</strong><span>产品需要在启动 ESP-Iris 前注册逻辑卷</span></main>;

  const parts = path ? path.split("/") : [];
  return (
    <main className="files-page">
      <header className="device-heading">
        <div><p className="eyebrow">文件服务</p><h1>{displayName(device)}</h1><p className="heading-meta">仅显示产品显式注册的逻辑卷；写操作按设备串行并记录审计</p></div>
        <div className="files-actions">
          <label>逻辑卷<select aria-label="逻辑卷" value={volume} onChange={(event) => void loadDirectory(event.target.value, "")} disabled={loading || mutating}>{volumes.map((item) => <option key={item.id} value={item.id}>{item.id}</option>)}</select></label>
          <input ref={uploadInput} className="visually-hidden" type="file" aria-label="选择上传文件" onChange={(event) => { const file = event.target.files?.[0]; event.target.value = ""; if (file) void uploadFile(file); }} />
          <button onClick={() => uploadInput.current?.click()} disabled={loading || mutating || !supports("write")}>上传文件</button>
          <button onClick={() => void createDirectory()} disabled={loading || mutating || !supports("mkdir")}>新建目录</button>
          <button onClick={() => void loadDirectory(volume, path, cursor)} disabled={loading || mutating || !volume}>刷新</button>
        </div>
      </header>
      <nav className="file-breadcrumb" aria-label="文件路径">
        <button onClick={() => void loadDirectory(volume, "")}>/</button>
        {parts.map((part, index) => {
          const target = parts.slice(0, index + 1).join("/");
          return <span key={target}>{index > 0 && <i>/</i>}<button onClick={() => void loadDirectory(volume, target)}>{part}</button></span>;
        })}
      </nav>
      {error && <div className="files-error">{error}</div>}
      {notice && <div className="files-notice">{notice}</div>}
      <section className="file-table-wrap" aria-busy={loading}>
        <table className="file-table">
          <thead><tr><th>名称</th><th>类型</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead>
          <tbody>
            {entries.map((entry) => {
              const target = path ? `${path}/${entry.name}` : entry.name;
              return <tr key={`${entry.kind}:${entry.name}`}><td><button className="file-name" disabled={entry.kind === "file"} onClick={entry.kind === "directory" ? () => void loadDirectory(volume, target) : undefined}><span>{entry.kind === "directory" ? "DIR" : "FILE"}</span><strong>{entry.name}</strong><small>{entry.etag}</small></button></td><td>{entry.kind === "directory" ? "目录" : "文件"}</td><td>{entry.kind === "directory" ? "—" : formatBytes(entry.size)}</td><td>{entry.mtime_s ? new Date(entry.mtime_s * 1000).toLocaleString() : "—"}</td><td><div className="file-row-actions">{entry.kind === "file" ? <a className="button-link" href={fileUrl(device.device_id, volume, target)} download={entry.name}>下载</a> : <button className="button-link" disabled={mutating} onClick={() => void loadDirectory(volume, target)}>打开</button>}{supports("rename") && <button className="button-link" disabled={mutating} onClick={() => void renameEntry(entry)}>重命名</button>}{supports("delete") && <button className="button-link danger-outline" disabled={mutating} onClick={() => void deleteEntry(entry)}>删除</button>}</div></td></tr>;
            })}
          </tbody>
        </table>
        {!loading && !entries.length && <p className="compact-empty">目录为空</p>}
        {loading && <p className="compact-empty">正在读取目录…</p>}
      </section>
      <footer className="file-pagination"><span>第 {cursor + 1} 项起 · 目录游标不是快照</span><div><button disabled={loading || cursor === 0} onClick={() => void loadDirectory(volume, path, Math.max(0, cursor - PAGE_SIZE))}>上一页</button><button disabled={loading || nextCursor === null} onClick={() => void loadDirectory(volume, path, nextCursor ?? 0)}>下一页</button></div></footer>
    </main>
  );
}

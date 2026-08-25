import { useMemo, useState } from "react";
import { formatDuration, formatTime } from "./api";
import { PageHeading } from "./Overview";
import { actionLabel } from "./Workspace";
import type { Device, Operation } from "./types";

export default function Records({ operations, devices }: { operations: Operation[]; devices: Device[] }) {
  const [actor, setActor] = useState("all");
  const [status, setStatus] = useState("all");
  const [deviceId, setDeviceId] = useState("all");
  const [search, setSearch] = useState("");
  const [selected, setSelected] = useState<Operation | null>(null);
  const filtered = useMemo(() => operations.filter((item) => (actor === "all" || item.actor_type === actor) && (status === "all" || item.status === status) && (deviceId === "all" || item.device_id === deviceId) && (!search || JSON.stringify(item).toLowerCase().includes(search.toLowerCase()))), [operations, actor, status, deviceId, search]);

  return <main className="content-page records-page"><PageHeading eyebrow="DEVICE ACTIONS / STRUCTURED TIMELINE" title="操作记录" copy="只记录真实设备侧行为；代码修改、构建和 PC Shell 不进入此时间线。" /><section className="records-toolbar"><select value={deviceId} onChange={(event) => setDeviceId(event.target.value)}><option value="all">全部设备</option>{devices.map((device) => <option key={device.device_id} value={device.device_id}>{device.alias || device.suggested_alias || device.device_id}</option>)}</select><select value={actor} onChange={(event) => setActor(event.target.value)}><option value="all">全部操作者</option><option value="agent">Agent</option><option value="developer">Developer</option></select><select value={status} onChange={(event) => setStatus(event.target.value)}><option value="all">全部状态</option>{["queued", "running", "waiting_device", "reconnecting", "succeeded", "failed", "cancelled", "interrupted", "outcome_unknown"].map((value) => <option key={value}>{value}</option>)}</select><input placeholder="搜索动作、参数、ID" value={search} onChange={(event) => setSearch(event.target.value)} /><span>{filtered.length} 条</span></section><section className="records-layout"><div className="table-pane records-table"><table><thead><tr><th>时间</th><th>操作者</th><th>设备</th><th>动作</th><th>状态</th><th>耗时</th><th>队列</th></tr></thead><tbody>{filtered.map((item) => <tr key={item.operation_id} className={selected?.operation_id === item.operation_id ? "selected-row" : ""} onClick={() => setSelected(item)}><td>{formatTime(item.created_ns)}</td><td><span className={`actor ${item.actor_type}`}>{item.actor_type === "agent" ? "AGENT" : "DEV"}</span><small>{item.actor_name}</small></td><td><code>{deviceName(devices, item.device_id)}</code></td><td><strong>{actionLabel(item.action)}</strong><small>{item.action}</small></td><td><span className={`op-status ${item.status}`}>{item.status}</span></td><td>{formatDuration(item.started_ns, item.finished_ns)}</td><td>{item.queue_position || "—"}</td></tr>)}</tbody></table>{!filtered.length && <p className="empty-state">没有符合筛选条件的操作</p>}</div><aside className="record-detail"><div className="panel-title"><span>操作详情</span><small>STRUCTURED</small></div>{selected ? <><dl><dt>Operation ID</dt><dd>{selected.operation_id}</dd><dt>设备 ID</dt><dd>{selected.device_id}</dd><dt>操作者</dt><dd>{selected.actor_type} / {selected.actor_name}</dd><dt>状态</dt><dd><span className={`op-status ${selected.status}`}>{selected.status}</span></dd><dt>开始</dt><dd>{formatTime(selected.started_ns)}</dd><dt>结束</dt><dd>{formatTime(selected.finished_ns)}</dd><dt>耗时</dt><dd>{formatDuration(selected.started_ns, selected.finished_ns)}</dd></dl><h3>已净化参数</h3><pre>{JSON.stringify(selected.params, null, 2)}</pre><h3>结果</h3><pre className={selected.error ? "error-text" : ""}>{selected.error || JSON.stringify(selected.result, null, 2) || "—"}</pre></> : <p className="empty-state">选择一条操作查看阶段、参数和结果</p>}</aside></section></main>;
}

function deviceName(devices: Device[], id: string) {
  const item = devices.find((device) => device.device_id === id);
  return item?.alias || item?.suggested_alias || id.slice(0, 12);
}

import { FormEvent, useEffect, useState } from "react";
import { PageHeading } from "./Overview";

type Spec = { info?: { title: string; version: string; description: string }; paths?: Record<string, Record<string, { summary?: string }>> };

export default function Docs({ mode }: { mode: "develop" | "observe" }) {
  const [spec, setSpec] = useState<Spec>({});
  const [method, setMethod] = useState("GET");
  const [path, setPath] = useState("/v1/health");
  const [body, setBody] = useState("{}");
  const [result, setResult] = useState("");
  const [status, setStatus] = useState(0);
  useEffect(() => { fetch("/v1/openapi.json").then((response) => response.json()).then(setSpec); }, []);

  async function execute(event: FormEvent) {
    event.preventDefault();
    try {
      const response = await fetch(path, { method, credentials: "same-origin", headers: { "Content-Type": "application/json" }, body: ["GET", "HEAD"].includes(method) ? undefined : body });
      setStatus(response.status);
      const text = await response.text();
      try { setResult(JSON.stringify(JSON.parse(text), null, 2)); } catch { setResult(text); }
    } catch (reason) { setResult(String(reason)); setStatus(0); }
  }

  const paths = Object.entries(spec.paths || {}).flatMap(([route, methods]) => Object.entries(methods).map(([verb, operation]) => ({ route, verb: verb.toUpperCase(), summary: operation.summary })));
  return <main className="content-page docs-page"><PageHeading eyebrow="OPENAPI 3.1 / GATEWAY" title="API 文档与试验台" copy="网关设备接口的可执行参考；CLI 与 Agent 使用相同的 /v1 契约。" />{mode === "observe" && <div className="observe-banner"><strong>观察模式</strong><span>可以查看文档；设备业务请求会返回 423，不会到达设备。</span></div>}<div className="docs-layout"><section className="endpoint-list"><div className="panel-title"><span>{spec.info?.title || "ESP-Iris API"}</span><small>v{spec.info?.version || "1.0.0"}</small></div>{paths.map((item) => <button key={`${item.verb}-${item.route}`} onClick={() => { setMethod(item.verb); setPath(item.route.replace("{device_id}", "demo-a1b2c3d4").replace("{job_id}", "1")); }}><em className={`http-method ${item.verb.toLowerCase()}`}>{item.verb}</em><code>{item.route}</code><span>{item.summary}</span></button>)}</section><section className="api-console"><div className="panel-title"><span>请求</span><small>UNIT TEST CONSOLE</small></div><form onSubmit={execute}><div className="request-line"><select value={method} onChange={(event) => setMethod(event.target.value)}>{["GET", "POST", "PUT", "PATCH", "DELETE"].map((value) => <option key={value}>{value}</option>)}</select><input value={path} onChange={(event) => setPath(event.target.value)} /><button className="primary-button">发送</button></div><label>JSON Body<textarea value={body} onChange={(event) => setBody(event.target.value)} /></label></form><div className="response-heading"><span>响应</span><em className={status >= 400 ? "failed" : status ? "succeeded" : ""}>{status || "—"}</em></div><pre className="api-response">{result || "// 在这里执行 /v1 请求"}</pre></section></div></main>;
}

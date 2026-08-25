import { useEffect, useState } from "react";
import PageHeading from "./PageHeading";

type Spec = { info?: { title: string; version: string; description: string }; paths?: Record<string, Record<string, { summary?: string }>> };

export default function Docs({ mode }: { mode: "develop" | "observe" }) {
  const [spec, setSpec] = useState<Spec>({});
  useEffect(() => { fetch("/v1/openapi.json").then((response) => response.json()).then(setSpec); }, []);
  const paths = Object.entries(spec.paths || {}).flatMap(([route, methods]) => Object.entries(methods).map(([verb, operation]) => ({ route, verb: verb.toUpperCase(), summary: operation.summary })));

  return <main className="content-page docs-page">
    <PageHeading title="API 文档" copy="ESP-IRIS 网关的 OpenAPI 接口参考。" actions={<a className="button-link" href="/v1/openapi.json" target="_blank" rel="noreferrer">打开 OpenAPI JSON</a>} />
    {mode === "observe" && <div className="observe-banner docs-mode-note"><strong>观察模式</strong><span>写入设备的请求当前不可用。</span></div>}
    <section className="endpoint-list docs-endpoints"><div className="panel-title"><span>{spec.info?.title || "ESP-IRIS API"}</span><small>v{spec.info?.version || "1.0.0"}</small></div>{paths.map((item) => <div className="endpoint-row" key={`${item.verb}-${item.route}`}><em className={`http-method ${item.verb.toLowerCase()}`}>{item.verb}</em><code>{item.route}</code><span>{item.summary}</span></div>)}</section>
  </main>;
}

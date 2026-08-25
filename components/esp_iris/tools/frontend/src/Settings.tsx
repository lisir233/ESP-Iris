import { FormEvent, useEffect, useState } from "react";
import { api, formatTime } from "./api";
import { PageHeading } from "./Overview";
import type { Audit } from "./types";

type Token = {
  token_id: string;
  name: string;
  created_ns: number;
  last_used_ns?: number;
  revoked_ns?: number;
  token?: string;
};

type Props = {
  mode: "develop" | "observe";
  transitioning: boolean;
  setMode: (value: "develop" | "observe") => Promise<void>;
  audits: Audit[];
  demo: boolean;
  localAuthRequired: boolean;
};

export default function Settings({
  mode,
  transitioning,
  setMode,
  audits,
  demo,
  localAuthRequired,
}: Props) {
  const [tokens, setTokens] = useState<Token[]>([]);
  const [name, setName] = useState("");
  const [shownToken, setShownToken] = useState("");
  const [password, setPassword] = useState("");
  const [message, setMessage] = useState("");
  const [catalog, setCatalog] = useState<{
    methods?: { name: string; service_id: number; method_id: number; timeout_ms: number }[];
  }>({});

  async function refreshTokens() {
    const result = await api<{ tokens: Token[] }>("/v1/auth/tokens");
    setTokens(result.tokens);
  }

  useEffect(() => {
    refreshTokens();
    api<typeof catalog>("/v1/rpc-catalog").then(setCatalog);
  }, []);

  async function createToken(event: FormEvent) {
    event.preventDefault();
    const result = await api<Token>("/v1/auth/tokens", {
      method: "POST",
      body: JSON.stringify({ name }),
      headers: { "Content-Type": "application/json" },
    });
    setShownToken(result.token || "");
    setName("");
    await refreshTokens();
  }

  async function revoke(id: string) {
    if (!window.confirm("确认撤销此 Agent Token？撤销后无法恢复。")) return;
    await api(`/v1/auth/tokens/${id}`, { method: "DELETE" });
    await refreshTokens();
  }

  async function changePassword(event: FormEvent) {
    event.preventDefault();
    if (!window.confirm("更改口令会使当前浏览器会话失效，确认继续？")) return;
    await api("/v1/auth/password", {
      method: "PUT",
      body: JSON.stringify({ password }),
      headers: { "Content-Type": "application/json" },
    });
    location.reload();
  }

  return (
    <main className="content-page settings-page">
      <PageHeading
        eyebrow="GATEWAY / LOCAL CONTROL PLANE"
        title="系统设置"
        copy="本机认证状态、远程访问凭据、运行模式、TLS、存储与 RPC Catalog。"
      />
      {message && <div className="inline-notice">{message}</div>}
      <div className="settings-grid">
        <section className="settings-section">
          <div className="panel-title"><span>访问认证</span><small>LOCAL / REMOTE</small></div>
          <dl className="settings-dl">
            <dt>本机回环访问</dt>
            <dd>{localAuthRequired ? "需要开发口令或 Agent Token" : "默认免认证"}</dd>
            <dt>局域网与其他设备</dt>
            <dd>始终需要开发口令或具名 Agent Token</dd>
          </dl>
          <p className="section-copy">仅依据实际 TCP 对端地址判断，不信任 X-Forwarded-For。</p>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>运行模式</span><small>GLOBAL</small></div>
          <div className="mode-setting">
            <button className={mode === "develop" ? "selected" : ""} disabled={transitioning} onClick={() => setMode("develop")}>
              <span className="status-dot online" /><strong>开发模式</strong><small>开放全部设备接口；Agent 默认模式</small>
            </button>
            <button className={mode === "observe" ? "selected" : ""} disabled={transitioning} onClick={() => setMode("observe")}>
              <span className="status-dot" /><strong>观察模式</strong><small>停止所有业务设备请求，仅显示缓存和主动上报</small>
            </button>
          </div>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>远程 Agent Token</span><small>具名 · 同等权限</small></div>
          <form className="inline-form" onSubmit={createToken}>
            <input placeholder="Token 名称，例如 codex-bench-a" value={name} onChange={(event) => setName(event.target.value)} required />
            <button className="primary-button">创建 Token</button>
          </form>
          {shownToken && <div className="token-once"><strong>仅显示一次</strong><code>{shownToken}</code><button onClick={() => navigator.clipboard.writeText(shownToken)}>复制</button></div>}
          <div className="token-list">
            {tokens.map((token) => <div key={token.token_id}>
              <span className={`token-icon ${token.revoked_ns ? "revoked" : ""}`}>A</span>
              <p><strong>{token.name}</strong><small>{token.token_id} · 创建于 {formatTime(token.created_ns)} · 最近使用 {formatTime(token.last_used_ns)}</small></p>
              <em>{token.revoked_ns ? "已撤销" : "有效"}</em>
              {!token.revoked_ns && <button onClick={() => revoke(token.token_id)}>撤销</button>}
            </div>)}
          </div>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>远程开发口令</span><small>共享认证</small></div>
          <form className="inline-form" onSubmit={changePassword}>
            <input type="password" minLength={8} placeholder="新的开发口令" value={password} onChange={(event) => setPassword(event.target.value)} required />
            <button>更改口令</button>
          </form>
          <p className="section-copy">局域网和其他非回环访问必须通过此口令认证。V1 不创建多开发者账号。</p>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>TLS 与网络</span><small>{location.protocol === "https:" ? "HTTPS" : "HTTP DEFAULT"}</small></div>
          <dl className="settings-dl">
            <dt>当前来源</dt><dd>{location.origin}</dd>
            <dt>协议</dt><dd>{location.protocol === "https:" ? "HTTPS / TLS" : "HTTP（默认，未加密）"}</dd>
            <dt>局域网访问</dt><dd>使用 <code>--listen 0.0.0.0</code>；其他设备仍需认证</dd>
            <dt>可选 HTTPS</dt><dd>使用 <code>--tls</code>，并在客户端信任证书或指纹</dd>
          </dl>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>存储与导出</span><small>SQLITE + FILES</small></div>
          <dl className="settings-dl">
            <dt>结构化记录 / 工件</dt><dd>默认永久保留</dd>
            <dt>原始设备日志</dt><dd>7 天或 1 GiB</dd>
            <dt>实时媒体</dt><dd>默认不落盘</dd>
          </dl>
          <button className="wide-button" onClick={async () => {
            const response = await fetch("/v1/export", { method: "POST" });
            if (!response.ok) return setMessage("导出失败");
            const blob = await response.blob();
            const link = document.createElement("a");
            link.href = URL.createObjectURL(blob);
            link.download = "esp-iris-export.zip";
            link.click();
          }}>导出完整证据包 ZIP</button>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>RPC Catalog</span><small>{catalog.methods?.length || 0} METHODS</small></div>
          <div className="catalog-list">{catalog.methods?.map((method) => <div key={method.name}><code>{method.name}</code><span>S{method.service_id} / M{method.method_id}</span><small>{method.timeout_ms} ms</small></div>)}</div>
        </section>

        <section className="settings-section">
          <div className="panel-title"><span>Demo 与发现</span><small>RUNTIME</small></div>
          <dl className="settings-dl">
            <dt>Demo 模式</dt><dd><span className={`demo-badge ${demo ? "" : "disabled"}`}>{demo ? "ACTIVE" : "OFF"}</span></dd>
            <dt>USB 自动发现</dt><dd>ESP-Iris Normal / Recovery</dd>
            <dt>默认传输</dt><dd>USB Highspeed</dd>
          </dl>
        </section>

        <section className="settings-section audit-section">
          <div className="panel-title"><span>系统审计</span><small>非设备行为</small></div>
          <div className="audit-list">{audits.map((audit) => <div key={audit.audit_id}><time>{formatTime(audit.created_ns)}</time><span className={`actor ${audit.actor_type}`}>{audit.actor_type}</span><strong>{audit.action}</strong><code>{JSON.stringify(audit.details)}</code></div>)}</div>
        </section>
      </div>
    </main>
  );
}

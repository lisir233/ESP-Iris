import { FormEvent, useEffect, useState } from "react";
import { api, formatTime } from "./api";
import PageHeading from "./PageHeading";

type Token = { token_id: string; name: string; created_ns: number; last_used_ns?: number; revoked_ns?: number; token?: string };
type Props = { mode: "develop" | "observe"; demo: boolean; localAuthRequired: boolean; onOpenDocs: () => void };

export default function Settings({ mode, demo, localAuthRequired, onOpenDocs }: Props) {
  const [tokens, setTokens] = useState<Token[]>([]);
  const [name, setName] = useState("");
  const [shownToken, setShownToken] = useState("");
  const [password, setPassword] = useState("");
  const [message, setMessage] = useState("");

  async function refreshTokens() {
    const result = await api<{ tokens: Token[] }>("/v1/auth/tokens");
    setTokens(result.tokens);
  }

  useEffect(() => { void refreshTokens(); }, []);

  async function createToken(event: FormEvent) {
    event.preventDefault();
    const result = await api<Token>("/v1/auth/tokens", { method: "POST", body: JSON.stringify({ name }), headers: { "Content-Type": "application/json" } });
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
    await api("/v1/auth/password", { method: "PUT", body: JSON.stringify({ password }), headers: { "Content-Type": "application/json" } });
    location.reload();
  }

  async function exportData() {
    const response = await fetch("/v1/export", { method: "POST" });
    if (!response.ok) return setMessage("导出失败");
    const blob = await response.blob();
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "esp-iris-export.zip";
    link.click();
    URL.revokeObjectURL(link.href);
    setMessage("证据包已导出");
  }

  return <main className="content-page settings-page">
    <PageHeading title="设置" copy="管理网关凭据、数据和开发者资源。" />
    {message && <div className="inline-notice page-notice">{message}</div>}
    <div className="settings-grid">
      <section className="settings-section">
        <div className="panel-title"><span>访问认证</span></div>
        <dl className="settings-dl"><dt>本机访问</dt><dd>{localAuthRequired ? "需要开发口令或 Agent Token" : "当前免认证"}</dd><dt>远程访问</dt><dd>需要开发口令或具名 Agent Token</dd><dt>当前模式</dt><dd>{mode === "develop" ? "开发模式" : "观察模式"} · 在页面顶部切换</dd></dl>
      </section>

      <section className="settings-section">
        <div className="panel-title"><span>网关信息</span></div>
        <dl className="settings-dl"><dt>地址</dt><dd>{location.origin}</dd><dt>连接协议</dt><dd>{location.protocol === "https:" ? "HTTPS" : "HTTP（未加密）"}</dd><dt>Demo</dt><dd>{demo ? "已启用" : "未启用"}</dd></dl>
      </section>

      <section className="settings-section settings-wide">
        <div className="panel-title"><span>Agent Token</span></div>
        <form className="inline-form" onSubmit={createToken}><input placeholder="Token 名称" value={name} onChange={(event) => setName(event.target.value)} required /><button className="primary-button">创建 Token</button></form>
        {shownToken && <div className="token-once"><strong>仅显示一次</strong><code>{shownToken}</code><button onClick={() => navigator.clipboard.writeText(shownToken)}>复制</button></div>}
        <div className="token-list">{tokens.map((token) => <div key={token.token_id}><span className={`token-icon ${token.revoked_ns ? "revoked" : ""}`}>A</span><p><strong>{token.name}</strong><small>{token.token_id} · 创建于 {formatTime(token.created_ns)} · 最近使用 {formatTime(token.last_used_ns)}</small></p><em>{token.revoked_ns ? "已撤销" : "有效"}</em>{!token.revoked_ns && <button onClick={() => revoke(token.token_id)}>撤销</button>}</div>)}</div>
      </section>

      <section className="settings-section">
        <div className="panel-title"><span>开发口令</span></div>
        <form className="inline-form" onSubmit={changePassword}><input type="password" minLength={8} placeholder="新的开发口令" value={password} onChange={(event) => setPassword(event.target.value)} required /><button>更改口令</button></form>
        <p className="section-copy">更改后当前浏览器会话将失效。</p>
      </section>

      <section className="settings-section">
        <div className="panel-title"><span>数据与开发者资源</span></div>
        <div className="settings-actions"><button onClick={exportData}>导出证据包 ZIP</button><button onClick={onOpenDocs}>查看 API 文档</button><a href="/v1/openapi.json" target="_blank" rel="noreferrer">打开 OpenAPI JSON</a></div>
      </section>
    </div>
  </main>;
}

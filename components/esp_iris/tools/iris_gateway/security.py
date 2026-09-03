from __future__ import annotations

import dataclasses
import hashlib
import hmac
import json
import secrets
import sqlite3
import time
from collections.abc import Iterable
from typing import Any

from .store import GatewayStore

DEFAULT_DEVELOPER_PASSWORD = "espressif"
AGENT_TOKEN_SCOPES = frozenset({"files.read", "files.write", "files.delete"})


@dataclasses.dataclass(frozen=True)
class Actor:
    kind: str
    name: str
    scopes: frozenset[str] = frozenset({"*"})

    def as_dict(self) -> dict[str, Any]:
        return {"type": self.kind, "name": self.name, "scopes": sorted(self.scopes)}

    def allows(self, scope: str) -> bool:
        return "*" in self.scopes or scope in self.scopes


class AuthManager:
    COOKIE_NAME = "esp_iris_session"
    PASSWORD_ROUNDS = 310_000

    def __init__(self, store: GatewayStore) -> None:
        self.store = store
        self._sessions: dict[str, tuple[Actor, int]] = {}

    @property
    def configured(self) -> bool:
        return self.store.get_setting("password_hash") is not None

    def set_initial_password(self, password: str, actor_name: str = "bootstrap") -> None:
        if self.configured:
            raise RuntimeError("developer password is already configured")
        self._set_password(password)
        self.store.add_audit("system", actor_name, "password.configured")

    def change_password(self, password: str, actor: Actor) -> None:
        self._set_password(password)
        self._sessions.clear()
        self.store.add_audit(actor.kind, actor.name, "password.changed")

    def _set_password(self, password: str) -> None:
        if len(password) < 8:
            raise ValueError("developer password must contain at least 8 characters")
        salt = secrets.token_bytes(16)
        digest = hashlib.pbkdf2_hmac(
            "sha256", password.encode("utf-8"), salt, self.PASSWORD_ROUNDS
        )
        self.store.set_setting(
            "password_hash",
            {
                "algorithm": "pbkdf2-sha256",
                "rounds": self.PASSWORD_ROUNDS,
                "salt": salt.hex(),
                "digest": digest.hex(),
            },
        )

    def verify_password(self, password: str) -> bool:
        saved = self.store.get_setting("password_hash")
        if not isinstance(saved, dict):
            return False
        try:
            digest = hashlib.pbkdf2_hmac(
                "sha256",
                password.encode("utf-8"),
                bytes.fromhex(saved["salt"]),
                int(saved["rounds"]),
            )
            return hmac.compare_digest(digest.hex(), str(saved["digest"]))
        except (KeyError, TypeError, ValueError):
            return False

    def login(self, password: str) -> str:
        if not self.verify_password(password):
            raise PermissionError("invalid developer password")
        token = secrets.token_urlsafe(32)
        self._sessions[token] = (Actor("developer", "Developer"), time.time_ns())
        return token

    def logout(self, token: str | None) -> None:
        if token:
            self._sessions.pop(token, None)

    def browser_actor(self, token: str | None) -> Actor | None:
        if not token:
            return None
        entry = self._sessions.get(token)
        return entry[0] if entry else None

    def create_agent_token(
        self,
        name: str,
        actor: Actor,
        scopes: Iterable[str] | None = None,
    ) -> dict[str, Any]:
        clean = name.strip()
        if not clean or len(clean) > 64:
            raise ValueError("token name must contain 1 to 64 characters")
        selected_scopes = frozenset({"files.read"} if scopes is None else scopes)
        if not selected_scopes or not selected_scopes <= AGENT_TOKEN_SCOPES:
            raise ValueError("agent token contains an unsupported or empty scope set")
        token_id = secrets.token_hex(8)
        secret = secrets.token_urlsafe(32)
        bearer = f"iris_{token_id}_{secret}"
        digest = hashlib.sha256(bearer.encode("utf-8")).hexdigest()
        now = time.time_ns()
        try:
            self.store.db.execute(
                "INSERT INTO agent_tokens"
                "(token_id, name, token_hash, created_ns, scopes_json) "
                "VALUES(?, ?, ?, ?, ?)",
                (token_id, clean, digest, now, json.dumps(sorted(selected_scopes))),
            )
        except sqlite3.IntegrityError as exc:
            raise ValueError(f"agent token name is already in use: {clean}") from exc
        self.store.db.commit()
        self.store.add_audit(
            actor.kind, actor.name, "agent_token.created", {"token_id": token_id, "name": clean}
        )
        return {
            "token_id": token_id,
            "name": clean,
            "token": bearer,
            "created_ns": now,
            "scopes": sorted(selected_scopes),
            "shown_once": True,
        }

    def authenticate_bearer(self, bearer: str | None) -> Actor | None:
        if not bearer or not bearer.startswith("iris_"):
            return None
        digest = hashlib.sha256(bearer.encode("utf-8")).hexdigest()
        row = self.store.db.execute(
            "SELECT token_id, name, scopes_json FROM agent_tokens "
            "WHERE token_hash=? AND revoked_ns IS NULL",
            (digest,),
        ).fetchone()
        if row is None:
            return None
        self.store.db.execute(
            "UPDATE agent_tokens SET last_used_ns=? WHERE token_id=?",
            (time.time_ns(), row["token_id"]),
        )
        self.store.db.commit()
        try:
            scopes = frozenset(json.loads(str(row["scopes_json"])))
        except (TypeError, ValueError):
            scopes = frozenset()
        return Actor("agent", str(row["name"]), scopes)

    def list_agent_tokens(self) -> list[dict[str, Any]]:
        rows = self.store.db.execute(
            "SELECT token_id, name, created_ns, last_used_ns, revoked_ns, scopes_json "
            "FROM agent_tokens ORDER BY created_ns DESC"
        ).fetchall()
        result = []
        for row in rows:
            item = dict(row)
            item["scopes"] = json.loads(item.pop("scopes_json"))
            result.append(item)
        return result

    def revoke_agent_token(self, token_id: str, actor: Actor) -> None:
        row = self.store.db.execute(
            "SELECT name, revoked_ns FROM agent_tokens WHERE token_id=?", (token_id,)
        ).fetchone()
        if row is None:
            raise KeyError(token_id)
        if row["revoked_ns"] is None:
            self.store.db.execute(
                "UPDATE agent_tokens SET revoked_ns=? WHERE token_id=?",
                (time.time_ns(), token_id),
            )
            self.store.db.commit()
            self.store.add_audit(
                actor.kind,
                actor.name,
                "agent_token.revoked",
                {"token_id": token_id, "name": row["name"]},
            )


__all__ = ["AGENT_TOKEN_SCOPES", "DEFAULT_DEVELOPER_PASSWORD", "Actor", "AuthManager"]

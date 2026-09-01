from __future__ import annotations

import json
import os
import socket
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Mapping
from http.cookies import SimpleCookie
from typing import Any, Self, TextIO

from .artifacts import ArtifactStore
from .config import TOOLS
from .runner import CommandRunner


def unused_tcp_port() -> int:
    with socket.socket() as handle:
        handle.bind(("127.0.0.1", 0))
        return int(handle.getsockname()[1])


class GatewayApi:
    def __init__(
        self,
        base_url: str,
        artifacts: ArtifactStore,
        *,
        ssl_context: ssl.SSLContext | None = None,
        bearer_token: str | None = None,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.artifacts = artifacts
        self.ssl_context = ssl_context
        self.bearer_token = bearer_token
        self.cookies: dict[str, str] = {}

    def request(
        self,
        method: str,
        path: str,
        *,
        json_body: Any | None = None,
        body: bytes | None = None,
        headers: Mapping[str, str] | None = None,
        timeout: float = 15,
        evidence_name: str | None = None,
    ) -> tuple[int, Any, dict[str, str]]:
        request_headers = dict(headers or {})
        if self.bearer_token:
            request_headers["Authorization"] = f"Bearer {self.bearer_token}"
        if self.cookies:
            request_headers["Cookie"] = "; ".join(
                f"{name}={value}" for name, value in self.cookies.items()
            )
        if json_body is not None:
            body = json.dumps(json_body).encode()
            request_headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self.base_url + path,
            data=body,
            headers=request_headers,
            method=method,
        )
        try:
            response = urllib.request.urlopen(
                request, timeout=timeout, context=self.ssl_context
            )
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            status = exc.code
            response_headers = dict(exc.headers)
        else:
            with response:
                raw = response.read()
                status = response.status
                response_headers = dict(response.headers)
        cookie = SimpleCookie()
        for value in response_headers.get("Set-Cookie", "").splitlines():
            cookie.load(value)
        self.cookies.update(
            {name: morsel.value for name, morsel in cookie.items()}
        )
        content_type = response_headers.get("Content-Type", "")
        if "application/json" in content_type and raw:
            payload: Any = json.loads(raw)
        else:
            payload = raw
        if evidence_name:
            self.artifacts.write_json(
                f"responses/{evidence_name}.json",
                {
                    "method": method,
                    "path": path,
                    "status": status,
                    "headers": response_headers,
                    "payload": payload if not isinstance(payload, bytes) else {
                        "bytes": len(payload)
                    },
                },
            )
        return status, payload, response_headers

    def wait_healthy(self, timeout: float = 30) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                status, payload, _ = self.request("GET", "/v1/health", timeout=2)
                if status == 200 and isinstance(payload, dict):
                    return payload
            except (OSError, ValueError) as exc:
                last_error = exc
            time.sleep(0.1)
        raise TimeoutError("Gateway did not become healthy") from last_error

    def wait_device(self, timeout: float = 45) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status, payload, _ = self.request("GET", "/v1/devices", timeout=3)
            if status == 200 and isinstance(payload, dict):
                connected = [
                    item for item in payload.get("devices", []) if item.get("connected")
                ]
                if len(connected) == 1:
                    return connected[0]
            time.sleep(0.2)
        raise TimeoutError("exactly one connected Iris device was not observed")


class GatewayProcess:
    def __init__(
        self,
        artifacts: ArtifactStore,
        *,
        endpoint_kind: str,
        endpoint: str,
        pairing_token: str | None = None,
        tls: bool = False,
        require_local_auth: bool = False,
        port: int | None = None,
        name: str = "gateway",
    ) -> None:
        self.artifacts = artifacts
        self.endpoint_kind = endpoint_kind
        self.endpoint = endpoint
        self.pairing_token = pairing_token
        self.tls = tls
        self.require_local_auth = require_local_auth
        self.port = port or unused_tcp_port()
        self.name = name
        self.state_dir = artifacts.root / "state" / name
        self.process: subprocess.Popen[str] | None = None
        self._log: TextIO | None = None
        scheme = "https" if tls else "http"
        self.base_url = f"{scheme}://127.0.0.1:{self.port}"

    def start(self) -> GatewayApi:
        self.state_dir.mkdir(parents=True, exist_ok=True)
        argv = [
            sys.executable,
            str(TOOLS / "esp_iris.py"),
            "web",
            "--listen",
            "127.0.0.1",
            "--port",
            str(self.port),
            "--state-dir",
            str(self.state_dir),
            "--instance-id",
            self.name,
            "--tls" if self.tls else "--no-tls",
        ]
        if self.endpoint_kind != "discover_usb":
            argv.append("--no-discover-usb")
        if self.require_local_auth:
            argv.append("--require-local-auth")
        if self.endpoint_kind == "usb":
            argv.extend(["--usb", self.endpoint])
        elif self.endpoint_kind == "usj":
            argv.extend(["--usb-serial-jtag", self.endpoint])
        elif self.endpoint_kind == "tcp":
            argv.extend(["--tcp", self.endpoint])
            if self.pairing_token:
                argv.extend(["--pairing-token", self.pairing_token])
        elif self.endpoint_kind == "discover_usb":
            pass
        else:
            raise ValueError(f"unsupported endpoint kind: {self.endpoint_kind}")
        log_path = self.artifacts.logs / f"{self.name}.log"
        self._log = log_path.open("w", encoding="utf-8")
        environment = os.environ.copy()
        environment["ESP_IRIS_DEVELOPER_PASSWORD"] = "iris-e2e-developer-password"
        creationflags = (
            subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        )
        self.process = subprocess.Popen(
            argv,
            cwd=TOOLS,
            env=environment,
            text=True,
            stdout=self._log,
            stderr=subprocess.STDOUT,
            creationflags=creationflags,
        )
        context = None
        if self.tls:
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
        api = GatewayApi(self.base_url, self.artifacts, ssl_context=context)
        try:
            deadline = time.monotonic() + 30
            last_error: Exception | None = None
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    self._log.flush()
                    output = log_path.read_text(encoding="utf-8", errors="replace")
                    detail = output.strip().splitlines()[-1] if output.strip() else ""
                    raise RuntimeError(
                        f"Gateway exited before becoming healthy "
                        f"({self.process.returncode}): {detail}"
                    )
                try:
                    status, payload, _ = api.request(
                        "GET", "/v1/health", timeout=1
                    )
                    if status == 200 and isinstance(payload, dict):
                        return api
                except (OSError, ValueError) as exc:
                    last_error = exc
                time.sleep(0.1)
            raise TimeoutError("Gateway did not become healthy") from last_error
        except Exception:
            self.stop()
            raise

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self._log is not None:
            self._log.close()
            self._log = None

    def __enter__(self) -> Self:
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.stop()


class CliRunner:
    def __init__(self, artifacts: ArtifactStore, runner: CommandRunner) -> None:
        self.artifacts = artifacts
        self.runner = runner

    def run(
        self,
        base_url: str,
        arguments: list[str],
        *,
        log_name: str,
        check: bool = True,
        insecure: bool = False,
        agent_token: str | None = None,
    ) -> Any:
        environment = os.environ.copy()
        environment["ESP_IRIS_PROFILE_FILE"] = str(
            self.artifacts.private / "cli-profiles.json"
        )
        if agent_token:
            environment["ESP_IRIS_AGENT_TOKEN"] = agent_token
        result = self.runner.run(
            [
                sys.executable,
                TOOLS / "esp_iris.py",
                "ctl",
                "--url",
                base_url,
                "--json",
                *(["--insecure"] if insecure else []),
                *arguments,
            ],
            cwd=TOOLS,
            env=environment,
            timeout=120,
            log_name=log_name,
            check=check,
        )
        text = result.stdout.strip()
        return json.loads(text) if text else None

    def run_top(
        self,
        arguments: list[str],
        *,
        log_name: str,
        check: bool = True,
        env: Mapping[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        if env:
            environment.update(env)
        return self.runner.run(
            [sys.executable, TOOLS / "esp_iris.py", *arguments],
            cwd=TOOLS,
            env=environment,
            timeout=120,
            log_name=log_name,
            check=check,
        )


class PlaywrightRunner:
    def __init__(self, artifacts: ArtifactStore, runner: CommandRunner) -> None:
        self.artifacts = artifacts
        self.runner = runner

    def run_hardware(self, base_url: str, device_id: str) -> None:
        self.runner.run(
            ["npm", "run", "build"],
            cwd=TOOLS / "frontend",
            timeout=300,
            log_name="frontend-build-hardware.log",
        )
        environment = os.environ.copy()
        environment.update(
            {
                "ESP_IRIS_E2E_BASE_URL": base_url,
                "ESP_IRIS_TEST_URL": base_url,
                "ESP_IRIS_E2E_DEVICE_ID": device_id,
                "ESP_IRIS_E2E_SCREENSHOT": str(
                    self.artifacts.root / "workbench-hardware.png"
                ),
            }
        )
        self.runner.run(
            ["npm", "run", "test:e2e", "--", "hardware.spec.ts"],
            cwd=TOOLS / "frontend",
            env=environment,
            timeout=300,
            log_name="playwright-hardware.log",
        )

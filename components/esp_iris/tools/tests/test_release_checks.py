"""Release gates must fail closed when requested evidence is missing."""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


def test_budget_checker_rejects_missing_and_oversized_artifacts(tmp_path):
    script = Path(__file__).resolve().parents[4] / "tools/check_esp_iris_budgets.py"
    spec = importlib.util.spec_from_file_location("budget_checker", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    budgets = {"artifacts_bytes": {"firmware.bin": 4}}
    assert module.check(tmp_path, budgets, ["artifacts_bytes"])[0]["error"] == "missing"
    (tmp_path / "firmware.bin").write_bytes(b"12345")
    assert not module.check(tmp_path, budgets, ["artifacts_bytes"])[0]["passed"]
    (tmp_path / "firmware.bin").write_bytes(b"1234")
    assert module.check(tmp_path, budgets, ["artifacts_bytes"])[0]["passed"]


def test_release_index_rejects_incomplete_boot_chain(tmp_path):
    script = Path(__file__).resolve().parents[4] / "tools/release_evidence.py"
    config = tmp_path / "sdkconfig"
    config.write_text('CONFIG_IDF_TARGET="esp32s31"\n')
    (tmp_path / "project_description.json").write_text(json.dumps({
        "config_file": str(config), "target": "esp32s31", "project_version": "test",
        "idf_path": str(tmp_path), "app_bin": "app.bin", "app_elf": "app.elf",
    }))
    for name in ("app.bin", "app.elf", "app.map"):
        (tmp_path / name).write_bytes(b"fixture")
    result = subprocess.run([
        sys.executable, str(script), "--build", str(tmp_path),
        "--idf-path", str(tmp_path), "--output", str(tmp_path / "report.json"),
    ], text=True, capture_output=True, check=False, timeout=30)
    assert result.returncode != 0
    assert "missing bootloader BIN" in result.stderr
    assert not (tmp_path / "report.json").exists()

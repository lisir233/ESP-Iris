from __future__ import annotations

import pathlib
from datetime import UTC, datetime

import pytest


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("ESP-Iris local HIL/E2E")
    group.addoption(
        "--iris-e2e",
        action="store_true",
        default=False,
        help="enable destructive ESP-Iris hardware end-to-end tests",
    )
    group.addoption(
        "--iris-chip-mac",
        help="required physical target MAC, for example 30:ed:a0:f4:0c:28",
    )
    group.addoption(
        "--iris-program-port",
        help="programming/console endpoint, for example COM8",
    )
    group.addoption(
        "--iris-app-port",
        help="optional application USB CDC endpoint; auto-discovered otherwise",
    )
    group.addoption(
        "--iris-artifacts",
        type=pathlib.Path,
        help="evidence directory; defaults to test_results/e2e/<UTC run id>",
    )


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers", "iris_e2e: destructive local ESP-Iris hardware test"
    )
    config.addinivalue_line(
        "markers", "iris_stage(number): stable order for stateful HIL stages"
    )
    config.addinivalue_line(
        "markers", "firmware_profile(name): firmware required by a HIL test"
    )
    if config.getoption("--iris-e2e"):
        run_id = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
        configured = config.getoption("--iris-artifacts")
        repository = pathlib.Path(__file__).resolve().parents[3]
        artifacts = (
            configured.resolve()
            if configured
            else repository / "test_results" / "e2e" / run_id
        )
        artifacts.mkdir(parents=True, exist_ok=True)
        config._iris_e2e_run_id = run_id
        config._iris_e2e_artifacts = artifacts
        if not getattr(config.option, "xmlpath", None):
            config.option.xmlpath = str(artifacts / "junit.xml")


def pytest_collection_modifyitems(
    config: pytest.Config, items: list[pytest.Item]
) -> None:
    enabled = bool(config.getoption("--iris-e2e"))
    skipped = pytest.mark.skip(reason="requires explicit --iris-e2e")
    for item in items:
        if item.get_closest_marker("iris_e2e") and not enabled:
            item.add_marker(skipped)

    def order(item: pytest.Item) -> tuple[int, int, str]:
        if not item.get_closest_marker("iris_e2e"):
            return (0, 0, item.nodeid)
        marker = item.get_closest_marker("iris_stage")
        stage = int(marker.args[0]) if marker and marker.args else 999
        return (1, stage, item.nodeid)

    items[:] = sorted(items, key=order)


def pytest_sessionfinish(session: pytest.Session, exitstatus: int) -> None:
    store = getattr(session.config, "_iris_e2e_store", None)
    if store is not None:
        store.finish("passed" if exitstatus == 0 else "failed")

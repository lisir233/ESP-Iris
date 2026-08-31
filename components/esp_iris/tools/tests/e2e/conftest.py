from __future__ import annotations

from collections.abc import Iterator

import pytest

from .artifacts import ArtifactStore
from .config import E2EConfig
from .gateway import CliRunner, PlaywrightRunner
from .hardware import BoardController
from .runner import CommandRunner


@pytest.fixture(scope="session")
def iris_e2e_config(request: pytest.FixtureRequest) -> E2EConfig:
    return E2EConfig.from_pytest(request)


@pytest.fixture(scope="session")
def iris_artifacts(
    iris_e2e_config: E2EConfig, request: pytest.FixtureRequest
) -> Iterator[ArtifactStore]:
    store = ArtifactStore(
        iris_e2e_config.artifacts, iris_e2e_config.secrets.redactions
    )
    request.config._iris_e2e_store = store
    if getattr(request.config.option, "xmlpath", None) is None:
        request.config.option.xmlpath = str(store.root / "junit.xml")
    yield store


@pytest.fixture(scope="session")
def iris_runner(iris_artifacts: ArtifactStore) -> CommandRunner:
    return CommandRunner(iris_artifacts)


@pytest.fixture(scope="session")
def iris_board(
    iris_e2e_config: E2EConfig,
    iris_artifacts: ArtifactStore,
    iris_runner: CommandRunner,
) -> Iterator[BoardController]:
    board = BoardController(iris_e2e_config, iris_artifacts, iris_runner)
    completed = False
    cleanup_error: Exception | None = None
    try:
        details = board.preflight()
        iris_artifacts.write_json("preflight.json", details)
        unfinished = board.unfinished_journal()
        if unfinished is not None and unfinished.get("state") != "nvs-backed-up":
            board.build_all()
            restored = board.recover_unfinished(unfinished)
            iris_artifacts.record_stage(
                "recover-unfinished", "passed", restored=restored
            )
        elif unfinished is not None:
            board.clear_journal()
        nvs_sha256 = board.backup_nvs()
        baseline = board.capture_baseline_identity()
        iris_artifacts.write_json("baseline.json", baseline)
        iris_artifacts.record_stage(
            "preflight",
            "passed",
            nvs_sha256=nvs_sha256,
            original_device_id=baseline["device_id"],
            **details,
        )
        board.build_all()
        iris_artifacts.record_stage("build-all", "passed")
        yield board
        completed = True
    finally:
        try:
            if board.mutation_started and board.nvs_backup.exists():
                endpoint = board.restore_baseline()
                iris_artifacts.record_stage(
                    "restore-baseline", "passed", endpoint=endpoint
                )
        except Exception as exc:  # noqa: BLE001 - cleanup must preserve evidence
            iris_artifacts.record_stage(
                "restore-baseline", "failed", error=str(exc)
            )
            completed = False
            cleanup_error = exc
        finally:
            if not board.mutation_started:
                board.clear_journal()
            board.close()
            iris_artifacts.finish("passed" if completed else "failed")
        if cleanup_error is not None:
            raise cleanup_error


@pytest.fixture(scope="session")
def iris_cli(
    iris_artifacts: ArtifactStore, iris_runner: CommandRunner
) -> CliRunner:
    return CliRunner(iris_artifacts, iris_runner)


@pytest.fixture(scope="session")
def iris_playwright(
    iris_artifacts: ArtifactStore, iris_runner: CommandRunner
) -> PlaywrightRunner:
    return PlaywrightRunner(iris_artifacts, iris_runner)


@pytest.fixture
def firmware_profile(iris_board: BoardController, request: pytest.FixtureRequest):
    marker = request.node.get_closest_marker("firmware_profile")
    if marker is None or not marker.args:
        raise RuntimeError("firmware_profile fixture requires @pytest.mark.firmware_profile")
    name = str(marker.args[0])
    iris_board.flash(name)
    return name

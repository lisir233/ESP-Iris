from __future__ import annotations

import json

import pytest

from .config import PROFILES

pytestmark = [pytest.mark.iris_e2e, pytest.mark.iris_stage(0)]


def test_preflight_builds_every_profile_before_flashing(
    iris_board, iris_artifacts
) -> None:
    assert set(iris_board._built) == set(PROFILES)
    manifest = json.loads(
        (iris_artifacts.root / "manifest.json").read_text(encoding="utf-8")
    )
    assert set(manifest["profiles"]) == set(PROFILES)
    for profile in manifest["profiles"].values():
        assert profile["partition_remaining_bytes"] >= 64 * 1024
        assert len(profile["sha256"]) == 64
        assert profile["project_name"]
        assert profile["project_version"]

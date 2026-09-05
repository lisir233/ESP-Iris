from __future__ import annotations

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from iris_gateway.cli import build_parser
from iris_gateway.compatibility import (
    compatibility_expectation,
    validate_update_compatibility,
)
from iris_gateway.gateway import GatewayService

CONTRACT = {"chip_target": "esp32s31", "product_contract": "esp-mosaico/v1",
            "board_id": "esp-mosaico", "layout_id": "mosaico-retained-recovery-v1",
            "recovery_abi": 1}


@pytest.mark.parametrize("key,bad", [
    ("firmware_mode", "unknown"), ("chip_target", "esp32c3"),
    ("product_contract", "another/v1"), ("board_id", "another"),
    ("layout_id", "another"), ("recovery_abi", 2), ("recovery_abi", True),
])
@pytest.mark.parametrize("system_update", [False, True])
@pytest.mark.parametrize("missing", [False, True])
def test_preflight_rejects_missing_or_mismatched_fields_before_any_mutation(
    key, bad, system_update, missing
) -> None:
    async def scenario() -> None:
        status = {**CONTRACT, "firmware_mode": "normal", "boot_id": 1}
        if missing:
            status.pop(key)
        else:
            status[key] = bad
        service = GatewayService.__new__(GatewayService)
        service.hub = AsyncMock()
        service.hub.status.return_value = status
        service.operations = AsyncMock()
        service.preserve_coredump = AsyncMock()
        with pytest.raises(ValueError):
            if system_update:
                await service.closed_loop_system_update(
                    "device", SimpleNamespace(chip_id=0x20), "op", CONTRACT
                )
            else:
                await service.closed_loop_ota(
                    "device", b"image", {"chip_id": 0x20}, "op",
                    execution_mode="application", compatibility=CONTRACT,
                )
        service.hub.enter_recovery.assert_not_called()
        service.hub.ota_update.assert_not_called()
        service.hub.system_update.assert_not_called()
        service.preserve_coredump.assert_not_called()

    asyncio.run(scenario())


@pytest.mark.parametrize("system_update", [False, True])
def test_recovery_must_preserve_normal_contract_before_writer(system_update) -> None:
    async def scenario() -> None:
        before = {**CONTRACT, "firmware_mode": "normal", "boot_id": 1}
        recovery = {**CONTRACT, "firmware_mode": "recovery", "boot_id": 2,
                    "board_id": "different-board"}
        service = GatewayService.__new__(GatewayService)
        service.hub = AsyncMock()
        service.hub.status.side_effect = [before, recovery]
        service.operations = AsyncMock()
        service.preserve_coredump = AsyncMock(return_value=None)
        with pytest.raises(ValueError, match="board_id"):
            if system_update:
                await service.closed_loop_system_update(
                    "device", SimpleNamespace(chip_id=0x20), "op"
                )
            else:
                await service.closed_loop_ota(
                    "device", b"image", {"chip_id": 0x20}, "op"
                )
        service.hub.enter_recovery.assert_awaited_once()
        service.hub.ota_update.assert_not_called()
        service.hub.system_update.assert_not_called()

    asyncio.run(scenario())


def test_generic_requires_explicit_role_and_chip_but_no_product_claim() -> None:
    assert validate_update_compatibility(
        {"firmware_mode": "normal", "chip_target": "esp32s31"}, 0x20
    ) == {"chip_target": "esp32s31"}
    with pytest.raises(ValueError):
        validate_update_compatibility(
            {"firmware_mode": "normal", "chip_target": "esp32s31"}, 0x20,
            {"product_contract": "expected/v1"},
        )


@pytest.mark.parametrize("value", [{"typo": 1}, {"recovery_abi": True},
                                     {"board_id": ""}, [], {"board_id": "x" * 65}])
def test_expectation_is_bounded_and_strict(value) -> None:
    with pytest.raises((ValueError, TypeError)):
        compatibility_expectation(value)


@pytest.mark.parametrize("command", ["ota", "system-update"])
def test_cli_accepts_explicit_compatibility(command) -> None:
    args = build_parser().parse_args([
        "ctl", command, "device", "artifact", "--compatibility-json",
        '{"chip_target":"esp32s31","recovery_abi":1}',
    ])
    assert args.compatibility == {"chip_target": "esp32s31", "recovery_abi": 1}

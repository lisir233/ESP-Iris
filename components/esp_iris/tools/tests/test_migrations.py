from __future__ import annotations

import sqlite3

import pytest

from iris_gateway.migrations import LATEST_SCHEMA_VERSION, apply_migrations
from iris_gateway.store import GatewayStore


def test_fresh_store_applies_all_migrations(tmp_path) -> None:
    store = GatewayStore(tmp_path)
    assert store.schema_version == LATEST_SCHEMA_VERSION
    columns = {
        row["name"] for row in store.db.execute("PRAGMA table_info(operations)")
    }
    assert "progress_json" in columns
    store.close()


def test_legacy_schema_is_upgraded_transactionally(tmp_path) -> None:
    path = tmp_path / "legacy.sqlite3"
    db = sqlite3.connect(path)
    db.execute(
        "CREATE TABLE operations (operation_id TEXT PRIMARY KEY, status TEXT NOT NULL)"
    )
    db.execute("PRAGMA user_version=1")
    db.commit()
    assert apply_migrations(db) == LATEST_SCHEMA_VERSION
    columns = {row[1] for row in db.execute("PRAGMA table_info(operations)")}
    assert "progress_json" in columns
    assert int(db.execute("PRAGMA user_version").fetchone()[0]) == LATEST_SCHEMA_VERSION
    db.close()


def test_newer_database_is_never_opened_by_older_gateway() -> None:
    db = sqlite3.connect(":memory:")
    db.execute(f"PRAGMA user_version={LATEST_SCHEMA_VERSION + 1}")
    with pytest.raises(RuntimeError, match="newer than supported"):
        apply_migrations(db)


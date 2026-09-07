"""Build an unsigned System Update bundle without Gateway dependencies."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from iris_gateway.system_update import (
    PARTITION_TABLE_REGION_BYTES,
    build_system_update_bundle,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--component-root", type=pathlib.Path, required=True)
    parser.add_argument("--target-layout", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        manifest = json.loads(arguments.manifest.read_text(encoding="utf-8"))
        target_layout = arguments.target_layout.read_bytes()
        if not target_layout or len(target_layout) > PARTITION_TABLE_REGION_BYTES:
            raise ValueError("target layout must fit one non-empty 4 KiB sector")
        target_layout = target_layout.ljust(PARTITION_TABLE_REGION_BYTES, b"\xff")
        manifest["target_layout_sha256"] = hashlib.sha256(target_layout).hexdigest()
        output = build_system_update_bundle(
            arguments.output, manifest, arguments.component_root
        )
    except (OSError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"system-update bundle error: {error}", file=sys.stderr)
        return 2
    print(f"system-update bundle ready: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

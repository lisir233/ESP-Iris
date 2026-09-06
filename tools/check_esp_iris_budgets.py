#!/usr/bin/env python3
"""Check reviewed budgets; missing requested artifacts are failures, never skips."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(root, budgets, groups):
    results = []
    for group in groups:
        if not budgets.get(group):
            results.append({"group": group, "error": "empty or missing group", "passed": False})
            continue
        for name, limit in budgets[group].items():
            path = root / name
            if not path.exists():
                results.append({"path": name, "group": group, "error": "missing", "passed": False})
                continue
            if (group == "directory_bytes" and not path.is_dir()) or (group != "directory_bytes" and not path.is_file()):
                results.append({"path": name, "group": group, "error": "wrong path type", "passed": False})
                continue
            if group == "source_lines":
                measured = len(path.read_text(encoding="utf-8").splitlines())
            elif group == "directory_bytes":
                measured = sum(item.stat().st_size for item in path.rglob("*") if item.is_file())
            else:
                measured = path.stat().st_size
            results.append({"path": name, "group": group, "measured": measured,
                                "limit": limit, "passed": measured <= limit})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", action="append", choices=["source_lines", "artifacts_bytes", "directory_bytes"])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    budgets = json.loads((ROOT / "components/esp_iris/resource_budgets.json").read_text())
    results = check(ROOT, budgets, args.group or ["source_lines"])
    report = json.dumps({"schema": "esp-iris-budget-result/v1", "results": results}, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report + "\n", encoding="utf-8")
    print(report)
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

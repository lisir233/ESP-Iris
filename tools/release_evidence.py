#!/usr/bin/env python3
"""Index exact firmware artifacts and tool revisions for a release build."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git_revision(path):
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="append", type=Path, required=True)
    parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.idf_path:
        parser.error("--idf-path or IDF_PATH is required")
    profiles = []
    for directory in args.build:
        artifacts = []
        description_path = directory / "project_description.json"
        if not description_path.is_file():
            parser.error(f"missing project_description.json in {directory}")
        description = json.loads(description_path.read_text(encoding="utf-8"))
        if Path(description.get("idf_path", "")).resolve() != args.idf_path.resolve():
            parser.error(f"IDF path differs from build description in {directory}")
        config = Path(description["config_file"])
        if not config.is_file():
            parser.error(f"missing sdkconfig: {config}")
        evidence_files = [description_path, config]
        app_bin = directory / description["app_bin"]
        app_elf = directory / description["app_elf"]
        for path in (app_bin, app_elf, app_elf.with_suffix(".map")):
            if not path.is_file():
                parser.error(f"missing declared application artifact: {path}")
            evidence_files.append(path)
        for subdirectory in ("bootloader", "partition_table"):
            matches = sorted((directory / subdirectory).glob("*.bin"))
            if not matches:
                parser.error(f"missing {subdirectory} BIN in {directory}")
            evidence_files.extend(matches)
        for path in evidence_files:
            artifacts.append({"path": path.as_posix(), "bytes": path.stat().st_size,
                              "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        profiles.append({"build": directory.as_posix(), "target": description["target"],
                         "project_version": description["project_version"], "artifacts": artifacts})
    dirty = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"], text=True)
    source_names = subprocess.check_output([
        "git", "-C", str(ROOT), "ls-files", "-z", "--cached", "--others", "--exclude-standard",
    ]).decode("utf-8").split("\0")
    source_files = [{"path": name, "sha256": hashlib.sha256((ROOT / name).read_bytes()).hexdigest()}
                    for name in sorted(set(source_names)) if name and (ROOT / name).is_file()]
    report = {"schema": "esp-iris-release-evidence/v1", "revision": git_revision(ROOT),
                  "worktree_dirty": bool(dirty.strip()), "idf_revision": git_revision(args.idf_path),
                  "source_diff_sha256": hashlib.sha256(subprocess.check_output(
                      ["git", "-C", str(ROOT), "diff", "HEAD", "--binary"])).hexdigest(),
                  "source_files": source_files,
                  "profiles": profiles, "hardware_validation": "not-performed-by-this-tool"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(str(args.output))


if __name__ == "__main__":
    main()

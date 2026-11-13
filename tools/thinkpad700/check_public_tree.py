#!/usr/bin/env python3
"""Reject private runtime files in the Git index (no third-party dependencies)."""

from pathlib import PurePosixPath
import subprocess
import sys


def main():
    tracked = subprocess.check_output(["git", "ls-files", "-z"])
    private_roots = {"local-assets", "private-dist", "build", "out"}
    private_suffixes = {".rom", ".bin", ".ima", ".img", ".vhd", ".vhdx",
                        ".vdi", ".qcow2", ".exe", ".dll", ".7z", ".zip",
                        ".xwd", ".log", ".dmp"}
    problems = []
    paths = [name.decode("utf-8") for name in tracked.split(b"\0") if name]
    for name in paths:
        path = PurePosixPath(name)
        if path.parts[0].lower() in private_roots or path.suffix.lower() in private_suffixes:
            problems.append(name)
    if problems:
        print("Private/generated files must stay outside the public Git tree:", file=sys.stderr)
        print("\n".join(problems), file=sys.stderr)
        return 1
    print(f"Public-tree check passed ({len(paths)} indexed files; no runtime assets).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

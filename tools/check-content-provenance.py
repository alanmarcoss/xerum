#!/usr/bin/env python3
"""Release check: xerum ships only content we authored.

Implements the enforcement half of `docs/content-policy.md` (ticket 16, built by ticket 34):

1. **Provenance.** Every file under the content root appears in `content/manifest.json` with a
   non-empty `origin` and `licence`, and every manifest entry names a file that exists. Adding content
   without saying where it came from fails the release.
2. **Byte-identity.** Where the reference instrument is installed, no shipped content file may be
   byte-identical to any of its files. This is the check that would actually catch a mistake, so it
   runs where it can and **skips loudly** where it cannot -- never silently passing.

Exit status: 0 pass (a skip is still a pass, but it is printed), 1 a check failed, 2 bad usage.

    python3 tools/check-content-provenance.py [--content DIR] [--manifest PATH]
                                              [--reference-root DIR] [--json]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

REFERENCE_ROOT = Path("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/")
DEFAULT_MANIFEST_NAME = "manifest.json"


def relpath(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def files_under(root: Path):
    for directory, subdirectories, names in os.walk(root):
        subdirectories.sort()
        for name in sorted(names):
            yield Path(directory) / name


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            hasher.update(block)
    return hasher.hexdigest()


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.notes: list[str] = []
        self.skipped: list[str] = []

    def fail(self, message: str) -> None:
        self.errors.append(message)

    def note(self, message: str) -> None:
        self.notes.append(message)

    def skip(self, message: str) -> None:
        self.skipped.append(message)


def check_manifest(content: Path, manifest_path: Path, report: Report):
    """Returns the {relative path: entry} mapping, or None if the manifest cannot be read."""
    if not manifest_path.is_file():
        report.fail(
            f"no provenance manifest at {manifest_path}: content cannot ship without one "
            "(docs/content-policy.md)"
        )
        return None

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        report.fail(f"{manifest_path} is not readable JSON: {error}")
        return None

    tables = manifest.get("tables")
    if not isinstance(tables, list):
        report.fail(f"{manifest_path} has no 'tables' list")
        return None

    declared: dict[str, dict] = {}

    for index, entry in enumerate(tables):
        if not isinstance(entry, dict):
            report.fail(f"{manifest_path}: tables[{index}] is not an object")
            continue

        path = entry.get("path")
        if not isinstance(path, str) or not path:
            report.fail(f"{manifest_path}: tables[{index}] has no path")
            continue

        if path in declared:
            report.fail(f"{manifest_path}: {path} is declared twice")
            continue

        if not str(entry.get("origin") or "").strip():
            report.fail(f"{manifest_path}: {path} has no origin")

        if not str(entry.get("licence") or "").strip():
            report.fail(f"{manifest_path}: {path} has no licence")

        declared[path] = entry

    return declared


def check_files(content: Path, declared: dict[str, dict], manifest_path: Path, report: Report) -> None:
    shipped = [path for path in files_under(content) if path != manifest_path]

    for path in shipped:
        relative = relpath(path, content)

        if relative not in declared:
            report.fail(
                f"{relative} ships but is absent from the manifest: content is added through "
                "tools/wavetable-author.cpp, and every shipped file must say where it came from"
            )
            continue

        entry = declared[relative]
        recorded = entry.get("sha256")

        if isinstance(recorded, str) and recorded:
            actual = digest(path)

            if actual != recorded:
                report.fail(
                    f"{relative} does not match the manifest: sha256 {actual} on disk, {recorded} recorded"
                )

        size = entry.get("bytes")
        if isinstance(size, int) and size != path.stat().st_size:
            report.fail(
                f"{relative} does not match the manifest: {path.stat().st_size} bytes on disk, {size} recorded"
            )

    for relative in sorted(set(declared) - {relpath(path, content) for path in shipped}):
        report.fail(f"{relative} is declared in the manifest but does not exist under {content}")

    report.note(f"{len(shipped)} shipped content file(s) checked against {manifest_path.name}")


def check_reference(content: Path, reference_root: Path, report: Report) -> None:
    if not reference_root.is_dir():
        report.skip(
            f"reference instrument not installed at {reference_root}: the byte-identity check "
            "DID NOT RUN (this is the check that would catch a copied reference file)"
        )
        return

    # Byte-identical implies identical size, so index the reference by size and only hash candidates
    # that could possibly collide -- the tree holds thousands of files, several of them large.
    by_size: dict[int, list[Path]] = {}
    reference_files = 0

    for path in files_under(reference_root):
        try:
            size = path.stat().st_size
        except OSError:
            continue

        by_size.setdefault(size, []).append(path)
        reference_files += 1

    compared = 0
    identical: list[str] = []

    for path in files_under(content):
        candidates = by_size.get(path.stat().st_size)

        if not candidates:
            continue

        try:
            ours = path.read_bytes()
        except OSError as error:
            report.fail(f"{path} could not be read: {error}")
            continue

        for candidate in candidates:
            compared += 1

            try:
                if candidate.read_bytes() == ours:
                    identical.append(f"{path} == {candidate}")
            except OSError:
                continue

    if identical:
        for match in identical:
            report.fail(f"shipped content is byte-identical to a reference file: {match}")
    else:
        report.note(
            f"byte-identity: {reference_files} reference file(s) indexed, {compared} same-size "
            "candidate(s) compared, 0 identical"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--content", type=Path, default=Path("content"))
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--reference-root", type=Path, default=REFERENCE_ROOT)
    parser.add_argument("--json", action="store_true", help="print the result as JSON")
    arguments = parser.parse_args()

    content: Path = arguments.content

    if not content.is_dir():
        print(f"content root {content} does not exist", file=sys.stderr)
        return 2

    manifest_path: Path = arguments.manifest or content / DEFAULT_MANIFEST_NAME

    report = Report()
    declared = check_manifest(content, manifest_path, report)

    if declared is not None:
        check_files(content, declared, manifest_path, report)

    check_reference(content, arguments.reference_root, report)

    if arguments.json:
        print(json.dumps({
            "ok": not report.errors,
            "errors": report.errors,
            "notes": report.notes,
            "skipped": report.skipped,
        }, indent=2))
    else:
        for note in report.notes:
            print(f"check: {note}")

        for message in report.skipped:
            print(f"SKIP: {message}")

        for message in report.errors:
            print(f"FAIL: {message}", file=sys.stderr)

        if report.errors:
            print(f"content provenance: FAILED ({len(report.errors)} problem(s))", file=sys.stderr)
        else:
            print("content provenance: ok")

    return 1 if report.errors else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Release check: xerum ships only content we authored.

Implements the enforcement half of `docs/content-policy.md` (ticket 16, built by ticket 34):

1. **Provenance.** Every file under the content root appears in a manifest with a non-empty `origin`
   and `licence`, and every manifest entry names a file that exists. Adding content without saying where
   it came from fails the release. There are two manifests, because there are two authoring tools:
   `content/manifest.json` for the generated wavetables and `content/Factory/manifest.json` for the
   factory presets (ticket 54). A factory preset must also only name tables the table manifest declares,
   which is the self-containment rule stated where the build can enforce it.
2. **Byte-identity.** Where the reference instrument is installed, no shipped content file may be
   byte-identical to any of its files. This is the check that would actually catch a mistake, so it
   runs where it can and **skips loudly** where it cannot -- never silently passing. The same pass
   covers the factory bank's **names** (ticket 54): inventing our own names is a licence boundary, not
   a style preference, so no preset name may be one of the reference's.

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
# The factory bank's own manifest (ticket 54): its presets are content too, and content says where it
# came from. Kept as a second file rather than merged into the first because two tools write them, and a
# file with two writers is a file that eventually loses one of them.
BANK_MANIFEST_NAME = "Factory/manifest.json"


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


def check_bank_manifest(content: Path, report: Report):
    """The factory bank's own manifest: {relative path: entry}, and the tables its presets name.

    Returns ({}, set()) when there is no bank -- a content root without `Factory/` is a legitimate tree
    (a user's own content directory, for instance), and the bank is not something the release has to
    invent. A bank that exists and is malformed is an error, not a skip.
    """
    path = content / BANK_MANIFEST_NAME
    declared: dict[str, dict] = {}
    tables: set[str] = set()

    if not path.is_file():
        report.note(f"no factory bank at {path}: nothing to declare (ticket 54)")
        return declared, tables

    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        report.fail(f"{path} is not readable JSON: {error}")
        return declared, tables

    presets = manifest.get("presets")

    if not isinstance(presets, list):
        report.fail(f"{path} has no 'presets' list")
        return declared, tables

    # The browser reads this, so a bank whose own description is missing is a bank ticket 55 cannot
    # present; the check says so rather than leaving it to be discovered.
    if not isinstance(manifest.get("categories"), list) or not manifest["categories"]:
        report.fail(f"{path} has no 'categories' list: the bank does not say how it is browsed")

    for index, entry in enumerate(presets):
        if not isinstance(entry, dict):
            report.fail(f"{path}: presets[{index}] is not an object")
            continue

        relative = entry.get("path")

        if not isinstance(relative, str) or not relative:
            report.fail(f"{path}: presets[{index}] has no path")
            continue

        if relative in declared:
            report.fail(f"{path}: {relative} is declared twice")
            continue

        if not str(entry.get("origin") or "").strip():
            report.fail(f"{path}: {relative} has no origin")

        if not str(entry.get("licence") or "").strip():
            report.fail(f"{path}: {relative} has no licence")

        declared[relative] = entry

        for table in entry.get("tables") or []:
            if not isinstance(table, str) or not table:
                report.fail(f"{path}: {relative} names a table that is not a string")
                continue

            tables.add("Tables/" + table)

    report.note(f"{len(declared)} factory preset(s) and {len(tables)} table reference(s) declared "
                f"in {BANK_MANIFEST_NAME}")

    return declared, tables


def check_files(content: Path, declared: dict[str, dict], manifest_path: Path, report: Report) -> None:
    manifest_paths = {manifest_path, content / BANK_MANIFEST_NAME}
    shipped = [path for path in files_under(content) if path not in manifest_paths]

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

    report.note(f"{len(shipped)} shipped content file(s) checked against the manifest")


def check_bank_names(content: Path, reference_root: Path, report: Report) -> None:
    """No factory preset may be named after one of the reference's.

    The reference's preset *file names* are their display names, so the comparison is exact. A collision
    would mean a name was taken from its bank, which is the one thing the content policy forbids outright
    -- the bank's names are ours and the register is deliberately our own.
    """
    path = content / BANK_MANIFEST_NAME

    if not path.is_file():
        return

    if not reference_root.is_dir():
        report.skip(
            f"reference instrument not installed at {reference_root}: the factory bank's NAME check "
            "DID NOT RUN (this is the check that would catch a name taken from the reference)"
        )
        return

    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return   # already reported by check_bank_manifest

    ours = {str(entry.get("name", "")).strip().casefold()
            for entry in manifest.get("presets") or [] if isinstance(entry, dict)}
    ours.discard("")

    theirs: set[str] = set()
    presets = reference_root / "Presets"

    if not presets.is_dir():
        report.skip(f"no preset bank under {reference_root}: the name check DID NOT RUN")
        return

    for directory, subdirectories, names in os.walk(presets):
        subdirectories.sort()
        for name in names:
            if name.endswith(".SerumPreset"):
                theirs.add(Path(name).stem.strip().casefold())

    shared = sorted(ours & theirs)

    if shared:
        for name in shared:
            report.fail(f"factory preset name \"{name}\" is one of the reference's own preset names: "
                        "the bank's names are ours (docs/content-policy.md)")
    else:
        report.note(f"names: {len(ours)} factory preset name(s) compared against {len(theirs)} reference "
                    "preset name(s), 0 shared")


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
    bank, bank_tables = check_bank_manifest(content, report)

    if declared is not None:
        declared.update(bank)
        check_files(content, declared, manifest_path, report)

        # A factory preset may only play tables this repository ships: every table it names must be a
        # declare-and-present file in the content root, which is what makes the bank self-contained.
        for table in sorted(bank_tables - set(declared)):
            report.fail(f"a factory preset names {table}, which the content manifest does not declare: "
                        "the bank must play only tables xerum generates (docs/content-policy.md)")

    check_reference(content, arguments.reference_root, report)
    check_bank_names(content, arguments.reference_root, report)

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

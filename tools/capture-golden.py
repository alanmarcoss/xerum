#!/usr/bin/env python3
"""Capture the golden render corpus described by tests/golden/manifest.json.

The WAVs are deliberately **not** in this repository: they are renders of a commercial instrument and
live outside the checkout (see docs/content-policy.md). What ships is this script and the manifest, so
a licence later — or a claim — makes re-capturing a single command.

Every fixture is captured through tools/render-preset.sh --verify, which renders twice and requires the
two files to be byte-identical. An unverified capture is not evidence: ticket 20 found that a loaded
preset races its asynchronous asset load, so an unsettled render is not reproducible at all.

On success the manifest is rewritten with each fixture's sha256 and byte count, so a capture records
what it produced without recording the audio itself.

Usage:
    python3 tools/capture-golden.py [--manifest PATH] [--dir DIR] [--only SLUG] [--check]

  --check   verify the manifest's hashes against the files on disk without capturing
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", default=str(REPO / "tests/golden/manifest.json"))
    ap.add_argument("--dir", default=os.environ.get("XERUM_FIXTURE_DIR", str(Path.home() / ".xerum-fixtures")))
    ap.add_argument("--only", default=None, help="capture just this slug")
    ap.add_argument("--check", action="store_true", help="verify hashes instead of capturing")
    args = ap.parse_args(argv)

    manifest_path = Path(args.manifest)
    manifest = json.loads(manifest_path.read_text())
    cap = manifest["capture"]
    out_dir = Path(args.dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    script = REPO / "tools/render-preset.sh"

    failures = []
    captured = 0

    for fixture in manifest["fixtures"]:
        if args.only and fixture["slug"] != args.only:
            continue

        wav = out_dir / fixture["wav"]

        if args.check:
            if not wav.exists():
                failures.append(f"{fixture['slug']}: missing {wav}")
                continue
            got = sha256(wav)
            if got != fixture["sha256"]:
                failures.append(f"{fixture['slug']}: sha256 {got[:12]} != recorded {str(fixture['sha256'])[:12]}")
            else:
                print(f"  ok  {fixture['slug']}")
            continue

        cmd = [
            str(script),
            "--preset", fixture["preset"],
            "--out", str(wav),
            "--seconds", str(cap["seconds"]),
            "--verify",
            "--",
            "--sr", str(cap["sampleRate"]),
            "--block", str(cap["blockSize"]),
            "--note", str(cap["note"]),
            "--pre-roll", str(cap["preRoll"]),
            "--settle-ms", str(cap["settleMs"]),
        ]

        print(f"capture {fixture['slug']} …", flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True)

        if proc.returncode != 0:
            failures.append(f"{fixture['slug']}: exit {proc.returncode}: {proc.stderr.strip()[:200]}")
            continue

        if "verified: two renders are byte-identical" not in proc.stdout:
            failures.append(f"{fixture['slug']}: capture was not verified")
            continue

        fixture["sha256"] = sha256(wav)
        fixture["bytes"] = wav.stat().st_size
        captured += 1
        print(f"  {fixture['sha256'][:12]}  {fixture['bytes']} bytes")

    if not args.check:
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    print()
    if failures:
        print(f"FAILURES ({len(failures)}):", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    if args.check:
        print("all recorded hashes match")
    else:
        total = sum(f["bytes"] or 0 for f in manifest["fixtures"])
        print(f"captured {captured} fixture(s); corpus is {total / 1e6:.1f} MB in {out_dir}")
        print("(WAVs stay outside the repository — only the manifest is committed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

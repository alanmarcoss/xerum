#!/usr/bin/env python3
"""Generate src/model/generated/schema_data.h from the decoded parameter schema.

The output is a compact, *structural* table — module name patterns and one record per leaf — not a
dump of the reference's parameter values. Corpus statistics (occurrence counts, means, modes) are
deliberately dropped: they are facts about the factory bank, not part of the model.

Run it again when the schema changes:

    python3 tools/generate-schema.py \
        --schema .scratch/xerum/research/01-serum2-param-model.schema.json \
        --out    src/model/generated/schema_data.h

The input is a decoded schema of the reference's preset model, produced locally by the tracker's
tooling (see `.scratch/xerum/issues/21-parameter-model-and-schema.md`). It is not needed to build —
the generated header is committed so a fresh clone builds without a reference install.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

HEADER = """// GENERATED FILE — do not edit by hand.
//
// Produced by tools/generate-schema.py from a decoded schema of the reference preset model.
// Regenerate rather than patching: a hand edit here is invisible to the generator and will be
// silently lost the next time the schema changes.
//
// This file describes the SHAPE of the parameter model — module name patterns, one record per leaf,
// its kind, its observed range and enum set, and which schema versions it appears in. It contains no
// parameter values from any preset and no content of any kind.
//
// `defaultProvenance` is deliberately honest rather than convenient. A preset never stores a default,
// so no default can be read out of the corpus; the value observed most often in the factory bank is a
// MODE, not a default, and using it would be a guess wearing a number. Every leaf therefore starts as
// "unestablished" and must be authored and validated behaviourally before it may claim otherwise.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xerum::model::generated
{

inline constexpr std::string_view kOrigin =
    "%(origin)s";

inline constexpr std::string_view kVersions[] = {%(versions)s};

/// Module names as patterns: `{N}` stands for an index. The reference indexes its modules, so the
/// taxonomy is a handful of families rather than a list of 179 strings.
inline constexpr std::string_view kModulePatterns[] = {
%(patterns)s
};

struct Leaf
{
    std::string_view path;      ///< module path, `{N}` for an instance index, `[]` inside an array
    std::string_view kind;      ///< float | int | string | bool | null | empty-map | empty-array
    bool hasRange;
    double min;
    double max;
    std::uint8_t versionMask;   ///< bit 0 = 3.0, 1 = 4.0, 2 = 5.0, 3 = 6.0
    const std::string_view* values;  ///< enum set for string/int leaves, else nullptr
    std::uint16_t valueCount;
    std::string_view defaultProvenance;
};

%(valueArrays)s

inline constexpr Leaf kLeaves[] = {
%(leaves)s
};

inline constexpr std::size_t kLeafCount = sizeof (kLeaves) / sizeof (kLeaves[0]);

} // namespace xerum::model::generated
"""


def cxx_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--schema", default=".scratch/xerum/research/01-serum2-param-model.schema.json")
    ap.add_argument("--out", default="src/model/generated/schema_data.h")
    args = ap.parse_args()

    schema = json.loads(Path(args.schema).read_text())
    versions = sorted(schema["schemaVersions"]["versionLeafSets"].keys())
    leaves = schema["leaves"]

    # The taxonomy lists top-level module kinds, and it is **incomplete for normalisation**: it carries
    # `WTOsc{N}` but not `NoiseOsc{N}`, even though leaf paths use the latter. Normalisation decides
    # whether a trailing digit is an instance index by asking whether a pattern exists, so a missing
    # pattern turns a real path into one the schema cannot resolve — and every preset using that engine
    # then reports its parameters as unknown.
    #
    # So the pattern list is the union of the taxonomy and every indexed segment the leaf paths actually
    # use. Deriving it from the data rather than trusting one source is what keeps the two consistent.
    patterns = set(schema["moduleTaxonomy"]["kinds"])

    for path in leaves:
        for segment in path.split("/"):
            if segment.endswith("{N}"):
                patterns.add(segment)

    patterns = sorted(patterns)

    value_arrays: list[str] = []
    value_array_of: dict[int, tuple[str, int]] = {}
    rows: list[str] = []

    for index, path in enumerate(sorted(leaves)):
        rec = leaves[path]
        kind = rec["kind"]
        has_range = "min" in rec and "max" in rec and kind in ("float", "int")
        lo, hi = (rec.get("min", 0.0), rec.get("max", 0.0)) if has_range else (0.0, 0.0)

        absent = set(rec.get("versionsAbsentFrom", []))
        mask = 0
        for bit, v in enumerate(versions):
            if v not in absent:
                mask |= 1 << bit

        values_ref, count = "nullptr", 0
        # Only *string* leaves carry a genuine enumeration — the vocabulary that matters for naming
        # what we cannot reproduce. An int leaf's observed value set is corpus data, not an enum: a set
        # of `[0]` means the factory bank always wrote zero, which is range, not vocabulary.
        #
        # The guards below are load-bearing, not tidiness. `embedded_sfz` and friends have "values"
        # that are whole embedded asset payloads — multi-kilobyte SFZ text with newlines — and a naive
        # generator emits them straight into the header. That would publish reference content in a
        # committed file, which is exactly what docs/content-policy.md forbids. An enumeration is
        # short, has few members, and never contains a newline; anything else is data, and data does
        # not belong in the schema.
        if kind == "string" and rec.get("values"):
            vals = [str(v) for v in rec["values"]]
            # A vocabulary can be large (83 filter types, 138 module names) so the count limit is
            # generous; what it must never admit is *payload* — long strings, embedded newlines, or
            # asset paths. Those are data, not vocabulary, and the count alone would let them in.
            looks_like_payload = (len(vals) > 256
                                  or any(len(v) > 48 or "\n" in v or "\r" in v for v in vals))
            asset_bearing = any(w in path for w in ("embedded", "sfz", "Path", "path", "file"))
            if looks_like_payload or asset_bearing:
                print(f"  skipped non-enum value set: {path} ({len(vals)} values)")
            else:
                name = f"kValues{index}"
                value_arrays.append("inline constexpr std::string_view %s[] = {%s};"
                                    % (name, ", ".join(cxx_string(v) for v in vals)))
                value_array_of[index] = (name, len(vals))
                values_ref, count = name, len(vals)

        rows.append(
            "    { %s, %s, %s, %.10g, %.10g, %d, %s, %d, %s },"
            % (cxx_string(path), cxx_string(kind), "true" if has_range else "false", lo, hi,
               mask, values_ref, count, cxx_string("unestablished"))
        )

    origin = (
        "Decoded from the reference preset model locally; %d leaves, %d module patterns, "
        "schema versions %s. See tools/generate-schema.py."
        % (len(leaves), len(patterns), ", ".join(versions))
    )

    out = HEADER % {
        "origin": origin.replace('"', "'"),
        "versions": ", ".join(cxx_string(v) for v in versions),
        "patterns": "\n".join("    %s," % cxx_string(p) for p in patterns),
        "valueArrays": "\n".join(value_arrays),
        "leaves": "\n".join(rows),
    }

    dest = Path(args.out)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(out)
    print(f"wrote {dest} ({dest.stat().st_size} bytes)")
    print(f"  leaves {len(leaves)}   patterns {len(patterns)}   enum sets {len(value_arrays)}   versions {versions}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

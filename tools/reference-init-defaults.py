#!/usr/bin/env python3
"""Crosswalk the reference plugin's init parameter state onto the preset model's leaves.

Ticket 38. The answer to "where do defaults come from" is: the reference's own init state, read out
of the real plugin by `xerum-host --dump-params`. This tool does the half that is not plumbing —
pairing the plugin's automatable parameter surface with the preset model's leaf paths — and it
*measures* that pairing, the conversions between the two unit systems, and what is left over.

The two address spaces

  * the plugin exposes 2623 VST3 parameters. 2048 are per-channel MIDI CC slots, 32 are per-channel
    pitch-bend/aftertouch, one is the 128-slot program "Bank"; the remaining **543** are the genuine
    parameter surface, in the plugin's own unit tree ("OSC A", "Env 1"). Values are normalised 0..1
    with a per-parameter display string — the reference's own rendering of the engineering value
    ("0.5 ms", "-2", "PWM").
  * the preset model addresses 776 leaf paths ("Oscillator{N}/plainParams/kParamVolume"). Its values
    are engineering units: seconds, semitones, 0..1 faders, symbolic enum ids ("kPWM").

The pairing rule — measured, not assumed

  1. the plugin's unit group name gives the module family and, where the family is indexed, which
     instance ("OSC A" -> Oscillator{N} instance 0; "Env 3" -> Env{N} instance 2);
  2. within that module the local parameter name (the group's own prefix stripped) is matched
     against the leaf's last path segment with `kParam`/`kUI` dropped, case and punctuation ignored,
     preferring the family's own `plainParams/` over a subtype's (so "A Fine" is Oscillator{N}'s
     kParamFine, not NoiseOsc{N}'s);
  3. where the reference's UI vocabulary and its preset vocabulary disagree, an explicit synonym
     table names the rename ("Main Vol" -> kParamMasterVolume, "Semi" -> kParamCoarsePit).

  The rule is name-based because the *orders* do not line up: the plugin lists an envelope as
  Attack/Hold/Decay/Sustain/Release/curves, the schema's leaves are alphabetical. An ordinal rule
  pairs nothing, and is not used.

  The rule is *checked* by loading presets into the reference and observing which parameters move:
  a preset stores exactly the leaves that differ from its defaults, so a correctly paired parameter
  moves. `--observe` measures that.

Usage

    python3 tools/reference-init-defaults.py \
        --params   ~/.xerum-fixtures/params38/init.tsv \
        --schema   .scratch/xerum/research/01-serum2-param-model.schema.json \
        --manifest ~/.xerum-fixtures/params38/manifest.json \
        --defaults ~/.xerum-fixtures/params38/defaults.json \
        --report

    # pair checking: repeatable, PRESET_JSON,LOADED_TSV per preset loaded into the reference
    python3 tools/reference-init-defaults.py ... \
        --observe ~/.xerum-fixtures/params38/preset-a.json,~/.xerum-fixtures/params38/loaded-a.tsv \
        --observe ~/.xerum-fixtures/params38/preset-b.json,~/.xerum-fixtures/params38/loaded-b.tsv

    # a table the model layer can consume, emitted outside the repository
    python3 tools/reference-init-defaults.py ... --emit-header ~/.xerum-fixtures/params38/defaults_data.h

Nothing containing captured values may be committed: they are derived from the reference instrument
(ticket 10). The manifest of counts is the committable part; the defaults table and header are not.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------- the rule

# group name -> (module family, instance index or None, group prefix, subtype modules)
GROUPS = {
    "OSC A": ("Oscillator{N}", 0, "A", ["WTOsc{N}", "SampleOsc{N}", "GranularOsc{N}",
                                        "SpectralOsc{N}", "MultiSampleOsc{N}"]),
    "OSC B": ("Oscillator{N}", 1, "B", ["WTOsc{N}", "SampleOsc{N}", "GranularOsc{N}",
                                        "SpectralOsc{N}", "MultiSampleOsc{N}"]),
    "OSC C": ("Oscillator{N}", 2, "C", ["WTOsc{N}", "SampleOsc{N}", "GranularOsc{N}",
                                        "SpectralOsc{N}", "MultiSampleOsc{N}"]),
    "Noise OSC": ("Oscillator{N}", 3, "Noise", ["NoiseOsc{N}"]),
    "SUB OSC": ("Oscillator{N}", 4, "Sub", ["SubOsc4"]),
    "Filter 1": ("VoiceFilter{N}", 0, None, []),
    "Filter 2": ("VoiceFilter{N}", 1, None, []),
    "Env 1": ("Env{N}", 0, None, []),
    "Env 2": ("Env{N}", 1, None, []),
    "Env 3": ("Env{N}", 2, None, []),
    "Env 4": ("Env{N}", 3, None, []),
    "Macros": ("Macro{N}", None, None, []),
    "Mod Matrix": ("ModSlot{N}", None, None, []),
    "Routing Matrix": ("RoutingSlot{N}", None, None, []),
    "Arpeggiator": ("ArpClip{N}", None, None, []),
    "Clip Player": ("ClipPlayer{N}", None, None, []),
    "Voice Panel": ("VoicePanel{N}", None, None, []),
    "": ("Global{N}", 0, None, []),
    "MIDI Params": (None, None, None, []),
}
for _i in range(1, 11):
    GROUPS[f"LFO {_i}"] = ("LFO{N}", _i - 1, None, [])

# The reference's UI vocabulary -> its preset vocabulary: renames the mechanical pass cannot see.
SYNONYMS = {
    "mainvol": "mastervolume",
    "maintuning": "globaltuning",
    "amp": "voiceamp",
    "portatime": "portamentotime",
    "portacurve": "portamentocurve",
    "bendup": "bendrangeup",
    "benddown": "bendrangedn",
    "bus1vol": "fxbus1vol",
    "bus2vol": "fxbus2vol",
    "level": "volume",
    "semi": "coarsepit",
    "ratio": "pitchratio",
    "coarsepitch": "pitchmode",
    "scankeytrack": "scantempolock",
    "loopxfade": "loopcrossfade",
    "relativeloop": "loopstartlink",
    "singleslice": "slicingrootnote",
    "sliceplaymode": "manualpositionmode",
    "unistack": "unisonstack",
    "unidetune": "detune",
    "uniblend": "unisonstereo",
    "uniwidth": "detunewid",
    "unispan": "unisonspan",
    "unirandstart": "randomstart",
    "uniwarp": "unisonwarp",
    "uniwarp2": "unisonwarp2",
    "randphase": "randomphase",
    "phase": "initialphase",
    "noisephase": "initialphase",
    "noiserandphase": "randomphase",
    "warpmode": "warpmenu",
    "warp2mode": "warpmenu2",
    "wtpos": "tablepos",
    "uniwtpos": "unisonwtpos",
    "subshape": "shape",
    "subcontphase": "contiguousphase",
    "contphase": "contiguousphase",
    "on": "enable",
    "enable": "enabled",
    "res": "reso",
    "atkcurve": "curve1",
    "deccurve": "curve2",
    "relcurve": "curve3",
}

# Parameters whose group maps to one leaf whatever the local name is.
SPECIALS = {
    "Macros": "Macro{N}/plainParams/kParamValue",
    "Voice Panel": {
        "oscdetunernd": "VoicePanel{N}/plainParams/kParamGlobalRandomOscDetune",
        "oscpanrand": "VoicePanel{N}/plainParams/kParamGlobalRandomOscPan",
        "envrand": "VoicePanel{N}/plainParams/kParamGlobalRandomEnvTime",
        "cutoffrand": "VoicePanel{N}/plainParams/kParamGlobalRandomFilterCutoff",
    },
    "": {
        "key": "PitchQuantizer{N}/plainParams/kParamKey",
        "scale": "PitchQuantizer{N}/plainParams/kParamScale",
    },
}

# Routing matrix: source -> RoutingSlot instance; destination -> the leaf it feeds.
ROUTING_INDEX = {"A": 0, "B": 1, "C": 2, "Noise": 3, "Sub Osc": 4, "Filter 1": 5, "Filter 2": 6}
ROUTING_LEAF = {
    "filterbalance": "kParamFilterBalance",
    "bus1": "kParamFXBus1Level",
    "bus2": "kParamFXBus2Level",
}

# A rename that only applies inside one module: the filter's own "Level" is `kParamLevelOut`, while an
# oscillator's "Level" is `kParamVolume`. Verified by mutation — a preset storing 0.5402 for
# VoiceFilter0/plainParams/kParamLevelOut moves "Filter 1 Level" to exactly 0.5402.
SYNONYMS_BY_FAMILY = {
    ("VoiceFilter{N}", "level"): "levelout",
}

PREFIXES = (r"^Filter \d+ ", r"^Env \d+ ", r"^LFO \d+ ", r"^Mod \d+ ", r"^Macro \d+", r"^Arp ",
            r"^Clip Player ", r"^FX Main Param \d+$", r"^FX Bus [12] Param \d+$", r"^[^>]+>\s*")

# A group that exposes parameters from two modules: the arpeggiator's on/off lives on Arp{N}, while
# its rate, gate, chance and so on live on the selected ArpClip{N}. Searched in order.
EXTRA_SCOPES = {
    "Arpeggiator": [("Arp{N}", [])],
}

# A unit relation is only claimed from at least this many independent observations. Two points fit
# any line exactly, which is how a curved parameter (portamento time) once passed as affine.
MIN_OBSERVATIONS_FOR_A_FIT = 3
FIT_TOLERANCE = 1e-6


def normalise_name(s: str) -> str:
    s = re.sub(r"^(kParam|kUI)", "", s, flags=re.I)
    return re.sub(r"[^a-z0-9]", "", s.lower())


INDEXED: set[str] = set()


def normalise_path(p: str) -> str:
    """Concrete preset path -> schema path. Mirrors `Schema::normalise` for the constructs it needs:
    an index run naming an indexed module becomes `{N}`, a bracketed index becomes `[]`."""
    out = []
    for seg in p.lstrip("/").split("/"):
        m = re.fullmatch(r"(.+?)\[(\d+)\]", seg)
        if m:
            out.append(m.group(1) + "[]")
            continue
        m = re.fullmatch(r"([A-Za-z]+)(\d+)", seg)
        if m and m.group(1) in INDEXED:
            out.append(m.group(1) + "{N}")
            continue
        out.append(seg)
    return "/".join(out)


def instance_from_name(name: str) -> int | None:
    """The instance a per-instance group's parameter addresses, from the name: "Mod 37 Amount" is
    ModSlot 36, "Macro 2" is Macro 1."""
    m = re.match(r"^(Mod|Macro) (\d+)", name)
    return int(m.group(2)) - 1 if m else None


class Crosswalk:
    def __init__(self, schema_json: dict):
        self.schema = schema_json
        # The research schema still spells an enumerated first index (`lfo[0][]`, `embeddedIR[0][]`);
        # the committed table carries one canonical spelling per row (ticket 21). Canonicalise here
        # so figures are quoted against the same 776 leaves the model ships.
        leaves = collections.OrderedDict()
        for p, desc in schema_json["leaves"].items():
            leaves.setdefault(re.sub(r"\[\d+\]\[\]", "[]", p), desc)
        self.leaves = dict(leaves)
        global INDEXED
        INDEXED = {seg[:-3] for p in self.leaves for seg in p.split("/") if seg.endswith("{N}")}
        self.by_family: dict[str, dict[str, str]] = collections.defaultdict(dict)
        self.leaves_of_family: dict[str, list[str]] = collections.defaultdict(list)
        for p in self.leaves:
            fam = p.split("/")[0]
            self.leaves_of_family[fam].append(p)
            name = normalise_name(p.split("/")[-1])
            cur = self.by_family[fam].get(name)
            if cur is None or p.count("/") < cur.count("/"):
                self.by_family[fam][name] = p

    def local_name(self, group: str, name: str) -> str:
        tag = GROUPS[group][2]
        if tag is not None:
            return re.sub(r"^" + re.escape(tag) + r"\s+", "", name)
        for pat in PREFIXES:
            if re.match(pat, name):
                return re.sub(pat, "", name)
        return name

    def candidate_paths(self, family: str, subs: list[str]) -> list[str]:
        out = []
        for key in [f"{family}/plainParams/"] + [f"{family}/{s}/plainParams/" for s in subs]:
            out += [p for p in self.leaves_of_family[family] if p.startswith(key)]
        for key in [f"{family}/"] + [f"{family}/{s}/" for s in subs]:
            out += [p for p in self.leaves_of_family[family]
                    if p.startswith(key) and "/plainParams/" not in p]
        return out

    def pair_param(self, row: dict) -> tuple[str, int | None, str] | None:
        """-> (leaf, instance, confidence); confidence is 'exact', 'synonym' or 'editorial'."""
        group = row["group"]
        if group not in GROUPS:
            return None
        family, inst, _tag, subs = GROUPS[group]
        if family is None:
            return None
        if group == "Routing Matrix":
            m = re.match(r"^(.+?)>\s*(.+)$", row["name"])
            hit = None
            if m:
                leaf = ROUTING_LEAF.get(normalise_name(m.group(2)))
                idx = ROUTING_INDEX.get(m.group(1))
                if leaf and idx is not None:
                    hit = (f"RoutingSlot{{N}}/plainParams/{leaf}", idx, "synonym")
            return hit

        local = self.local_name(group, row["name"])
        key = normalise_name(local)
        special = SPECIALS.get(group)
        if isinstance(special, dict) and key in special:
            return special[key], inst, "editorial"
        if isinstance(special, str):
            return special, instance_from_name(row["name"]) if inst is None else inst, "editorial"

        mapped = SYNONYMS.get(key, key)
        scoped = SYNONYMS_BY_FAMILY.get((family, key))
        scopes = EXTRA_SCOPES.get(group, []) + [(family, subs)]
        for cand in (scoped, mapped, key):
            if cand is None:
                continue
            for fam, fam_subs in scopes:
                for path in self.candidate_paths(fam, fam_subs):
                    if normalise_name(path.split("/")[-1]) == cand:
                        return path, inst if inst is not None else instance_from_name(row["name"]), \
                            "exact" if cand == key else "synonym"
        return None


def classify_non_surface(name: str) -> str | None:
    if re.fullmatch(r"CC\d+ Chan \d+", name):
        return "midi-cc-slot"
    if re.fullmatch(r"(Pitch Bend|Aftertouch) Chan \d+", name):
        return "midi-channel-expression"
    return None


# --------------------------------------------------------------------------- io


def read_params(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text().splitlines():
        f = line.split("\t")
        if len(f) < 8:
            continue
        rows.append({"index": int(f[0]), "id": f[1], "group": f[2], "name": f[3],
                     "value": float(f[4]), "text": f[5], "steps": int(f[6]), "discrete": f[7] == "1"})
    return rows


def flatten(obj, prefix=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from flatten(v, f"{prefix}/{k}")
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from flatten(v, f"{prefix}[{i}]")
    else:
        yield prefix, obj


def concrete(leaf: str, inst: int | None) -> str:
    return leaf if inst is None else leaf.replace("{N}", str(inst))


def is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def fit_unit_model(observations: list[tuple[float, float]]) -> dict:
    """Fit the preset-units <-> plugin-normalised relation from observed (stored, normalised) pairs.

    Only affine relations are claimed, and only from MIN_OBSERVATIONS_FOR_A_FIT independent pairs:
    two pairs fit any line, which is how a curved parameter once passed as affine here.
    """
    n = len(observations)
    if n < MIN_OBSERVATIONS_FOR_A_FIT:
        return {"model": "unestablished", "reason": "too-few-observations", "observations": n}
    (v1, n1) = sorted(observations)[0]
    (v2, n2) = sorted(observations)[-1]
    if abs(n1 - n2) < 1e-9:
        return {"model": "unestablished", "reason": "no-spread", "observations": n}
    a = (v1 - v2) / (n1 - n2)
    b = v1 - a * n1
    resid = max(abs(a * nv + b - v) for v, nv in observations)
    scale = max(1.0, max(abs(v) for v, _ in observations))
    if resid > FIT_TOLERANCE * scale:
        return {"model": "unestablished", "reason": "not-affine", "observations": n,
                "residual": resid}
    return {"model": "affine", "observations": n, "residual": resid, "scale": a, "offset": b}


def invert(model: dict, normalised: float) -> float | None:
    if model.get("model") != "affine":
        return None
    value = model["scale"] * normalised + model["offset"]
    # A fitted affine map leaves float dust around a true zero (a warp amount came out as -3.8e-11).
    # Snap it, rather than hand ticket 21 a default of minus forty picobillionths.
    if abs (value) < 1e-9 * max (1.0, abs (model.get ("scale", 1.0))):
        return 0.0
    return value


# --------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--params", required=True)
    ap.add_argument("--schema", required=True)
    ap.add_argument("--manifest")
    ap.add_argument("--defaults")
    ap.add_argument("--emit-header")
    ap.add_argument("--observe", action="append", default=[],
                    metavar="PRESET_JSON,LOADED_TSV")
    ap.add_argument("--source-note", default="",
                    help="recorded in the manifest, e.g. where the capture was taken and whether "
                         "the user Default.SerumPreset slot was empty")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    rows = read_params(Path(args.params))
    schema = json.loads(Path(args.schema).read_text())
    xw = Crosswalk(schema)
    leaves = xw.leaves

    paired, surface, slots = {}, [], []
    for r in rows:
        kind = classify_non_surface(r["name"])
        if kind:
            slots.append((r, kind))
            continue
        surface.append(r)
        hit = xw.pair_param(r)
        if hit:
            paired[r["index"]] = hit

    # ---- observations: (leaf) -> [(preset value, plugin normalised value)], and the pairing check
    known = set(leaves)
    observations: dict[str, list[tuple[float, float]]] = collections.defaultdict(list)
    check = collections.Counter()
    unpaired_stored: set[str] = set()
    for spec in args.observe:
        preset_path, params_path = spec.split(",")
        preset = json.loads(Path(preset_path).read_text())
        stored = {}
        for k, v in flatten(preset["payload"]):
            if k and normalise_path(k) in known:
                stored[k.lstrip("/")] = v
        init_by_index = {r["index"]: r for r in rows}
        loaded = {r["index"]: r for r in read_params(Path(params_path))}
        for i, (leaf, inst, _c) in paired.items():
            path = concrete(leaf, inst)
            moved = abs(init_by_index[i]["value"] - loaded[i]["value"]) > 1e-6
            if path in stored:
                check["storedAndMoved" if moved else "storedAndStill"] += 1
                if is_number(stored[path]):
                    observations[leaf].append((float(stored[path]), loaded[i]["value"]))
            elif moved:
                check["omittedButMoved"] += 1
        for path in stored:
            if path not in {concrete(l, i) for l, i, _ in paired.values()} and path in leaves:
                check["storedUnpaired"] += 1
                unpaired_stored.add(path)

    units = {leaf: fit_unit_model(ps) for leaf, ps in observations.items()}

    # Two checks that make the pairing and the "absence means default" claim measurable rather than
    # asserted: a correct pairing is monotone in (preset value, normalised value), and the reference's
    # own init value should differ from the factory bank's modal value — because a mode is not a
    # default.
    monotone_checked = monotone_ok = 0
    for ps in observations.values():
        if len(ps) < MIN_OBSERVATIONS_FOR_A_FIT:
            continue
        monotone_checked += 1
        s = sorted(ps)
        if all(s[i][1] <= s[i + 1][1] + 1e-9 for i in range(len(s) - 1)):
            monotone_ok += 1

    init_normalised: dict[str, float] = {}
    for i, (leaf, _inst, _c) in paired.items():
        init_normalised.setdefault(leaf, next(r for r in rows if r["index"] == i)["value"])
    mode_same = mode_differs = 0
    for leaf, model in units.items():
        implied = invert(model, init_normalised[leaf])
        mode = leaves[leaf].get("mode")
        if implied is None or not is_number(mode):
            continue
        if abs(implied - mode) <= 1e-6 * max(1.0, abs(mode)):
            mode_same += 1
        else:
            mode_differs += 1

    # ---- leaves with no plugin parameter addressing them
    addressed = {leaf for leaf, _, _ in paired.values()}
    unaddressed = {}
    for p, desc in leaves.items():
        if p in addressed:
            continue
        if any(t in p for t in ("/flex", "/curveData", "/pathData", "embedded", "[]", "/files/",
                                "/PZs", "/sliceMarkers", "/scale", "/clip/", "/activeClips",
                                "/storedPhasePos", "/source[]")):
            why = "payload"
        elif desc.get("kind") in ("empty-map", "empty-array", "null"):
            why = "structural"
        elif p.split("/")[0] in ("SerumGUI", "RetriggerState{N}", "midiMap", "lfoPointModAssignments",
                                 "tuningData", "tuningName", "arpBankDisplayName",
                                 "clipBankDisplayName", "scalars", "Osc{N}") or "/" not in p:
            why = "metadata"
        else:
            why = "scalar-with-no-automatable-counterpart"
        unaddressed[p] = why

    unpaired_why, unpaired_list = collections.Counter(), []
    for r in surface:
        if r["index"] in paired:
            continue
        if re.search(r"Param \d+$", r["name"]):
            why = "unnamed-slot"
        elif r["name"] == "Bank":
            why = "program-bank"
        elif r["name"] in ("Pitch Bend", "Mod Wheel", "Bypass"):
            why = "plugin-only-global"
        else:
            why = "name-not-matched"
        unpaired_why[why] += 1
        if why == "name-not-matched":
            unpaired_list.append((r["index"], r["group"], r["name"]))

    manifest = {
        "source": "reference plugin init state via xerum-host --dump-params",
        "sourceNote": args.source_note,
        "pluginParameters": len(rows),
        "midiCcSlots": sum(1 for _, k in slots if k == "midi-cc-slot"),
        "midiChannelExpression": sum(1 for _, k in slots if k == "midi-channel-expression"),
        "automatableSurface": len(surface),
        "surfacePairedToALeaf": len(paired),
        "surfacePairedByConfidence": dict(collections.Counter(c for _, _, c in paired.values())),
        "surfaceUnpaired": len(surface) - len(paired),
        "surfaceUnpairedByReason": dict(unpaired_why),
        "schemaLeaves": len(leaves),
        "leavesAddressedByAParameter": len(addressed),
        "leavesUnaddressed": len(unaddressed),
        "leavesUnaddressedByReason": dict(collections.Counter(unaddressed.values())),
        "pairingCheck": dict(check),
        "distinctStoredLeavesWithNoPairedParameter": len(unpaired_stored),
        "pairingMonotonicity": {"leavesChecked": monotone_checked, "monotone": monotone_ok},
        "initDefaultVersusCorpusMode": {"comparable": mode_same + mode_differs,
                                        "agree": mode_same, "differ": mode_differs},
        "unitModels": dict(collections.Counter(m["model"] for m in units.values())),
        "leavesWithUnitObservations": len(units),
    }

    # ---- the table: one entry per addressed leaf, with every instance's init value
    table: dict[str, dict] = {}
    for r in surface:
        hit = paired.get(r["index"])
        if not hit:
            continue
        leaf, inst, conf_ = hit
        entry = table.setdefault(leaf, {
            "path": leaf,
            "kind": leaves[leaf].get("kind"),
            "provenance": "reference-init",
            "instances": {},
        })
        entry["instances"]["*" if inst is None else str(inst)] = {
            "pluginParameter": r["name"],
            "pluginGroup": r["group"],
            "pluginParameterIndex": r["index"],
            "normalised": r["value"],
            "displayText": r["text"],
            "steps": r["steps"],
            "discrete": r["discrete"],
            "pairing": conf_,
        }

    counts = collections.Counter()
    for leaf, entry in table.items():
        values = {v["normalised"] for v in entry["instances"].values()}
        entry["instanceVaries"] = len(values) > 1
        entry["normalised"] = min(values)
        model = units.get(leaf)
        if model is None:
            model = {"model": "no-observations", "observations": 0}
        entry["unitModel"] = model
        entry["presetUnitDefault"] = invert(model, entry["normalised"])
        if entry["kind"] == "string":
            entry["valueState"] = "text-only"          # a double cannot carry an enum id
        elif entry["presetUnitDefault"] is not None:
            entry["valueState"] = "preset-units"
        else:
            entry["valueState"] = "normalised-only"
        counts[entry["valueState"]] += 1

    manifest["leavesAddressedByValueState"] = dict(counts)

    if args.manifest:
        Path(args.manifest).write_text(json.dumps(manifest, indent=1) + "\n")
    if args.defaults:
        Path(args.defaults).write_text(json.dumps(table, indent=1, sort_keys=True) + "\n")
    if args.emit_header:
        write_header(Path(args.emit_header), table, manifest)

    if args.report:
        print(json.dumps(manifest, indent=1))
        print(f"\nsurface parameters the name rule could not pair ({len(unpaired_list)}):")
        for i, g, n in unpaired_list:
            print(f"  [{i:>4}] {g!r:16} {n!r}")
    return 0


def write_header(out: Path, table: dict, manifest: dict) -> None:
    lines = [
        "// GENERATED FILE - do not edit by hand, and do not commit.",
        "//",
        "// Produced by tools/reference-init-defaults.py from a reference init state captured with",
        "// `xerum-host --dump-params`. The values are derived from the reference instrument, so this",
        "// file is generated locally and is not part of the repository (ticket 10).",
        "//",
        "// Regenerate rather than patching. `schema.cpp` consults it only when present:",
        "//   #if __has_include(\"generated/defaults_data.h\")",
        "//",
        f"// source: {manifest['source']}",
        f"// leaves: {len(table)}  value states: {json.dumps(manifest.get('leavesAddressedByValueState', {}))}",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <string_view>",
        "",
        "namespace xerum::model::generated",
        "{",
        "",
        "struct EstablishedDefault",
        "{",
        "    std::string_view path;         ///< normalised schema path",
        "    std::string_view kind;         ///< float | int | string | bool",
        "    bool hasValue;                 ///< false when only the reference's own text is known",
        "    double value;                  ///< in the preset model's units when hasValue",
        "    std::string_view referenceText; ///< the reference's own display of the value",
        "    std::string_view provenance;",
        "};",
        "",
        "inline constexpr EstablishedDefault kDefaults[] = {",
    ]
    for leaf in sorted(table):
        e = table[leaf]

        def cxx(s: str) -> str:
            return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

        has = e["presetUnitDefault"] is not None
        value = e["presetUnitDefault"] if has else e["normalised"]
        first = next(iter(e["instances"].values()))
        lines.append(
            f"    {{ {cxx(leaf)}, {cxx(e['kind'] or 'unknown')}, {'true' if has else 'false'}, "
            f"{value!r}, {cxx(first['displayText'])}, \"reference-init\" }},")
    lines += [
        "};",
        "",
        "inline constexpr std::size_t kDefaultCount = sizeof (kDefaults) / sizeof (kDefaults[0]);",
        "",
        "} // namespace xerum::model::generated",
        "",
    ]
    out.write_text("\n".join(lines))


if __name__ == "__main__":
    sys.exit(main())

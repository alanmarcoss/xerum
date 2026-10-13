# Preset import contract

What xerum guarantees when it loads a preset, and what it does when it cannot. Normative: **MUST**,
**MUST NOT**, **SHOULD**, **MAY** carry their usual weight. This document is the contract ticket `23`
implements and ticket `15` tests against; where it and the code disagree, the code is wrong.

Derived from `.scratch/xerum/issues/14-preset-import-contract.md`.

## 1. The guarantee

Loading a preset file **MUST**:

1. not fail, whatever the file contains;
2. produce a complete parameter document in model terms;
3. produce an **import report** (§3) enumerating everything xerum did not reproduce;
4. render audio reflecting every parameter the engine supports.

Import **MUST NOT** be described as, or tested for, sonic identity with the reference instrument. That
is a separate claim, measured against golden renders with a stated tolerance, and belongs to the
fidelity suite.

## 2. Silent drops

**The set of silently dropped items MUST be empty.** Every engine, warp family, filter family, effect
class, module, parameter and asset that a preset asks for and xerum cannot honour **MUST** appear in
the import report with its module path.

## 3. The import report

The report **MUST** be available programmatically, as text in the harness, and in the interface.

| Severity | Meaning | Examples |
|---|---|---|
| `high` | The preset's sound is materially wrong or missing. | An unsupported engine; an unsupported warp, filter or effect; an unresolved or substituted asset. |
| `medium` | Something was present that we did not interpret. | An unknown module, parameter, or a module from a newer schema version. |
| `info` | We coped, and how. | A path resolved case-insensitively; an embedded asset preferred over a path reference. |

Each entry carries severity, module path, item and reason. The report **MUST** also carry counts per
severity, so a caller can test "this preset imported cleanly" without walking entries.

A clean import **MUST** still produce a report — empty, not absent. "No report" and "nothing to
report" are different states and the code must not conflate them.

## 4. Load semantics

Loading **MUST** reset the document to schema defaults, then apply the preset's sparse overrides.

Loading **MUST NOT** merge onto the currently loaded values. Because absence means *default*, merging
would leave the previous preset's values in every parameter the new preset does not mention, producing
a hybrid present in no file and authored by no one.

## 5. Assets

A relative asset reference **MUST** be resolved against the first of these that applies:

1. the user-configured content root;
2. the reference instrument's content root, if it exists on the machine;
3. xerum's own content directory.

Resolution **MUST**:

- strip leading `/` and `//` before joining;
- accept both `/` and `\` as separators, since packs are often authored on Windows;
- try an exact match first, then case-insensitive; a case-insensitive hit **MUST** be reported at
  `info`;
- when nothing matches, report at `high` and substitute a documented default. It **MUST NOT** crash,
  and it **MUST NOT** substitute silently.

An embedded asset **MUST** take precedence over a path reference, because a preset carrying its own
data is self-contained.

## 6. Schema versions

Versions **3.0 through 6.0 MUST** be accepted.

An unknown or newer version **MUST NOT** be refused. The file **MUST** be parsed as far as the schema
allows, with everything unrecognised reported at `medium`. Refusing makes xerum useless against a
future release; silent tolerance hides the gap. Visible degradation is the intended third option.

## 7. Writing

xerum **MUST** round-trip its own documents: a document written to a container and read back **MUST**
compare equal.

When writing, xerum **MUST**:

- compute the preset hash as `md5hex` of the compressed payload stream, matching the reference, so a
  written preset can be keyed and rated compatibly;
- preserve metadata fields it does not interpret, verbatim. Destroying what we do not understand is a
  way to lose someone's preset notes.

Whether a xerum-written preset opens in the **reference instrument** is a stronger, separately scoped
claim (ticket `32`) and **MUST NOT** be assumed by anything in v1.

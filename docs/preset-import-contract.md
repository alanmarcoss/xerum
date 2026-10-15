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

Resolution **MUST** ask the filesystem nothing it cannot answer with a value:

- **no call on this path may throw.** A path that cannot be examined — a permission, a symlink loop, an
  I/O error — is an `error_code` and not an exception. A `filesystem_error` escaping the resolver used to
  reach the plugin's own constructor, which aborts the host that instantiated it; "**MUST NOT** crash"
  above is exactly that promise;
- **a root that refuses to be read is reported**, with the path that refused and why, and the roots after
  it are still asked: a refused root loses only what was inside it;
- **no content-root filesystem call runs on the thread that asked for it.** Resolution is handed to a
  worker (`src/content/off_thread_work.h`) and the caller waits a bounded time. A walk of a handful of
  local files finishes well inside that bound, so an ordinary load is unchanged; a root that will not
  answer — a consent prompt nobody has answered, a volume that has gone to sleep, a drive that is not
  spinning — costs the caller the bound and no more. A resolution that lands after the bound is published
  when it lands, with its own report entries;
- while a slot has no table, **unresolved is what it stays**: nothing is substituted that the preset did
  not ask for, and the report says what has not loaded yet.

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

## 8. Third-party packs

Every corpus number this contract was measured against comes from the reference's own factory bank,
which is internally consistent: its asset paths are spelled the way its filesystem spells them, its
table references point at files that ship, and its schema version is one xerum knows. A **third-party
pack** is precisely the input that breaks that consistency, and importing one **MUST** survive it.

The verification is `tests/model/third_party_pack_test.cpp` and `plugin/tests/third_party_test.cpp`,
tagged `[third-party]`. What a third-party pack can do that the factory bank cannot, and what xerum does:

- **A case-mismatched asset path** — a reference whose spelling differs only in case from the file on
  disk (e.g. `/xerum/SINE.wav` for the shipped `.../xerum/Sine.wav`). Resolution **MUST** find the file,
  because a pack that works in the reference must work here, and the report **MUST** say the hit was
  *inexact*, at `info`, naming the reference and the path it landed on. On the case-insensitive
  filesystem macOS gives by default this requires comparing directory entries rather than asking the
  filesystem (§5): otherwise an exact hit and a lucky one are indistinguishable, and the report is
  silent about a pack that only works by luck.
- **A table reference that resolves to nothing present** — a reference no content root can supply.
  Reported at `high` naming the reference, then substituted with xerum's documented default
  (`xerum/Sine.wav`) and reported again at `info`. Two entries, because the miss and the thing that
  played in its place are different facts (§2, §5).
- **An absent table** — a slot that names no file at all. The same substitution happens, but **MUST NOT**
  be reported: `high` means the pack lost something it meant to use, and a slot that names nothing has
  lost nothing. The observable difference from the case above is the report, not the audio.
- **A schema version above 6.0** — not refused (§6). The payload **MUST** be parsed as far as the schema
  allows and the version named at `medium`, so a pack from a future reference imports visibly rather
  than being turned away or misread as current.
- **A parameter the schema does not describe** — named at `medium` and **retained** in the document, so a
  write reproduces the field the reference put there even though we did not interpret it (ticket `36`).

The fixtures are xerum's own, committed, and contain no reference content (ticket `10`): the tests build
their presets in memory or with `writeContainer`, and the only file they read is xerum's authored
content. They run unchanged on a machine without the reference installed.

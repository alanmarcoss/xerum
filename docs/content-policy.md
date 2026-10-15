# Content policy

What content xerum ships, where it reads content from, and how the rule is enforced. Derived from
`.scratch/xerum/issues/16-asset-and-content-policy.md`.

## The rule

**xerum ships only content we author or license. In v1, only what we author.**

Nothing from the reference instrument's content tree ships — no wavetables, multisamples, samples,
impulses, curves, skins or presets — in the distribution, in this repository, or in a bundled
installer.

The set of things that must never end up in the distribution is therefore short and unambiguous:
**any file the reference shipped.**

## The starter library

| Content | What it is |
|---|---|
| **Basic shapes** | sine, saw, square, triangle — mathematics, not anyone's content. These make xerum audible with no reference installed. |
| **Authored tables** | generated from formulas **we write**. |
| **Fallback table** | our equivalent of the reference's default shapes, used when a preset's table cannot be resolved. |

The reference's *formula system* is an interface and may be reimplemented for interoperability. The
factory **formula list** it ships is content, and is not reproduced.

Every shipped table lives at `content/Tables/xerum/<name>.wav` — in xerum's **own** root, resolved by
the import contract's search order, never written into the reference's tree. The fallback is
`xerum/Fallback Shapes.wav`, and a reference the library cannot resolve is still **reported** as missing
rather than substituted quietly (ticket `24`): substituting the fallback is the caller's decision, and
it is visible when made.

**The formula evaluator is an offline authoring tool, not a plugin feature.** It runs when content is
built, not when audio plays — which keeps a subsystem out of the plugin, and makes a bad formula a
build failure rather than a session failure.

## The factory preset bank

`54`. The bank the product ships with is **content**, so it follows the rules above: every preset is
written here, and every wavetable one plays is a file this repository generated.

| What | Where |
|---|---|
| The presets | `<content root>/Factory/<Category>/<Name>.xerumpreset` — 104 of them, across Bass, Lead, Pad, Keys, Pluck, FX, Texture and Percussion |
| The bank's manifest | `<content root>/Factory/manifest.json` — every preset's name, category, path, description and tables, and the file format, so the browser (ticket `55`) reads a file instead of reverse-engineering the bank |
| The authoring tool | `tools/preset-author.cpp` (`build/tools/xerum-preset-author`) |
| The bank itself, as data | `src/presets/factory_bank.cpp` |

### Why `Factory/`, and not `Presets/`

A reference install's content root **already contains `Presets/`** — its own factory bank, which is
licensed material and must never be browsed as ours. A bank directory called `Presets` would therefore
be ambiguous the moment a machine had both installed. `Factory/` exists in no content root but ours,
which turns "find the factory bank" into a search rather than a guess.

### Where the loader finds it

The same place every other asset is found: the content roots, in the order the import contract's §5
states — a user-configured root, the reference's tree if present, xerum's application-support
directory, and the source tree in a development checkout. A preset file is then loaded through the
product's **own** loader — either `XerumAudioProcessor::setStateInformation`, exactly as a host's state
restore is, or, since ticket `55`, `XerumAudioProcessor::loadPresetFromFile`, the door the browser uses
and the one the import button uses. Both reach the same reader: `model::decodePresetBlock`, which is why
the bank needs no reader of its own. Ticket `55` is the reason a *file* is read that way too — a preset
chosen in the browser had to be loaded through the same call as one imported, so `loadPresetFile` now
tells the two formats apart by their magic rather than by which call site opened them.

**If the bank is absent** — a build with no content directory, or a user who removed it — the search
finds nothing, and that is the honest answer: no factory presets. Nothing else depends on its presence;
a preset's *tables* are what the audio path needs, and those resolve separately.

### The format is ours

A `.xerumpreset` is **xerum's own block** — the magic, a format version and CBOR of the document
(`src/model/preset_block.h`) — not the reference's container. It is deliberately not shaped like a
`.SerumPreset`, so nobody can mistake one for the other, and the extension is ours so a browser can
tell them apart by name.

### How the rule is enforced for the bank

1. A preset may only play tables **this repository ships**: the authoring tool refuses to write a
   preset that names a table `dsp::starterLibrary()` does not generate, that spells its table as a
   rooted reference (`/Analog/...` — the spelling a reference preset uses), or that reads a frame
   outside the table it names.
2. The bank's own manifest is a second **provenance manifest**, and the release check reads it: every
   preset must declare its origin and licence and exist on disk, and **every table a preset names must
   be declared in `content/manifest.json`** — the cross-check that makes "self-contained" a build
   failure rather than an intention.
3. The bank's tests load every preset through the product's own loader and render it
   (`plugin/tests/bank_test.cpp`), and hold the catalogue to its own rules
   (`tests/model/factory_bank_test.cpp`).

## Content roots

xerum **reads** the reference's layout, so an existing install and third-party packs resolve as-is. It
**ships** into its own root. It **never writes** into the reference's content tree: that directory is
another application's install, and the reference maintains a database over it.

Resolution order — see [the import contract](preset-import-contract.md):

1. the user-configured content root;
2. the reference's content root, if present;
3. xerum's own content directory;
4. in a development checkout only, the source tree's own `content/`.

With no reference installed, a preset still loads and reports its unresolved references.

### The roots are offered, and asking is not the caller's job

Those four paths are **candidates**: xerum does not check them where it stands, because a directory the OS
will not let it look at is not a slow question, it is an open-ended one. `~/Desktop` is gated behind a
consent prompt; a network volume, a sleeping external drive and a stalled file provider all answer nothing
for as long as they like. Ticket `56` measured the cost of asking on the wrong thread: the plugin's own
constructor, inside a host, sat in `open$NOCANCEL` for over two hours while `tccd` logged an unanswered
`kTCCServiceSystemPolicyDesktopFolder` prompt for it — and a plugin that cannot finish constructing is a
host that cannot start.

So two rules hold, and `src/content/off_thread_work.h` is the mechanism for the second:

- **nothing on the resolution path throws.** A path that cannot be examined is an `error_code`, recorded
  with the path and the reason; a root that refuses is skipped and the roots behind it are still asked.
  The throwing overload used to reach the plugin's constructor and `abort()` the host — a crash report the
  dev produced — and a missing path never triggered it (`ENOENT` is `not_found`, not an error), so the
  case that did was a root the OS denied;
- **no content-root filesystem call runs on the thread that asked for it.** The walk runs on a worker and
  the caller waits a bounded time (250 ms — microseconds in the ordinary case). A resolution that lands
  later is published by the message thread's timer, with its own report entries.

  Nothing in that read can name the processor or the editor: it captures only `shared_ptr`s — the content
  library, the plan, the result block — and the roots by value. The worker's own queue state is shared with
  it the same way, so a destructor neither cancels nor joins a read that is inside a system call: it joins
  only an idle worker. The **residual** is that such a read holds its memory until the call returns — a
  bounded leak, not a dangling access, and a read the OS will not let go cannot be asked to stop.

`content/table_library_test.cpp` and `plugin/tests/content_gate_test.cpp` are where both rules are
tested: a mode-000 root, a FIFO standing in for a root that will not answer, and an owner destroyed
underneath an outstanding read.

## Deferred assets

Multisample and sample content is deferred, so v1 does not load it. v1 **reports** references to it
rather than ignoring them, and the resolution order above already accounts for it — so a later version
honours those references without a policy change. A deferred feature must not require rewriting the
rules it will eventually follow.

## Generated files are content too

A generator publishes as easily as a person — and more quietly, because nobody reads its output line by
line before it is committed.

This is not hypothetical here. `tools/generate-schema.py` first emitted every "value set" it found in
the decoded schema, and several of those were not vocabularies at all: **embedded SFZ text, tables of
asset paths, multi-kilobyte payloads lifted from real presets.** Its output would have published
reference content into this public repository without anyone ever deciding to.

So a generator deriving from reference-derived data **must carry its own guard**, and the guard belongs
in the generator rather than in a review step that will eventually be skipped. Ours admits a value set
only when it is short, has few members, and contains no newline: an enumeration is vocabulary, and
payload is not.

## Enforcement

Policy prose is not a check. The release path carries one:

1. **A provenance manifest** lists every shipped content file with its origin and licence. The build
   **fails** when a shipped file is absent from it — you cannot add content without saying where it
   came from. There are two, one per authoring tool: `content/manifest.json` for the wavetables and
   `content/Factory/manifest.json` for the factory presets, and the check also fails when a preset
   plays a table the table manifest does not declare.
2. **Where the reference is installed**, the check verifies no shipped file is byte-identical to any
   reference content file. This is the check that would actually catch a mistake; it runs locally
   because it needs the reference.
3. Content is added **through the authoring tool**, so what ships is what the tool generated rather
   than whatever was in a working directory.

### Where the enforcement lives (ticket `34`)

| Piece | Path |
|---|---|
| The formulas, and the catalogue they generate | `src/dsp/wavetable_write.cpp` (`starterLibrary()`) |
| The authoring tool | `tools/wavetable-author.cpp` (`build/tools/xerum-wavetable-author`) |
| The shipped tables | `content/Tables/xerum/` |
| The provenance manifest | `content/manifest.json` — per file: path, origin, licence, formula, frame count, interpolation, sha256 |
| The release check | `tools/check-content-provenance.py`, wired as the CTest test `content-provenance` |

The factory bank (ticket `54`) adds its own rows to that table, and its own two commands:

| Piece | Path |
|---|---|
| The bank, as data | `src/presets/factory_bank.cpp` |
| The authoring tool | `tools/preset-author.cpp` (`build/tools/xerum-preset-author`) |
| The shipped presets | `content/Factory/<Category>/<Name>.xerumpreset` |
| The bank's manifest | `content/Factory/manifest.json` — per preset: name, category, path, description, tables, sha256 |
| The bank's tests | `tests/model/factory_bank_test.cpp` (the catalogue and the bytes), `plugin/tests/bank_test.cpp` (loading, rendering, and that no two presets collapse) |

    build/tools/xerum-preset-author --out content --write --manifest content/Factory/manifest.json
    build/tools/xerum-preset-author --verify   # the shipped bytes are what the bank encodes
    build/tools/xerum-wavetable-author --out content --write --manifest content/manifest.json
    build/tools/xerum-wavetable-author --verify    # the shipped bytes are what the formulas generate
    ctest --test-dir build -R content-provenance   # the release gate

The gate fails on a shipped file with no manifest entry, on an entry with no origin or licence, on a
manifest hash that no longer matches the file, and — where the reference is installed — on any shipped
file that is byte-identical to one of the reference's. With no reference installed that last check
**skips loudly** instead of passing silently.

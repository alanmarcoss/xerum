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

**The formula evaluator is an offline authoring tool, not a plugin feature.** It runs when content is
built, not when audio plays — which keeps a subsystem out of the plugin, and makes a bad formula a
build failure rather than a session failure.

## Content roots

xerum **reads** the reference's layout, so an existing install and third-party packs resolve as-is. It
**ships** into its own root. It **never writes** into the reference's content tree: that directory is
another application's install, and the reference maintains a database over it.

Resolution order — see [the import contract](preset-import-contract.md):

1. the user-configured content root;
2. the reference's content root, if present;
3. xerum's own content directory.

With no reference installed, a preset still loads and reports its unresolved references.

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
   came from.
2. **Where the reference is installed**, the check verifies no shipped file is byte-identical to any
   reference content file. This is the check that would actually catch a mistake; it runs locally
   because it needs the reference.
3. Content is added **through the authoring tool**, so what ships is what the tool generated rather
   than whatever was in a working directory.

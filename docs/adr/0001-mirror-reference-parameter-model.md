# Mirror the reference preset model as xerum's parameter schema

**Status**: accepted (2026-10-13)

xerum's parameter model takes the shape of the reference instrument's preset model rather than being
designed independently: the same module tree, the same module instances, and a parameter addressed by
its module path. Importing a preset is therefore a translation of names and units, not a
reconstruction of structure, and the interface can be generated from the schema instead of hand-wired.
The public surface a host sees is still xerum's own — its own parameter identity and its own labels —
so the decision is about internal shape, not about imitating the reference's user-facing language.

## Considered options

- **Mirror the model** (chosen). Import is mechanical, the schema can be diffed when the reference
  changes, and the UI can be derived from the same source of truth as the importer.
- **Design an independent model plus a mapping layer.** Cleaner to own and free of the reference's
  quirks, but every future reference schema version then reopens the mapping, and the mapping layer
  becomes the place where fidelity bugs hide — it is testable only through behavior, not structure.
- **Mirror the shape but rename everything.** The worst of both: all the structural coupling, plus a
  second naming scheme to keep in sync, with no benefit that a naming convention does not already give.

## Consequences

- Importer bugs become **structural** (a module or parameter is missing) rather than **silent**
  (a value lands in the wrong place), which makes the whole class of failure testable against the
  preset corpus.
- The model inherits the reference's quirks: parameters left behind by unselected engines, and a
  whole-module "defaults" marker. Import normalises these on read rather than propagating them inward.
- Schema version differences become data — a set of modules and parameters per version — instead of
  conditionals scattered through the importer.
- Nothing here licenses copying the reference's content, assets, labels or code. Only the *shape* of
  its parameter model is shared, and only so that its presets can be read.

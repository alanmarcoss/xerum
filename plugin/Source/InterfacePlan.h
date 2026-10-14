// xerum::plugin — the layout plan.
//
// Ticket `31` fixed the rule the interface must obey: **the layout is designed, the binding is
// model-driven**. So the geometry below is derived from the parameter table and nothing else:
//
//   * which control a parameter gets is a function of its model type (`controlKindFor`), total over
//     `ParamType` — a new model type is a compile error here rather than a parameter that quietly gets
//     no control;
//   * which **panel** a parameter lands in is its `ParamSpec::group`, in the table's own order, so a module
//     added to the table appears in the interface without anyone editing a layout;
//   * a new parameter is placed rather than forgotten: every panel lays its controls out itself.
//
// ## Ticket `44` — the structure was one device per page
//
// Ticket `43` settled the structure as variant 3, "Devices": one device per page, behind a strip that
// always shows the others. A device was a **panel** — one `ParamSpec::group` — and the plan's panels *were*
// the pages.
//
// ## Ticket `52` — the composition is the design's, and it is data
//
// The design (`docs/design/xerum-direction-warm.html`) does not arrange panels by the schema; it arranges
// them **for a player**: three oscillators across the top, noise and sub beneath them, the two filters with
// their response display in the middle, the matrix under them, and the effects and macros side by side at
// the bottom. The dev's reaction to ticket `51` — the elements are the design's, the placements are not —
// is exactly this, and a panel-per-group grid cannot express it.
//
// So the composition is **declarative data**: an ordered list of **sections** (`designedComposition`),
// each naming the groups it holds and how they arrange. The binding is untouched — a control is still a
// function of the model type, a panel is still one group, and every control still lands in exactly one
// panel — and what the data decides is **which section a group lands in**. A group no section names falls
// through to the **default section**, which is a section like any other, so nothing vanishes:
// `planInterface` asserts the two halves of that invariant by construction (every group is claimed once).
//
// Within a panel the arrangement is derived again, from the control kinds the model types produce: the
// design's grammar is a row of knobs, a row of pickers and a row of pills (`osc-knobs` / `osc-controls`),
// and NOISE and SUB are the design's one compact band where the knobs and the option buttons share a line
// (`--gap-inline`: "between inline elements (knobs + buttons in noise/sub)").

#pragma once

#include "ParameterBridge.h"

#include <string>
#include <vector>

namespace xerum::plugin::ui
{

/// The widget a parameter of this model type is bound to.
enum class ControlKind
{
    Toggle,   ///< a two-state button — `ParamType::Bool`
    Choice,   ///< a picker over the Spec's vocabulary — `ParamType::Choice`
    Slider    ///< a slider (integer steps for `Int`, continuous for `Float`)
};

/// Total over `ParamType`. Deliberately no `default:` — adding a parameter type to the bridge breaks this
/// build instead of leaving the new type without a control.
constexpr ControlKind controlKindFor (const ParamType type)
{
    switch (type)
    {
        case ParamType::Bool:   return ControlKind::Toggle;
        case ParamType::Choice: return ControlKind::Choice;
        case ParamType::Int:    return ControlKind::Slider;
        case ParamType::Float:  return ControlKind::Slider;
    }

    return ControlKind::Slider;   // unreachable; `-Wswitch` already failed the build above
}

/// How a choice's vocabulary is drawn. The design draws a choice as a **row of option buttons**
/// (`btn-opt`) — NOISE's four colours, SUB's six waveforms, the filter's four types — and as a picker
/// where the list is too long to be a row. xerum's own vocabularies are the same shape, with one
/// exception it cannot hide: the filter's type is an **83-value** vocabulary (ticket `26`), so the filter
/// panels carry the themed picker rather than the design's four buttons, and the button row appears where
/// the design uses it, on the two fixed slots.
enum class ChoiceStyle
{
    Buttons,   ///< a row of pills — `NOISE`'s colours and `SUB`'s waveforms
    Menu       ///< the themed picker — warp modes, filter types, the matrix's source and class lists
};

/// The widest option row the design draws is SUB's six waveforms plus its sentinel, which fits a
/// half-width panel at the design's own pill size. Derived from the vocabulary's length rather than
/// written down per group, so a new small vocabulary gets the design's row for free.
inline constexpr int kMaxChoiceButtons = 8;

constexpr ChoiceStyle choiceStyleFor (const int choiceCount)
{
    return choiceCount <= kMaxChoiceButtons ? ChoiceStyle::Buttons : ChoiceStyle::Menu;
}

/// The diameter a slider (a `ui::Knob`) is drawn at. Ticket `51`: total, like `controlKindFor`, and a
/// function of the **spec** rather than of a hand-placed layout — a new panel gets the default diameter
/// without anyone editing a table, and the two sizes the design system names for something other than a
/// full-size knob are recognised by the model's own group.
///
/// A `ParamSpec` carries no "size" field and is not going to grow one: the size is decoration, the kind is
/// structure. The macro groups get the macro knob because a macro is a macro, and the two fixed
/// non-container slots (NOISE, SUB) get the small knob because the design puts them in one compact band.
enum class KnobSize
{
    Default,   ///< 44px — every knob on a panel
    Small,     ///< 28px — NOISE and SUB, the design's compact band
    Macro      ///< 26px — the eight macros
};

KnobSize knobSizeFor (const ParamSpec& spec);

//==============================================================================
// The design's numbers, in one place.

/// A row's height, from the design system and from what the row's control actually is: a toggle or a
/// choice is one compact line, and a knob is its own diameter plus its readout and its label. This is a
/// function of the spec for the same reason `knobSizeFor` is — the plan's derivation decides everything.
int rowHeightFor (const ParamSpec& spec);

constexpr int kRowHeight = 26;              ///< a toggle or a choice row
constexpr int kKnobRowHeight = 68;          ///< a 44px knob, its readout and its label
constexpr int kSmallKnobRowHeight = 52;     ///< a 28px knob, its readout and its label
constexpr int kMacroRowHeight = 50;         ///< a 26px macro knob, its readout and its label
constexpr int kRowGap = 4;                  ///< between the rows inside a panel (`--gap-panel` is 3; a knob row needs 4)
constexpr int kPanelHeaderHeight = 26;      ///< a panel's own title bar
constexpr int kPanelPadding = 10;
constexpr int kRowLabelWidth = 156;         ///< the label column of a choice row

/// The composition's own numbers, from the design system's grid: `--gap-row` 6 between the panels of one
/// row, `--gap-section` 3 between the rows of the composition, `--pad-content` 10 at the sides.
constexpr int kPanelGap = 6;                ///< between the panels of one section
constexpr int kKnobCellGap = 4;             ///< between the cells of one row (`--gap-knob`)
constexpr int kInlineGap = 6;               ///< between inline elements (`--gap-inline`)
constexpr int kSectionGap = 3;              ///< between the sections of the composition
constexpr int kSectionTitleHeight = 15;     ///< a section's own heading line
constexpr int kContentPadding = 10;         ///< the composition's side padding

/// The oscillator head: the design's wavetable slot — a monospaced name field and two 16px arrows.
constexpr int kWavetableHeadHeight = 22;

/// A macro item in the bottom row: a 26px knob and its label, in the design's `macro-item`.
constexpr int kMacroItemWidth = 44;

/// The width and minimum height a panel gives the drawn response it carries inside itself — the design's
/// `filter-viz`, beside the filter's knobs (`flex: 1` against the knobs' own width).
constexpr int kInlineDisplayMinWidth = 190;
constexpr int kInlineDisplayMinHeight = 190;

//==============================================================================
// The composition.

/// How a section arranges the panels it holds.
enum class SectionKind
{
    Panels,   ///< side by side, equal widths — the design's `osc-row`, `noise-sub-row`, `filter-section`
    Strip,    ///< fixed-width items in a strip — the design's macro strip
    Matrix    ///< the modulation matrix, which is not a panel: one grid of controls
};

/// One entry of the composition, as data. `holds` is how the section claims groups: an entry matches a
/// group whose name **is** the entry or **starts with** the entry followed by a space, so `Macro` claims
/// `Macro 1`…`Macro 8` and `Osc` claims `Osc A`…`Osc C` without listing them. An empty `holds` on a
/// **panel** section claims whatever no other section claimed — the **default section**, which is why a
/// group no section names still lands somewhere. A `Matrix` section names no group either, and is not a
/// candidate for the fall-through: it holds a grid, not a group.
///
/// `band` is the composition row the section sits in: sections sharing a band are laid out side by side in
/// one row, in this list's order. That is how the design's `bottom-row` — the effects cards and the macro
/// strip — is expressed, and it is the reason the two are one row rather than two.
struct SectionSpec
{
    std::string id;                  ///< the design's own name: `osc-row`, `filter-section`, `mod-panel`, …
    std::string title;               ///< the section's heading, in xerum's words
    SectionKind kind { SectionKind::Panels };
    std::vector<std::string> holds;  ///< the groups it claims; empty means "whatever is left"
    std::string band;                ///< sections sharing a band sit side by side
};

/// The design's composition, in its order: `osc-row` → `noise-sub-row` → `filter-section` → `mod-panel` →
/// `bottom-row` (the effects cards beside the macro strip) → the default section. The FX section claims a
/// group this table does not have — xerum's bridge exposes no effect parameters — so it resolves to no
/// panels and is not drawn; it stays in the data so the row completes itself the day the table has one.
std::vector<SectionSpec> designedComposition();

/// The leaf name of a container slot's frame position. Named here because the oscillator head's arrows
/// step **this** parameter: the design's arrows choose a different wavetable, xerum has no wavetable
/// chooser (`plugin/README.md` records the decision), and the frame position is the one per-slot table
/// control the model has. `planInterface` finds it by this leaf, so a table without it gets a head with no
/// arrows rather than arrows that write nothing.
inline constexpr const char* kTablePositionLeaf = "kParamTablePos";

/// One panel: a group's title, the model module its parameters live in, its parameters in table order,
/// and the two model slots whose *drawn* surfaces belong to this panel rather than to a page of their own.
struct PanelPlan
{
    std::string title;                       ///< the group — xerum's own name for the panel
    std::string module;                      ///< e.g. `Oscillator0`, `VoiceFilter1`, `Global0`
    std::vector<const ParamSpec*> specs;      ///< in table order

    /// The oscillator slot whose wavetable head the panel carries, or -1. Derived from the module
    /// (`Oscillator{N}` with `N` a container slot), so the head is a fact about the table rather than a
    /// list of three group names.
    int wavetableSlot { -1 };

    /// The row of the panel's own parameters the head's arrows step, or -1 — see `kTablePositionLeaf`.
    int positionRow { -1 };

    /// The voice filter whose drawn response the panel carries **inside itself**, beside its knobs, or
    /// -1. Also derived from the module (`VoiceFilter{N}`).
    int filterSlot { -1 };

    int rows() const { return static_cast<int> (specs.size()); }
};

/// One resolved section: the composition entry, the panels it holds (indices into `InterfacePlan::panels`,
/// in the table's order), and nothing else. Resolved — a section whose `holds` matched no group is not in
/// the plan at all, because a section with no panels would be an empty box rather than an arrangement.
struct SectionPlan
{
    std::string id;
    std::string title;
    SectionKind kind { SectionKind::Panels };
    std::string band;
    std::vector<int> panels;
};

/// The whole layout: one panel per group (the table's order) and the composition's sections, in the
/// design's order, with every panel in exactly one of them.
struct InterfacePlan
{
    std::vector<PanelPlan> panels;       ///< one per group, in the table's order
    std::vector<SectionPlan> sections;   ///< the composition, resolved

    int rows() const
    {
        int total = 0;

        for (const auto& panel : panels)
            total += panel.rows();

        return total;
    }

    /// The parameter ids the plan covers, in panel order — the interface's own coverage, stated.
    std::vector<std::string> ids() const;

    /// The group a panel is, or an empty string for an index that is not a panel.
    const std::string& titleAt (int panel) const;

    /// The section a group landed in, or -1 — what makes the fall-through visible rather than assumed.
    int sectionForGroup (const std::string& group) const;
};

/// Build the plan: one panel per group in first-appearance order, then the composition resolved over
/// them. Every group lands in exactly one section: the first section whose `holds` claims it, else the
/// default section (the composition's empty-`holds` entry, or one appended after it when the design names
/// no default).
InterfacePlan planInterface (const std::vector<ParamSpec>& specs,
                             const std::vector<SectionSpec>& composition);

/// Where the preset-import dialog opens.
///
/// **The interface does not browse the content root for presets** — see `plugin/README.md`. It offers one
/// explicit file action, importing a `.SerumPreset`, and this only decides where that dialog starts: the
/// first root that exists on this machine, so a machine with the reference installed opens somewhere
/// useful and a machine without one falls back to `fallback`.
juce::File importStartDirectory (const std::vector<juce::String>& contentRoots, const juce::File& fallback);

} // namespace xerum::plugin::ui

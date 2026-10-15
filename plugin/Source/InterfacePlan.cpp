// xerum::plugin — the layout plan, in code. See InterfacePlan.h.

#include "InterfacePlan.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace xerum::plugin::ui
{

namespace
{
/// The model module a parameter lives in: `Oscillator0/plainParams/kParamEnable` -> `Oscillator0`.
/// The group's own parameters all live in one module — `VoiceFilter0`, `Env0`, `Oscillator3` — and the
/// coverage test asserts it, because the strip's report dots and the panel's own tag are keyed on it.
std::string moduleOf (const ParamSpec& spec)
{
    const auto slash = spec.modelPath.find ('/');

    return spec.modelPath.substr (0, slash == std::string::npos ? spec.modelPath.size() : slash);
}

/// The last path element: `Oscillator0/WTOsc0/plainParams/kParamTablePos` -> `kParamTablePos`.
std::string leafOf (const std::string& modelPath)
{
    const auto slash = modelPath.rfind ('/');

    return modelPath.substr (slash == std::string::npos ? 0 : slash + 1);
}

/// `Oscillator{N}` where N is a **container** slot, or -1. `Oscillator3` and `Oscillator4` are the fixed
/// NOISE and SUB slots — oscillators in the model's own numbering, and not wavetable slots — which is
/// exactly why this is a range check rather than a prefix test.
int oscillatorSlotFor (const std::string& module)
{
    const std::string prefix = "Oscillator";

    if (module.rfind (prefix, 0) != 0)
        return -1;

    const auto digits = module.substr (prefix.size());

    if (digits.empty()
        || ! std::all_of (digits.begin(), digits.end(),
                          [] (const unsigned char c) { return std::isdigit (c) != 0; }))
        return -1;

    const auto slot = std::stoi (digits);

    return slot >= 0 && slot < dsp::kOscillatorSlots ? slot : -1;
}

/// `VoiceFilter{N}`, or -1.
int filterSlotFor (const std::string& module)
{
    const std::string prefix = "VoiceFilter";

    if (module.rfind (prefix, 0) != 0)
        return -1;

    const auto digits = module.substr (prefix.size());

    if (digits.empty()
        || ! std::all_of (digits.begin(), digits.end(),
                          [] (const unsigned char c) { return std::isdigit (c) != 0; }))
        return -1;

    const auto slot = std::stoi (digits);

    return slot >= 0 && slot < dsp::kFilterSlots ? slot : -1;
}

/// Does a section claim this group? An entry claims the group whose name **is** the entry, or that
/// **starts with** the entry followed by a space — so `Osc` claims `Osc A`…`Osc C` and `Macro` claims
/// `Macro 1`…`Macro 8` without either being listed. An entry that matched by prefix alone would let
/// `Sub` claim `Sub Osc` and `Filter` claim nothing else, which is what we want; what it must not do is
/// let `Osc` claim `Sub Osc`, and a prefix test alone would not.
bool claims (const SectionSpec& section, const std::string& group)
{
    for (const auto& held : section.holds)
        if (group == held || (group.size() > held.size() && group.compare (0, held.size(), held) == 0
                              && group[held.size()] == ' '))
            return true;

    return false;
}

/// The section a group no section names falls through to. Its panels keep their own names, and it is a
/// section like any other — which is what makes "a group the composition does not name still lands"
/// a fact about the plan rather than a promise.
///
/// Ticket `53` gives it the design's **bottom row** as its band. The design pairs its effects cards with
/// the macro strip there and xerum's table has no effects group, so the half of that row the absent FX
/// section would have held is exactly where the fall-through groups belong: the row completes itself
/// without anyone writing down which groups go in it.
///
/// Ticket `57` names a section for the amplitude envelope, so `Global` is the one group left falling
/// through — and the row is unchanged, because a section that gained a home must not move the ones that
/// did not.
SectionSpec defaultSection()
{
    return { "default-section", "Other", SectionKind::Panels, {}, "bottom" };
}
} // namespace

KnobSize knobSizeFor (const ParamSpec& spec)
{
    // Only a slider becomes a knob at all — a boolean is a toggle and a choice is a picker, whatever
    // diameter a table might want to write down — so this is the *second* decision about a control and it
    // is only ever asked about the first one's answer.
    if (controlKindFor (spec.type) != ControlKind::Slider)
        return KnobSize::Default;

    // The eight macros: one knob each, and the design draws them at 26px.
    if (spec.group.rfind ("Macro ", 0) == 0)
        return KnobSize::Macro;

    // The two fixed non-container slots. The design puts NOISE and SUB in one compact band with the small
    // knob; they are the model's own groups, so this is still the table speaking rather than a hand list.
    if (spec.group == "Noise" || spec.group == "Sub Osc")
        return KnobSize::Small;

    return KnobSize::Default;
}

int rowHeightFor (const ParamSpec& spec)
{
    switch (knobSizeFor (spec))
    {
        case KnobSize::Small: return kSmallKnobRowHeight;
        case KnobSize::Macro: return kMacroRowHeight;
        case KnobSize::Default: break;
    }

    return controlKindFor (spec.type) == ControlKind::Slider ? kKnobRowHeight : kRowHeight;
}

std::vector<SectionSpec> designedComposition()
{
    // `docs/design/xerum-direction-warm.html`, read as data. The bands are the design's own rows, in its
    // own order — three oscillators, noise and sub, the amplitude envelope's own band, the two filters, the
    // bottom row of macros beside the fall-through, and the matrix at the end — and the entry that names no
    // group is the fall-through itself, which is where the groups the composition does not name land.
    // Ticket `57` moved two things and added one; ticket `59` removed the design's `1fr` from the data,
    // because there is no window slack for a section to take any more (see `SectionSpec`).
    return {
        { "osc-row", "Oscillators", SectionKind::Panels, { "Osc" }, "osc" },
        { "noise-sub-row", "Noise + Sub", SectionKind::Panels, { "Noise", "Sub Osc" }, "noiseSub" },
        { "env-section", "Amp Envelope", SectionKind::Panels, { "Amp Envelope" }, "env" },
        { "filter-section", "Filter", SectionKind::Panels, { "Filter" }, "filter" },
        { "fx-section", "Effects Chain", SectionKind::Strip, { "FX" }, "bottom" },
        { "macro-section", "Macros", SectionKind::Strip, { "Macro" }, "bottom" },
        defaultSection(),

        // The matrix is **last**, after the macros and after the fall-through section it shares no band
        // with: ticket `57`'s editorial call is that the page's end is where a 64-row grid belongs, and it
        // is also where the design's own grid has nothing left to give. Its band is its own, because the
        // grid is twelve columns wide and does not share a row with a panel or a strip.
        { "mod-panel", "Mod Matrix", SectionKind::Matrix, {}, "matrix" },
    };
}

std::optional<EnvelopeStage> envelopeStageForModelPath (const std::string& modelPath)
{
    const auto leaf = leafOf (modelPath);

    if (leaf == "kParamAttack")
        return EnvelopeStage::Attack;

    if (leaf == "kParamDecay")
        return EnvelopeStage::Decay;

    if (leaf == "kParamSustain")
        return EnvelopeStage::Sustain;

    if (leaf == "kParamRelease")
        return EnvelopeStage::Release;

    return std::nullopt;
}

std::vector<std::string> InterfacePlan::ids() const
{
    std::vector<std::string> out;
    out.reserve (static_cast<std::size_t> (rows()));

    for (const auto& panel : panels)
        for (const auto* spec : panel.specs)
            out.push_back (spec->id);

    return out;
}

const std::string& InterfacePlan::titleAt (const int panel) const
{
    static const std::string empty;

    return panel >= 0 && panel < static_cast<int> (panels.size())
               ? panels[static_cast<std::size_t> (panel)].title
               : empty;
}

int InterfacePlan::sectionForGroup (const std::string& group) const
{
    for (int index = 0; index < static_cast<int> (sections.size()); ++index)
        for (const auto panel : sections[static_cast<std::size_t> (index)].panels)
            if (titleAt (panel) == group)
                return index;

    return -1;
}

InterfacePlan planInterface (const std::vector<ParamSpec>& specs,
                             const std::vector<SectionSpec>& composition)
{
    InterfacePlan plan;

    // Panels are the table's groups, in first-appearance order. A `map` keyed by the group name plus a
    // first-appearance index reproduces the order the table reads in, which is also the order the
    // bridge's parameter tree uses — one interface, one order.
    std::map<std::string, int> panelIndexForGroup;

    for (const auto& spec : specs)
    {
        auto found = panelIndexForGroup.find (spec.group);

        if (found == panelIndexForGroup.end())
        {
            const auto module = moduleOf (spec);

            PanelPlan panel;
            panel.title = spec.group;
            panel.module = module;
            panel.wavetableSlot = oscillatorSlotFor (module);
            panel.filterSlot = filterSlotFor (module);

            found = panelIndexForGroup.emplace (spec.group, static_cast<int> (plan.panels.size())).first;
            plan.panels.push_back (std::move (panel));
        }

        plan.panels[static_cast<std::size_t> (found->second)].specs.push_back (&spec);
    }

    // The head's stepper: the slot's own frame position, found by the model's own leaf name so the plan
    // never writes down a parameter id.
    for (auto& panel : plan.panels)
        for (int row = 0; row < panel.rows(); ++row)
            if (leafOf (panel.specs[static_cast<std::size_t> (row)]->modelPath) == kTablePositionLeaf)
            {
                panel.positionRow = row;
                break;
            }

    // The composition, resolved. One default section at most: the first **panel** entry with no `holds`
    // takes the leftovers, and entries after it that also name nothing take nothing (and fall out below).
    //
    // The matrix's section names no group either — it holds a grid rather than a group — so it is
    // deliberately not a candidate: "names no group" and "takes whatever is left" are different claims and
    // the matrix makes only the first.
    auto effective = composition;

    const auto isDefault = [] (const SectionSpec& section)
    { return section.holds.empty() && section.kind != SectionKind::Matrix; };

    if (std::none_of (effective.begin(), effective.end(), isDefault))
        effective.push_back (defaultSection());

    int defaultIndex = -1;

    for (int index = 0; index < static_cast<int> (effective.size()); ++index)
        if (isDefault (effective[static_cast<std::size_t> (index)]))
        {
            defaultIndex = index;
            break;
        }

    std::vector<int> sectionForPanel (plan.panels.size(), -1);

    for (int panel = 0; panel < static_cast<int> (plan.panels.size()); ++panel)
    {
        const auto& group = plan.panels[static_cast<std::size_t> (panel)].title;

        for (int index = 0; index < static_cast<int> (effective.size()); ++index)
        {
            if (index == defaultIndex)
                continue;

            if (claims (effective[static_cast<std::size_t> (index)], group))
            {
                sectionForPanel[static_cast<std::size_t> (panel)] = index;
                break;
            }
        }

        // **The fall-through**: a group no section names lands in the default one rather than vanishing.
        if (sectionForPanel[static_cast<std::size_t> (panel)] < 0)
            sectionForPanel[static_cast<std::size_t> (panel)] = defaultIndex;
    }

    for (int index = 0; index < static_cast<int> (effective.size()); ++index)
    {
        const auto& spec = effective[static_cast<std::size_t> (index)];

        SectionPlan section;
        section.id = spec.id;
        section.title = spec.title;
        section.kind = spec.kind;
        section.band = spec.band;

        for (int panel = 0; panel < static_cast<int> (plan.panels.size()); ++panel)
            if (sectionForPanel[static_cast<std::size_t> (panel)] == index)
                section.panels.push_back (panel);

        // The matrix is a section even though no panel is in it; a section that claimed nothing else is
        // not a section of the page at all — an empty box is not an arrangement.
        if (section.panels.empty() && spec.kind != SectionKind::Matrix)
            continue;

        plan.sections.push_back (std::move (section));
    }

    return plan;
}

juce::File importStartDirectory (const std::vector<juce::String>& contentRoots, const juce::File& fallback)
{
    for (const auto& root : contentRoots)
    {
        const juce::File directory (root);

        if (directory.isDirectory())
            return directory;
    }

    return fallback;
}

} // namespace xerum::plugin::ui

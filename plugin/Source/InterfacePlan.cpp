// xerum::plugin — the layout plan, in code. See InterfacePlan.h.

#include "InterfacePlan.h"

#include <algorithm>
#include <map>

namespace xerum::plugin::ui
{

namespace
{
/// The model module a parameter lives in: `Oscillator0/plainParams/kParamEnable` -> `Oscillator0`.
/// The group's own parameters all live in one module — `VoiceFilter0`, `Env0`, `Oscillator3` — and the
/// coverage test asserts it, because the strip's report dots and the page's subtitle are keyed on it.
std::string moduleOf (const ParamSpec& spec)
{
    const auto slash = spec.modelPath.find ('/');

    return spec.modelPath.substr (0, slash == std::string::npos ? spec.modelPath.size() : slash);
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

std::vector<std::string> InterfacePlan::ids() const
{
    std::vector<std::string> out;
    out.reserve (static_cast<std::size_t> (rows()));

    for (const auto& panel : panels)
        for (const auto* spec : panel.specs)
            out.push_back (spec->id);

    return out;
}

InterfacePlan planInterface (const std::vector<ParamSpec>& specs, const int columnCount)
{
    InterfacePlan plan;

    // Pages are the table's groups, in first-appearance order. A `map` keyed by the group name plus a
    // first-appearance index reproduces the order the table reads in, which is also the order the
    // bridge's parameter tree uses — one interface, one order.
    std::map<std::string, int> panelIndexForGroup;

    for (const auto& spec : specs)
    {
        auto found = panelIndexForGroup.find (spec.group);

        if (found == panelIndexForGroup.end())
        {
            found = panelIndexForGroup.emplace (spec.group, static_cast<int> (plan.panels.size())).first;
            plan.panels.push_back ({ spec.group, moduleOf (spec), {}, {} });
        }

        plan.panels[static_cast<std::size_t> (found->second)].specs.push_back (&spec);
    }

    const auto maximumColumns = std::max (1, columnCount);

    // The page's rows across its columns. Greedy, ties to the left: deterministic, and it keeps the
    // first rows in reading order along the top.
    for (auto& panel : plan.panels)
    {
        const auto columns = columnsForRows (panel.rows(), maximumColumns);
        panel.columns.assign (static_cast<std::size_t> (columns), {});

        std::vector<int> columnRows (static_cast<std::size_t> (columns), 0);

        for (int row = 0; row < panel.rows(); ++row)
        {
            const auto shortest = static_cast<std::size_t> (
                std::min_element (columnRows.begin(), columnRows.end()) - columnRows.begin());

            panel.columns[shortest].push_back (row);
            ++columnRows[shortest];
        }
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

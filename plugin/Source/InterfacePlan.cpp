// xerum::plugin — the layout plan, in code. See InterfacePlan.h.

#include "InterfacePlan.h"

#include <algorithm>
#include <map>

namespace xerum::plugin::ui
{

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

    // Panels are the table's groups, in first-appearance order. A `map` keyed by the group name plus a
    // first-appearance index reproduces the order the table reads in, which is also the order the
    // bridge's parameter tree uses — one interface, one order.
    std::map<std::string, int> panelIndexForGroup;

    for (const auto& spec : specs)
    {
        auto found = panelIndexForGroup.find (spec.group);

        if (found == panelIndexForGroup.end())
        {
            found = panelIndexForGroup.emplace (spec.group, static_cast<int> (plan.panels.size())).first;
            plan.panels.push_back ({ spec.group, {} });
        }

        plan.panels[static_cast<std::size_t> (found->second)].specs.push_back (&spec);
    }

    const auto columns = std::max (1, columnCount);
    plan.columns.assign (static_cast<std::size_t> (columns), {});

    std::vector<int> columnRows (static_cast<std::size_t> (columns), 0);

    for (int panel = 0; panel < static_cast<int> (plan.panels.size()); ++panel)
    {
        // The currently shortest column, ties to the left: deterministic, and it keeps the first panels
        // in reading order along the top.
        const auto shortest = std::min_element (columnRows.begin(), columnRows.end()) - columnRows.begin();

        plan.columns[static_cast<std::size_t> (shortest)].push_back (panel);
        columnRows[static_cast<std::size_t> (shortest)] += plan.panels[static_cast<std::size_t> (panel)].rows();
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

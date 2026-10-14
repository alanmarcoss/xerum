// xerum::plugin — the layout plan.
//
// Ticket `31`. Ticket `13` fixed the rule the interface must obey: **the layout is designed, the binding
// is model-driven**. So the geometry below is derived from the parameter table and nothing else:
//
//   * which control a parameter gets is a function of its model type (`controlKindFor`), total over
//     `ParamType` — a new model type is a compile error here rather than a parameter that quietly gets
//     no control;
//   * which panel a parameter lands in is its `ParamSpec::group`, in the table's own order, so a module
//     added to the table appears in the interface without anyone editing a layout;
//   * which column a panel lands in is a balance over the panels, so a new panel is placed rather than
//     forgotten.
//
// The point is that this file, like the bridge, has no list of parameters in it. What it does have is
// the *design*: row heights, the number of columns, and the rule that keeps the columns even. That is
// the half ticket `13` says is allowed to be xerum's own taste.

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
    Choice,   ///< a combo box over the Spec's vocabulary — `ParamType::Choice`
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

//==============================================================================
// The design's numbers, in one place.

constexpr int kRowHeight = 24;        ///< one parameter's row inside a panel
constexpr int kRowGap = 2;
constexpr int kPanelHeaderHeight = 22;
constexpr int kPanelPadding = 6;
constexpr int kPanelGap = 10;
constexpr int kRowLabelWidth = 156;   ///< the label column of a row (a toggle takes the whole row)

/// One panel: the group's title, and the parameters in it, in table order.
struct PanelPlan
{
    std::string title;
    std::vector<const ParamSpec*> specs;

    int height() const { return kPanelHeaderHeight + kPanelPadding * 2 + rows() * (kRowHeight + kRowGap) - kRowGap; }
    int rows() const { return static_cast<int> (specs.size()); }
};

/// The whole layout: the panels, and how they are distributed across the columns.
struct InterfacePlan
{
    std::vector<PanelPlan> panels;           ///< in table order
    std::vector<std::vector<int>> columns;   ///< panel indices, top to bottom, one vector per column

    int rows() const
    {
        int total = 0;

        for (const auto& panel : panels)
            total += panel.rows();

        return total;
    }

    /// The parameter ids the plan covers, in reading order — the interface's own coverage, stated.
    std::vector<std::string> ids() const;
};

/// Build the plan from the parameter table: one panel per group, in first-appearance order, distributed
/// across `columnCount` columns by a greedy balance (each panel to the currently shortest column).
///
/// Every spec lands in exactly one panel, and every panel in exactly one column: a parameter cannot be
/// dropped from the layout, and the test asserts it.
InterfacePlan planInterface (const std::vector<ParamSpec>& specs, int columnCount);

/// Where the preset-import dialog opens.
///
/// **The interface does not browse the content root for presets** — see `plugin/README.md`. It offers one
/// explicit file action, importing a `.SerumPreset`, and this only decides where that dialog starts: the
/// first root that exists on this machine, so a machine with the reference installed opens somewhere
/// useful and a machine without one falls back to `fallback`.
juce::File importStartDirectory (const std::vector<juce::String>& contentRoots, const juce::File& fallback);

} // namespace xerum::plugin::ui

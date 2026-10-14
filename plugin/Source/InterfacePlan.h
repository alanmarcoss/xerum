// xerum::plugin — the layout plan.
//
// Ticket `31` fixed the rule the interface must obey: **the layout is designed, the binding is
// model-driven**. So the geometry below is derived from the parameter table and nothing else:
//
//   * which control a parameter gets is a function of its model type (`controlKindFor`), total over
//     `ParamType` — a new model type is a compile error here rather than a parameter that quietly gets
//     no control;
//   * which panel a parameter lands in is its `ParamSpec::group`, in the table's own order, so a module
//     added to the table appears in the interface without anyone editing a layout;
//   * which column a control lands in is a balance over its panel's rows, so nothing is dropped from the
//     layout and a new parameter is placed rather than forgotten.
//
// ## What ticket `44` changed, and what it did not
//
// Ticket `43` settled the structure as variant 3, "Devices": **one device per page, behind a strip that
// always shows the others**. A device is a **panel** — one `ParamSpec::group`, which is the model's own
// grouping and the same granularity the reference uses for its tabs — so the plan's panels *are* the
// pages. The rule above is untouched: a control is still a function of the model type, a panel is still
// a group in the table's order.
//
// The balance moved. It used to distribute *panels* across the editor's columns, because the editor was
// one long sheet; now a page holds one panel, so the same greedy balance distributes that panel's *rows*
// across its columns. The point of the balance is unchanged and is the reason it is not hand-placed:
// every row lands in exactly one column, so a parameter cannot be dropped from a page by a layout edit.
//
// `columnsForRows` is the design's own number: a 13-control page is two columns rather than one very
// long one, and a four-control page is one column rather than three absurdly narrow ones.

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

constexpr int kRowHeight = 26;         ///< one parameter's row inside a page
constexpr int kRowGap = 4;
constexpr int kPanelHeaderHeight = 26; ///< the page's own title bar
constexpr int kPanelPadding = 10;
constexpr int kColumnGap = 12;         ///< between the columns of one page
constexpr int kRowLabelWidth = 156;    ///< the label column of a row (a toggle takes the whole row)
constexpr int kPageHeaderHeight = 30;  ///< the editor's line above the page: device name and module
constexpr int kRowsPerColumn = 8;      ///< how tall a column is allowed to get before one more is started

/// How many columns a page's rows are spread over: one for a short page, otherwise enough to keep a
/// column at or under `kRowsPerColumn`, capped at the editor's `columnCount`.
inline int columnsForRows (const int rows, const int columnCount)
{
    if (rows <= 4 || columnCount <= 1)
        return 1;

    const int wanted = (rows + kRowsPerColumn - 1) / kRowsPerColumn;

    return wanted < columnCount ? wanted : columnCount;
}

/// One page: the group's title, the model module its parameters live in, its parameters in table order,
/// and the column each row landed in.
struct PanelPlan
{
    std::string title;                          ///< the group — xerum's own name for the device page
    std::string module;                         ///< e.g. `Oscillator0`, `VoiceFilter1`, `Global0`
    std::vector<const ParamSpec*> specs;        ///< in table order
    std::vector<std::vector<int>> columns;      ///< row indices per column, balanced

    int rows() const { return static_cast<int> (specs.size()); }

    int columnsCount() const { return static_cast<int> (columns.size()); }

    /// The tallest column, which is the page's natural height.
    int height() const
    {
        std::size_t tallest = 0;

        for (const auto& column : columns)
            tallest = column.size() > tallest ? column.size() : tallest;

        if (tallest == 0)
            tallest = static_cast<std::size_t> (rows());

        return kPanelHeaderHeight + kPanelPadding * 2
                   + static_cast<int> (tallest) * (kRowHeight + kRowGap) - kRowGap;
    }
};

/// The whole layout: one page per group, in table order.
struct InterfacePlan
{
    std::vector<PanelPlan> panels;   ///< one device page each, in the table's group order

    int rows() const
    {
        int total = 0;

        for (const auto& panel : panels)
            total += panel.rows();

        return total;
    }

    /// The widest page, in columns: what the editor's natural width is derived from.
    int widestPanel() const
    {
        int widest = 1;

        for (const auto& panel : panels)
            widest = panel.columnsCount() > widest ? panel.columnsCount() : widest;

        return widest;
    }

    /// The parameter ids the plan covers, in reading order — the interface's own coverage, stated.
    std::vector<std::string> ids() const;
};

/// Build the plan from the parameter table: one page per group, in first-appearance order, its rows
/// distributed across at most `columnCount` columns by a greedy balance (each row to the currently
/// shortest column, ties to the left).
///
/// Every spec lands in exactly one row and every row in exactly one column: a parameter cannot be
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

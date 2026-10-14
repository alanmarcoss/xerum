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

/// The diameter a slider (a `ui::Knob`) is drawn at. Ticket `51`: total, like `controlKindFor`, and a
/// function of the **spec** rather than of a hand-placed layout — a new panel gets the default diameter
/// without anyone editing a table, and the two sizes the design system names for something other than a
/// full-size knob are recognised by the model's own group.
///
/// A `ParamSpec` carries no "size" field and is not going to grow one: the size is decoration, the kind is
/// structure. The macro groups get the macro knob because a macro is a macro, and the two fixed non-
/// container slots (NOISE, SUB) get the small knob because the design puts them in one compact band.
enum class KnobSize
{
    Default,   ///< 44px — every knob on a device page
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
constexpr int kRowGap = 4;
constexpr int kPanelHeaderHeight = 26; ///< the page's own title bar
constexpr int kPanelPadding = 10;
constexpr int kColumnGap = 10;         ///< between the columns of one page
constexpr int kColumnWidth = 168;      ///< the width a control's cell wants inside a column
constexpr int kRowLabelWidth = 156;    ///< the label column of a choice row
constexpr int kPageHeaderHeight = 30;  ///< the editor's line above the page: device name and module
constexpr int kRowsPerColumn = 5;      ///< how tall a column is allowed to get before one more is started

/// How many columns a page's rows are spread over: one for a short page, otherwise enough to keep a
/// column at or under `kRowsPerColumn`, capped at the editor's `columnCount`.
///
/// `kRowsPerColumn` is 5 rather than 8 because ticket `51` made a slider row a 44px knob: at eight rows a
/// column of knobs is 576px tall, which does not fit the design's 600px window, and the balance is the
/// only thing that decides how tall a page is.
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

    /// The tallest column, which is the page's natural height. Ticket `51`: a column's height is the sum of
    /// its **rows'** heights, not its row count times one number, because a knob's row is three times a
    /// toggle's — and a page that reserved the old uniform height would leave a tall gap under a short page.
    int height() const
    {
        int tallest = 0;

        for (const auto& column : columns)
        {
            int columnHeight = 0;

            for (const auto row : column)
                if (row >= 0 && row < rows())
                    columnHeight += rowHeightFor (*specs[static_cast<std::size_t> (row)]) + kRowGap;

            if (columnHeight > 0)
                columnHeight -= kRowGap;   // no gap under the last row

            tallest = columnHeight > tallest ? columnHeight : tallest;
        }

        // No columns set means one column — the same rule `ParameterPanel::resized` applies — so the panel
        // is as tall as all of its rows.
        if (tallest == 0)
            for (const auto* spec : specs)
                tallest += rowHeightFor (*spec) + kRowGap;

        return kPanelHeaderHeight + kPanelPadding * 2 + tallest;
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

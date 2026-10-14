// xerum::plugin — the modulation matrix, as the interface shows it.
//
// Ticket `44`, and one of the two surfaces ticket `43` called hard: **64 rows × 12 columns is where a
// matrix either works or does not**, and the prototype showed what it looks like when the columns are
// real rather than a grid of anonymous cells.
//
//   * a row's `source` is the reference's **two-element pair** (`source[]`), both from its own 60-name
//     table — so the cells are *named* via `dsp::modSourceName`, the table ticket `27` recovered from the
//     binary, which is the correction ticket `43` recorded against the prototype's "the interface cannot
//     name a source";
//   * destinations are `(class, instance, parameter)`, and the row's address verdict is the engine's own
//     (`dsp::modDestinationUnresolvedReason`), shown in the row rather than only collected in the report;
//   * a source v1 does not evaluate is named where it is used (`dsp::modSourceIsImplemented`), because
//     "this row does nothing" is the one thing a user needs from a matrix row that does nothing.
//
// ## It is a view, not a control surface — and that is measured, not preferred
//
// The rows below are the **loaded preset's** rows. They are not sliders, and the reason is the bridge's
// own rule rather than a missing afternoon's work: `ParameterBridge` exposes 75 parameters and nothing in
// the matrix, and the plugin never hands the engine's `ModulationMatrix` a slot at all —
// `VoiceEngine::setModulationSlots` is called by the engine's tests and by nothing in `plugin/`. A control
// here would write a value nothing reads, which is the placeholder smell this project forbids. So the
// matrix *shows* the preset's modulation and reports the two gaps rather than inventing a panel that
// appears to edit them. Both facts are recorded in `plugin/README.md`.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"
#include "modulation.h"

#include <string>
#include <vector>

namespace xerum::plugin::ui
{

/// The reference's own capacity: `ModSlot0` … `ModSlot63`, every one present in every preset.
inline constexpr int kModulationSlots = 64;

/// One row as the interface shows it: the engine's own row, and whether the preset stores this slot at
/// all. The reference writes all 64 slots and most of them are empty, so an empty slot is **a row the
/// user can see**, not a gap in a list.
struct MatrixRow
{
    dsp::ModSlot slot;
    bool stored { false };
};

/// Read the matrix out of a loaded document. Always `kModulationSlots` rows.
///
/// Every value comes from the document's own keys, and a field the preset omits keeps the engine's own
/// default (`dsp::ModSlot`'s initialiser). That is what the engine will evaluate, and it is the model's
/// meaning for an absent key — "at default" (ticket `01`), never "nothing".
std::vector<MatrixRow> modulationRowsFromDocument (const model::PresetDocument& document);

/// The matrix as a table: one row per slot, one column per thing a row stores or addresses.
class ModulationMatrixView final : public juce::Component
{
public:
    ModulationMatrixView();

    void setRows (std::vector<MatrixRow> rows);

    int rowCount() const { return static_cast<int> (rows_.size()); }
    const MatrixRow& row (int index) const { return rows_.at (static_cast<std::size_t> (index)); }

    static int columnCount();
    static juce::String columnTitle (int column);
    static int columnWidth (int column);

    /// What a cell shows. Column 0 is the slot index; every other column is the row's own data, or an
    /// em-dash for a slot the preset does not store.
    juce::String cellText (int row, int column) const;

    /// The engine's own verdict on the row: why its destination does not resolve, or which source v1 does
    /// not evaluate. Empty when the row is fine, or when the preset stores no such row.
    juce::String noteForRow (int row) const;

    /// How many of the 64 slots the preset actually stores.
    int storedRowCount() const;

    static int preferredHeight();
    static int preferredWidth();

    static constexpr int kHeaderHeight = 26;
    static constexpr int kRowHeight = 22;

    void paint (juce::Graphics& graphics) override;

private:
    std::vector<MatrixRow> rows_;
};

} // namespace xerum::plugin::ui

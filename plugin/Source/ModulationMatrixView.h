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
// ## Ticket 45: the rows are editable, and heard
//
// Ticket `44` left this a view, and its reason was exactly right: `ParameterBridge` bound no modulation
// parameter and the plugin never handed `VoiceEngine::setModulationSlots` a slot, so a control here would
// have written a value nothing read — the placeholder smell this project forbids. Ticket `45` built the
// missing path: the bridge **declares** the eight editable fields of every row
// (`ParameterBridge::modulationSpecs`), `applyDocument` writes them from a preset and `getStateInformation`
// writes them back, and the processor hands the engine a rebuilt row table whenever one moves. So the
// cells below are real `ParameterControl`s, bound to the bridge's own parameter objects, and an edit is a
// **preset edit** — it lands in the document's `ModSlot{N}` rows — rather than a parallel state.
//
// The painted display is kept for the columns that are not edited (the slot number, the curve knobs and
// the engine's verdict on the row), and `cellText` is still how a test reads a row without a widget.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ParameterPanels.h"
#include "document.h"
#include "modulation.h"

#include <memory>
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

    //==============================================================================
    // Ticket 45: the editable columns.

    /// Whether a column's cells bind to the bridge's parameters. Eight of the twelve do — the source
    /// pair, the amount, the polarity, the bypass and the destination triple, which is exactly the field
    /// list the acceptance names. The slot number, the two curve knobs and the note are painted from the
    /// document: the curves reach the engine *from the document* (ticket 45 reads them there), and the
    /// note is the engine's verdict rather than a value.
    static bool columnIsEditable (int column);

    /// The field an editable column edits. Precondition: `columnIsEditable (column)`.
    static ModRowField fieldForColumn (int column);

    /// Bind the editable cells to the bridge's own parameter objects — one `ParameterControl` per cell of
    /// every row, created once. The editor calls this; a view built by hand stays painted-only, which is
    /// what keeps the ticket `44` display tests pure.
    void bindToBridge (const ParameterBridge& bridge);

    bool isBound() const { return ! cells_.empty(); }

    /// How many cells are bound — 0 for a painted-only view. The editor's coverage readout adds this to
    /// its page-control count, so the 512 matrix cells are inside the number the header states.
    int boundCellCount() const;

    /// The control bound to a cell, or nullptr for a painted column or an unbound view.
    ParameterControl* controlForCell (int row, int column) const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    std::vector<MatrixRow> rows_;

    /// One control per editable cell, indexed `row * columnCount() + column`; the non-editable columns'
    /// slots are null. A `unique_ptr` because each control owns the attachment that binds it.
    std::vector<std::unique_ptr<ParameterControl>> cells_;
};

} // namespace xerum::plugin::ui

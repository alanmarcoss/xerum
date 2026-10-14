// xerum::plugin — the modulation matrix, in code. See ModulationMatrixView.h.

#include "ModulationMatrixView.h"

#include "InterfaceLook.h"

#include <cmath>

namespace xerum::plugin::ui
{

namespace
{
/// The columns, in reading order, with their widths. Twelve of them: the slot, the source pair, what the
/// row does to the destination (amount, polarity, bypass, the two curve knobs), the address itself
/// (class, instance, parameter) and the engine's verdict.
struct ColumnSpec
{
    const char* title;
    int width;
};

constexpr ColumnSpec kColumns[] = {
    { "#",           40 },   // 0  the slot
    { "Source",     142 },   // 1  `source[0]`, named
    { "Aux source", 142 },   // 2  `source[1]`, named
    { "Amount",      78 },   // 3  `kParamAmount`, percent, signed
    { "Polarity",    70 },   // 4  `kParamBipolar`
    { "Bypass",      60 },   // 5  `kParamBypass`
    { "Curve in",    78 },   // 6  `kParamCurveIn`
    { "Curve out",   78 },   // 7  `kParamCurveOut`
    { "Destination", 152 },  // 8  `destModuleTypeString`
    { "Instance",     66 },  // 9  `destModuleID` — an FX rack and slot encoded as rack * 100 + slot
    { "Parameter",   168 },  // 10 `destModuleParamName`
    { "Note",        250 },  // 11 the engine's verdict on the row
};

constexpr int kColumnCount = static_cast<int> (sizeof (kColumns) / sizeof (kColumns[0]));
constexpr auto kDash = "\xe2\x80\x94";   // an em-dash: "the preset stores nothing here"

double numberAt (const model::PresetDocument& document, const std::string& path, const double fallback)
{
    const auto found = document.parameters.find (path);

    if (found == document.parameters.end())
        return fallback;

    double value = fallback;
    return found->second.numberAsDouble (value) ? value : fallback;
}

std::string textAt (const model::PresetDocument& document, const std::string& path)
{
    const auto found = document.parameters.find (path);

    return found != document.parameters.end() && found->second.isText() ? found->second.text : std::string();
}

/// A source value as the interface shows it: its name from the reference's own 60-name table, or an
/// em-dash when there is no source. **Never the bare index** — ticket `27` recovered the table so the
/// interface could name what a preset routed, and ticket `43` recorded the prototype's conclusion that it
/// could not as the one thing to correct.
juce::String sourceName (const int referenceValue)
{
    const auto name = dsp::modSourceName (referenceValue);

    return name.empty() ? juce::String (kDash) : juce::String (std::string (name));
}

juce::String percent (const double value)
{
    return juce::String (value, 1) + "%";
}
} // namespace

std::vector<MatrixRow> modulationRowsFromDocument (const model::PresetDocument& document)
{
    std::vector<MatrixRow> rows (static_cast<std::size_t> (kModulationSlots));

    const dsp::ModSlot defaults;

    for (int index = 0; index < kModulationSlots; ++index)
    {
        const auto prefix = "ModSlot" + std::to_string (index) + "/";

        // A slot the preset does not mention is a row the reference simply is not carrying — the document
        // is sparse and says so. It is not an empty row with invented values.
        const auto first = document.parameters.lower_bound (prefix);

        if (first == document.parameters.end() || ! first->first.starts_with (prefix))
            continue;

        auto& row = rows[static_cast<std::size_t> (index)];
        row.stored = true;
        row.slot.used = true;
        row.slot.source = static_cast<int> (numberAt (document, prefix + "source[0]", 0.0));
        row.slot.auxSource = static_cast<int> (numberAt (document, prefix + "source[1]", 0.0));
        row.slot.amount = numberAt (document, prefix + "plainParams/kParamAmount", defaults.amount);
        row.slot.bipolar = numberAt (document, prefix + "plainParams/kParamBipolar", 0.0) != 0.0;
        row.slot.bypassed = numberAt (document, prefix + "plainParams/kParamBypass", 0.0) != 0.0;
        row.slot.curveIn = numberAt (document, prefix + "plainParams/kParamCurveIn", defaults.curveIn);
        row.slot.curveOut = numberAt (document, prefix + "plainParams/kParamCurveOut", defaults.curveOut);
        row.slot.destinationClass = textAt (document, prefix + "destModuleTypeString");
        row.slot.destinationInstance = static_cast<int> (numberAt (document, prefix + "destModuleID", 0.0));
        row.slot.destinationParameter = static_cast<int> (numberAt (document, prefix + "destModuleParamID", 0.0));
        row.slot.destinationName = textAt (document, prefix + "destModuleParamName");
    }

    return rows;
}

//==============================================================================
ModulationMatrixView::ModulationMatrixView()
{
    setInterceptsMouseClicks (false, false);
    setRows ({});
}

void ModulationMatrixView::setRows (std::vector<MatrixRow> rows)
{
    rows_ = std::move (rows);
    repaint();
}

bool ModulationMatrixView::columnIsEditable (const int column)
{
    // The eight the acceptance names, and only those: the curves and the note stay painted.
    switch (column)
    {
        case 1: case 2: case 3: case 4: case 5: case 8: case 9: case 10:
            return true;
        default:
            return false;
    }
}

ModRowField ModulationMatrixView::fieldForColumn (const int column)
{
    switch (column)
    {
        case 1:  return ModRowField::Source;
        case 2:  return ModRowField::AuxSource;
        case 3:  return ModRowField::Amount;
        case 4:  return ModRowField::Bipolar;
        case 5:  return ModRowField::Bypass;
        case 8:  return ModRowField::DestinationClass;
        case 9:  return ModRowField::DestinationInstance;
        default: return ModRowField::DestinationParameter;   // column 10
    }
}

void ModulationMatrixView::bindToBridge (const ParameterBridge& bridge)
{
    if (isBound())
        return;

    cells_.clear();
    cells_.resize (static_cast<std::size_t> (kModulationSlots * kColumnCount));

    for (int row = 0; row < kModulationSlots; ++row)
        for (int column = 0; column < kColumnCount; ++column)
        {
            if (! columnIsEditable (column))
                continue;

            const auto* spec = bridge.modulationSpec (row, fieldForColumn (column));

            if (spec == nullptr || spec->paramIndex < 0
                || spec->paramIndex >= static_cast<int> (bridge.parameters().size()))
                continue;

            auto control = std::make_unique<ParameterControl> (
                *spec, *bridge.parameters()[static_cast<std::size_t> (spec->paramIndex)],
                bridge.choices (spec->choiceSet), /* showLabel */ false);

            // The two cells the design draws specially: an **amount** colours its sign, because for an
            // amount the sign is the polarity, and a **bypass** is a dot that is green while the row is
            // live. The parameter is `kParamBypass` — 1 when the row is *not* live — so the cell inverts
            // the dot rather than the parameter.
            if (column == 3)
                control->setSignColouring (true);
            else if (column == 5)
                control->setCompactToggle (true, /* inverted */ true);

            cells_[static_cast<std::size_t> (row * kColumnCount + column)] = std::move (control);
            addAndMakeVisible (*cells_[static_cast<std::size_t> (row * kColumnCount + column)]);
        }

    resized();
    repaint();
}

int ModulationMatrixView::boundCellCount() const
{
    int count = 0;

    for (const auto& cell : cells_)
        count += cell != nullptr ? 1 : 0;

    return count;
}

ParameterControl* ModulationMatrixView::controlForCell (const int row, const int column) const
{
    if (row < 0 || row >= kModulationSlots || column < 0 || column >= kColumnCount)
        return nullptr;

    const auto index = static_cast<std::size_t> (row * kColumnCount + column);

    return index < cells_.size() ? cells_[index].get() : nullptr;
}

void ModulationMatrixView::resized()
{
    for (int row = 0; row < kModulationSlots; ++row)
    {
        int x = 0;

        for (int column = 0; column < kColumnCount; ++column)
        {
            if (auto* control = controlForCell (row, column))
                control->setBounds (x + 3, kHeaderHeight + row * kRowHeight + 1,
                                    kColumns[column].width - 6, kRowHeight - 2);

            x += kColumns[column].width;
        }
    }
}

int ModulationMatrixView::columnCount() { return kColumnCount; }

juce::String ModulationMatrixView::columnTitle (const int column)
{
    return column >= 0 && column < kColumnCount ? juce::String (kColumns[column].title) : juce::String();
}

int ModulationMatrixView::columnWidth (const int column)
{
    return column >= 0 && column < kColumnCount ? kColumns[column].width : 0;
}

int ModulationMatrixView::preferredHeight() { return kHeaderHeight + kModulationSlots * kRowHeight + 2; }

int ModulationMatrixView::preferredWidth()
{
    int total = 0;

    for (const auto& column : kColumns)
        total += column.width;

    return total + 2;
}

int ModulationMatrixView::storedRowCount() const
{
    int count = 0;

    for (const auto& row : rows_)
        count += row.stored ? 1 : 0;

    return count;
}

juce::String ModulationMatrixView::cellText (const int rowIndex, const int column) const
{
    if (rowIndex < 0 || rowIndex >= rowCount() || column < 0 || column >= kColumnCount)
        return {};

    if (column == 0)
        return juce::String (rowIndex);

    // The note is a verdict on a row, so it says nothing for a row there is nothing to judge — and it says
    // the same thing whether the row is stored or not, because `noteForRow` is where that decision lives.
    if (column == 11)
        return noteForRow (rowIndex);

    const auto& row = rows_[static_cast<std::size_t> (rowIndex)];

    if (! row.stored)
        return juce::String (kDash);

    const auto& slot = row.slot;

    switch (column)
    {
        case 1:  return sourceName (slot.source);
        case 2:  return slot.auxSource == 0 ? juce::String (kDash) : sourceName (slot.auxSource);
        case 3:  return percent (slot.amount);
        case 4:  return slot.bipolar ? "bipolar" : "unipolar";
        case 5:  return slot.bypassed ? "bypassed" : juce::String();
        case 6:  return percent (slot.curveIn);
        case 7:  return percent (slot.curveOut);
        case 8:  return slot.destinationClass.empty() ? juce::String (kDash)
                                                      : juce::String (slot.destinationClass);
        case 9:  return slot.destinationClass.empty() ? juce::String (kDash)
                                                      : juce::String (slot.destinationInstance);
        case 10: return slot.destinationName.empty() ? juce::String (kDash)
                                                     : juce::String (slot.destinationName);
        default: break;
    }

    return {};
}

juce::String ModulationMatrixView::noteForRow (const int rowIndex) const
{
    if (rowIndex < 0 || rowIndex >= rowCount())
        return {};

    const auto& row = rows_[static_cast<std::size_t> (rowIndex)];

    if (! row.stored)
        return {};

    const auto& slot = row.slot;

    // An empty stored row is the reference's normal case — all 64 slots present, most of them holding
    // nothing — so it says nothing rather than crying wolf 40-odd times per preset.
    if (slot.source == 0 && slot.destinationClass.empty())
        return {};

    if (const auto unresolved = dsp::modDestinationUnresolvedReason (slot); ! unresolved.empty())
        return juce::String (unresolved);

    if (slot.source != 0 && ! dsp::modSourceIsImplemented (slot.source))
        return "source " + sourceName (slot.source) + " is named and not evaluated by this build";

    if (slot.auxSource != 0 && ! dsp::modSourceIsImplemented (slot.auxSource))
        return "aux source " + sourceName (slot.auxSource) + " is named and not evaluated by this build";

    return {};
}

void ModulationMatrixView::paint (juce::Graphics& graphics)
{
    const auto mono = look::monoFont (11.0f);

    graphics.setColour (look::bg);
    graphics.fillAll();

    // The header: the columns' names, on the bar the rows scroll under. The design's own table head —
    // 7px monospaced, uppercase, in the muted foreground.
    {
        auto header = getLocalBounds().removeFromTop (kHeaderHeight);
        graphics.setColour (look::surface);
        graphics.fillRect (header);

        int x = 0;

        for (int column = 0; column < kColumnCount; ++column)
        {
            const auto cell = juce::Rectangle<int> (x, header.getY(), kColumns[column].width, kHeaderHeight);
            graphics.setColour (look::fgMuted);
            graphics.setFont (look::monoFont (look::type::tableHead, true));
            graphics.drawText (juce::String (kColumns[column].title).toUpperCase(),
                               cell.reduced (6, 0), juce::Justification::centredLeft, true);
            x += kColumns[column].width;
        }

        graphics.setColour (look::border);
        graphics.fillRect (header.removeFromBottom (1));
    }

    for (int rowIndex = 0; rowIndex < rowCount(); ++rowIndex)
    {
        auto rowArea = juce::Rectangle<int> (0, kHeaderHeight + rowIndex * kRowHeight, getWidth(),
                                             kRowHeight);

        if (! rowArea.intersects (graphics.getClipBounds()))
            continue;

        const auto& row = rows_[static_cast<std::size_t> (rowIndex)];

        if (row.stored)
        {
            // A stored row is a row on the surface; an empty slot is the page behind it. Striping the
            // stored rows keeps a 64-row table readable without a grid of rules.
            graphics.setColour (rowIndex % 2 == 0 ? look::surface : look::surface2);
            graphics.fillRect (rowArea);
        }

        const auto note = noteForRow (rowIndex);
        int x = 0;

        for (int column = 0; column < kColumnCount; ++column)
        {
            const auto cell = juce::Rectangle<int> (x, rowArea.getY(), kColumns[column].width, kRowHeight)
                                  .reduced (6, 0);

            // An editable cell that is bound draws itself (a widget over this background), so the painted
            // text would double it. The row background stays: the grid still reads as a grid.
            if (isBound() && columnIsEditable (column))
            {
                x += kColumns[column].width;
                continue;
            }

            if (column == 11)
                graphics.setColour (note.isEmpty() ? look::fgDim : look::orange);
            else if (column == 1 && row.stored)
                graphics.setColour (look::accent);   // a source is the row's subject, and the accent says so
            else if (column == 3 && row.stored && std::abs (row.slot.amount) > 0.0)
                graphics.setColour (look::accent);   // an amount is the row's whole point when it has one
            else
                graphics.setColour (row.stored ? look::fg : look::fgMuted);

            graphics.setFont (mono);
            graphics.drawText (cellText (rowIndex, column), cell, juce::Justification::centredLeft, true);

            x += kColumns[column].width;
        }

        graphics.setColour (look::border);
        graphics.fillRect (rowArea.removeFromBottom (1));
    }
}

} // namespace xerum::plugin::ui

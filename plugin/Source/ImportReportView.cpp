// xerum::plugin — the import report view, in code. See ImportReportView.h.

#include "ImportReportView.h"

#include "InterfaceLook.h"

#include <algorithm>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kPad = 8;
constexpr int kSummaryHeight = 20;
constexpr int kPresetLineHeight = 15;
constexpr int kRowFirstLine = 16;    // the chip and path line
constexpr int kRowLineHeight = 13;   // each wrapped line of the item and the reason
constexpr int kRowGap = 7;
constexpr int kChipWidth = 52;

juce::Font monoFont (const float size)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain));
}
} // namespace

ImportReportView::ImportReportView()
{
    setInterceptsMouseClicks (false, false);
}

void ImportReportView::setReport (const model::ImportReport& report, juce::String presetName,
                                  juce::String lastError, const std::vector<juce::String>& contentRoots)
{
    rows_.clear();
    preset_ = std::move (presetName);
    lastError_ = std::move (lastError);

    rows_.reserve (report.entries.size());

    for (const auto& entry : report.entries)
    {
        Row row;
        row.severity = entry.severity;
        row.modulePath = juce::String (entry.modulePath);
        row.item = juce::String (entry.item);
        row.reason = juce::String (entry.reason);
        row.text = look::severityName (entry.severity).toUpperCase() + "  " + row.modulePath + "\n"
                   + row.item + "\n" + row.reason;
        rows_.push_back (std::move (row));
    }

    worst_ = report.worst();

    if (lastError_.isNotEmpty())
        summary_ = "Load failed — " + lastError_;
    else if (rows_.empty())
        // The contract's words: a clean import's report is empty, not absent, and the two must not look
        // the same. "Nothing to report" is how the interface tells them apart.
        summary_ = "Nothing to report";
    else
    {
        juce::StringArray counts;

        for (const auto severity : { model::Severity::High, model::Severity::Medium, model::Severity::Info })
            if (const auto count = severityCount (severity); count > 0)
                counts.add (juce::String (static_cast<int> (count)) + " " + look::severityName (severity));

        summary_ = juce::String (static_cast<int> (rows_.size()))
                   + (rows_.size() == 1 ? " item — " : " items — ") + counts.joinIntoString (", ");
    }

    juce::StringArray roots;

    for (const auto& root : contentRoots)
        roots.add (root);

    roots_ = roots.isEmpty() ? juce::String() : "content roots, in order: " + roots.joinIntoString (" | ");

    relayout();
}

juce::String ImportReportView::rowText (const int index) const
{
    if (index < 0 || index >= rowCount())
        return {};

    return rows_[static_cast<std::size_t> (index)].text;
}

std::size_t ImportReportView::severityCount (const model::Severity severity) const
{
    return static_cast<std::size_t> (std::count_if (rows_.begin(), rows_.end(),
                                                    [severity] (const Row& row)
                                                    { return row.severity == severity; }));
}

int ImportReportView::preferredHeight() const
{
    auto height = kPad * 2 + kSummaryHeight + kPresetLineHeight;

    for (const auto& row : rows_)
        height += row.height + kRowGap;

    height += rootsWrapped_.size() * kRowLineHeight;

    return height + 2;
}

void ImportReportView::relayout()
{
    // Re-wrap at the current width. Both `setReport` and `resized` land here, so what is painted is
    // always wrapped for the width it is painted at.
    const juce::FontOptions textFont { 11.5f };
    const auto textWidth = juce::jmax (80.0f, static_cast<float> (getWidth()) - kPad * 2.0f - 12.0f);

    for (auto& row : rows_)
    {
        row.itemLines = wrap (row.item, textFont, textWidth);
        row.reasonLines = wrap (row.reason, textFont, textWidth);
        row.height = kRowFirstLine + (row.itemLines.size() + row.reasonLines.size()) * kRowLineHeight;
    }

    rootsWrapped_ = roots_.isEmpty()
                        ? juce::StringArray()
                        : wrap (roots_, juce::FontOptions (11.0f),
                                juce::jmax (80.0f, static_cast<float> (getWidth()) - kPad * 2.0f));
}

void ImportReportView::resized()
{
    relayout();
}

juce::StringArray ImportReportView::wrap (const juce::String& text, const juce::Font& font, const float width)
{
    juce::StringArray lines;
    juce::String current;

    juce::StringArray words;
    words.addTokens (text, " ", "");

    for (const auto& word : words)
    {
        const auto candidate = current.isEmpty() ? word : current + " " + word;

        if (! current.isEmpty() && juce::GlyphArrangement::getStringWidth (font, candidate) > width)
        {
            lines.add (current);
            current = word;
        }
        else
        {
            current = candidate;
        }
    }

    if (current.isNotEmpty() || lines.isEmpty())
        lines.add (current);

    return lines;
}

void ImportReportView::paint (juce::Graphics& graphics)
{
    auto area = getLocalBounds().reduced (kPad);

    // The summary: the worst severity's colour, or the load failure's. Always present, so a clean report
    // and an absent report cannot be mistaken for each other.
    graphics.setColour (lastError_.isNotEmpty()
                            ? look::severityHigh
                            : (rows_.empty() ? look::dimText : look::severityColour (worst_)));
    graphics.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    graphics.drawText (summary_, area.removeFromTop (kSummaryHeight), juce::Justification::centredLeft, true);

    graphics.setColour (look::dimText);
    graphics.setFont (juce::FontOptions (11.0f));
    graphics.drawText (preset_.isEmpty() ? juce::String ("preset: init") : "preset: " + preset_,
                       area.removeFromTop (kPresetLineHeight), juce::Justification::centredLeft, true);

    for (const auto& row : rows_)
    {
        auto rowArea = area.removeFromTop (row.height);
        auto firstLine = rowArea.removeFromTop (kRowFirstLine);

        auto chip = firstLine.removeFromLeft (kChipWidth).reduced (0, 2);
        graphics.setColour (look::severityColour (row.severity));
        graphics.fillRoundedRectangle (chip.toFloat(), 3.0f);
        graphics.setColour (look::background);
        graphics.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        graphics.drawText (look::severityName (row.severity).toUpperCase(), chip,
                           juce::Justification::centred, false);

        // The module path, so a user can find the thing in the reference's own tree.
        graphics.setColour (look::text);
        graphics.setFont (monoFont (11.5f));
        graphics.drawText (row.modulePath, firstLine.reduced (6, 0), juce::Justification::centredLeft, true);

        // Then what the preset asked for, bright, and why it did not get it, quiet.
        for (const auto& line : row.itemLines)
        {
            graphics.setColour (look::text);
            graphics.setFont (juce::FontOptions (11.5f));
            graphics.drawText (line, rowArea.removeFromTop (kRowLineHeight).withTrimmedLeft (8),
                               juce::Justification::centredLeft, true);
        }

        for (const auto& line : row.reasonLines)
        {
            graphics.setColour (look::dimText);
            graphics.setFont (juce::FontOptions (11.5f));
            graphics.drawText (line, rowArea.removeFromTop (kRowLineHeight).withTrimmedLeft (8),
                               juce::Justification::centredLeft, true);
        }

        area.removeFromTop (kRowGap);
    }

    graphics.setColour (look::dimText);
    graphics.setFont (juce::FontOptions (11.0f));

    for (const auto& line : rootsWrapped_)
        graphics.drawText (line, area.removeFromTop (kRowLineHeight), juce::Justification::centredLeft, true);
}

} // namespace xerum::plugin::ui

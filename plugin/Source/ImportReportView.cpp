// xerum::plugin — the import report surface, in code. See ImportReportView.h.

#include "ImportReportView.h"

#include "InterfaceLook.h"

#include <algorithm>

namespace xerum::plugin::ui
{

namespace
{
constexpr int kPad = 8;
constexpr int kLegendHeight = 16;    // names the four fields once, at the top of the drawer
constexpr int kRowFirstLine = 16;    // the chip and path line
constexpr int kRowLineHeight = 13;   // each wrapped line of the item and the reason
constexpr int kRowGap = 7;
constexpr int kChipWidth = 52;

juce::Font monoFont (const float size)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain));
}
} // namespace

//==============================================================================
ReportBar::ReportBar()
{
    toggle_.setButtonText ("Details");
    toggle_.setColour (juce::TextButton::buttonColourId, look::track);
    toggle_.setColour (juce::TextButton::textColourOffId, look::dimText);
    toggle_.setColour (juce::TextButton::textColourOnId, look::text);
    toggle_.onClick = [this]
    {
        if (onToggleRequested)
            onToggleRequested();
    };

    addAndMakeVisible (toggle_);
}

void ReportBar::setReport (juce::String summary, juce::String presetName,
                           const model::ImportReport& report, juce::String lastError)
{
    summary_ = std::move (summary);
    preset_ = std::move (presetName);
    worst_ = report.worst();
    clean_ = report.clean();
    failed_ = lastError.isNotEmpty();
    repaint();
}

void ReportBar::setDrawerOpen (const bool open)
{
    open_ = open;
    toggle_.setButtonText (open ? "Hide details" : "Details");
    repaint();
}

void ReportBar::toggle()
{
    if (onToggleRequested)
        onToggleRequested();
}

void ReportBar::resized()
{
    auto area = getLocalBounds().reduced (kPad, 4);
    toggle_.setBounds (area.removeFromRight (juce::jmin (108, juce::jmax (72, area.getWidth() / 4))));
}

void ReportBar::paint (juce::Graphics& graphics)
{
    auto area = getLocalBounds();

    graphics.setColour (look::barFill);
    graphics.fillRoundedRectangle (area.toFloat(), 5.0f);
    graphics.setColour (look::panelBorder);
    graphics.drawRoundedRectangle (area.toFloat().reduced (0.5f), 5.0f, 1.0f);

    auto line = area.reduced (kPad, 0);

    // The state dot. Four states, four colours: a load failure, each severity, and *clean* — because a
    // clean import is a state and must not be drawn like "no data".
    auto dot = line.removeFromLeft (10).withSizeKeepingCentre (8, 8).toFloat();
    graphics.setColour (failed_ ? look::severityHigh
                                : (clean_ ? look::ok : look::severityColour (worst_)));
    graphics.fillEllipse (dot);

    line.removeFromLeft (6);

    const auto toggleWidth = toggle_.getWidth() + kPad;
    auto presetArea = line.removeFromRight (juce::jmin (juce::jmax (toggleWidth, line.getWidth() / 3),
                                                       toggleWidth + 220));

    graphics.setColour (look::text);
    graphics.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    graphics.drawText (summary_, line, juce::Justification::centredLeft, true);

    graphics.setColour (look::dimText);
    graphics.setFont (monoFont (10.5f));
    graphics.drawText (preset_.isEmpty() ? juce::String ("preset: init") : "preset: " + preset_,
                       presetArea.reduced (kPad, 0), juce::Justification::centredRight, true);
}

//==============================================================================
ImportReportView::ImportReportView()
{
    setInterceptsMouseClicks (false, false);
}

void ImportReportView::setReport (const model::ImportReport& report, juce::String lastError,
                                  const std::vector<juce::String>& contentRoots)
{
    rows_.clear();
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
    auto height = kPad * 2 + kLegendHeight;

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

    // The four things a row carries, named once. The rows themselves are the loader's own strings, so the
    // legend is the only hand-written text in the drawer and it says what the reader is looking at.
    graphics.setColour (look::dimText);
    graphics.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    graphics.drawText ("SEVERITY  ·  MODULE PATH  ·  ITEM  ·  REASON", area.removeFromTop (kLegendHeight),
                       juce::Justification::centredLeft, true);

    if (lastError_.isNotEmpty())
    {
        graphics.setColour (look::severityHigh);
        graphics.setFont (juce::FontOptions (12.0f));
        graphics.drawText ("Load failed — " + lastError_, area.removeFromTop (kRowFirstLine),
                           juce::Justification::centredLeft, true);
    }
    else if (rows_.empty())
    {
        graphics.setColour (look::ok);
        graphics.setFont (juce::FontOptions (12.0f));
        graphics.drawText ("Nothing to report: every module, parameter and asset this preset asks for is one "
                           "xerum implements.",
                           area.removeFromTop (kRowFirstLine), juce::Justification::centredLeft, true);
    }

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

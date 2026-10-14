// xerum::plugin — the import report, as the interface shows it.
//
// Ticket `31`, and the half of it that ticket `23`'s contract makes binding: a preset that half-loads
// **must say so to the person using it**. The contract (§3) requires the report to be available
// programmatically, as text in the harness, and in the interface; this class is the interface's form of
// it.
//
// It is deliberately not a modal that says "some items were skipped". A modal is a thing you dismiss, and
// what it dismisses is the *content*: which module, which item, and why it was not honoured. So the
// report is a permanent panel, one row per entry, and each row carries
//
//   * its **severity**, as a chip and as the row's colour (the ramp is in `InterfaceLook.h`);
//   * the **module path**, so a user can find the thing;
//   * the **item** the preset asked for, and the **reason** xerum could not honour it.
//
// A clean import shows the words **"Nothing to report"** and not an empty box, because the contract says
// a clean import's report is *empty, not absent* and the two must not look the same.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"

#include <string>
#include <vector>

namespace xerum::plugin::ui
{

class ImportReportView final : public juce::Component
{
public:
    ImportReportView();

    /// Show a report. The arguments are what the processor's own accessors returned — this class reads
    /// nothing itself, so what it shows is always the same data the harness and the tests read.
    ///
    /// Call `setSize` with the final width **before** this, then call it again with `preferredHeight()`,
    /// because how many lines a row needs depends on the width. `resized()` re-wraps, so the order only
    /// has to be roughly that.
    void setReport (const model::ImportReport& report, juce::String presetName, juce::String lastError,
                    const std::vector<juce::String>& contentRoots);

    int rowCount() const { return static_cast<int> (rows_.size()); }
    model::Severity rowSeverity (int index) const { return rows_[static_cast<std::size_t> (index)].severity; }

    /// One row, composed the way it is painted: severity, module path, item, reason. The test reads this
    /// to prove every entry in the report is an entry on screen.
    juce::String rowText (int index) const;
    std::size_t severityCount (model::Severity severity) const;

    /// The one-line summary: the counts per severity, `Nothing to report` for a clean import, or the load
    /// failure that produced no report at all.
    juce::String summaryText() const { return summary_; }

    /// How tall the rows are at the current width — what the editor sizes its viewport with.
    int preferredHeight() const;

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    struct Row
    {
        model::Severity severity { model::Severity::Info };
        juce::String modulePath, item, reason;
        juce::String text;                 ///< the composed row, for tests
        juce::StringArray itemLines, reasonLines;   ///< wrapped to the current width
        int height { 0 };
    };

    void relayout();
    static juce::StringArray wrap (const juce::String& text, const juce::Font& font, float width);

    std::vector<Row> rows_;
    juce::String summary_, preset_, lastError_, roots_;
    juce::StringArray rootsWrapped_;
    model::Severity worst_ { model::Severity::Info };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImportReportView)
};

} // namespace xerum::plugin::ui

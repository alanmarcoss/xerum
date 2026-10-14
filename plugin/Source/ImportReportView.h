// xerum::plugin — the import report, as the interface shows it.
//
// Ticket `31`, and the half of it that ticket `23`'s contract makes binding: a preset that half-loads
// **must say so to the person using it**. The contract (§3) requires the report to be available
// programmatically, as text in the harness, and in the interface; this file is the interface's form of it,
// and ticket `44` gave it the two halves ticket `43` settled:
//
//   * **`ReportBar` — the persistent bar.** Always on screen, never dismissed: the severity, the summary,
//     the preset it belongs to, and the button that opens the drawer. It is the thing that says
//     `Nothing to report` for a clean import, which the contract requires be distinguishable from an
//     absent report.
//   * **`ImportReportView` — the drawer.** The rows themselves, one per entry, each with its **severity**
//     (a chip and a colour, from the ramp in `InterfaceLook.h`), the **module path** (so a user can find
//     the thing), the **item** the preset asked for, and the **reason** xerum could not honour it — the
//     loader's own strings, because a paraphrase is a second statement of a fact that already exists.
//
// **Never a modal**, which is ticket `43`'s second answer from the dev. A modal is a thing you dismiss,
// and what it dismisses is the *content*: which module, which item, and why it was not honoured. So the
// bar is always there and the drawer is a component in this window, not a dialog in front of it.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"

#include <functional>
#include <string>
#include <vector>

namespace xerum::plugin::ui
{

class ImportReportView;

/// The report's persistent half: a summary, a severity, and the drawer's handle.
class ReportBar final : public juce::Component
{
public:
    ReportBar();

    /// Show a summary. The arguments are what the processor's own accessors returned — this class reads
    /// nothing itself, so what it shows is always the same data the harness and the tests read.
    void setReport (juce::String summary, juce::String presetName, const model::ImportReport& report,
                    juce::String lastError);

    juce::String summaryText() const { return summary_; }
    juce::String presetText() const { return preset_; }
    model::Severity worst() const { return worst_; }

    /// True when the report has no entries and the load did not fail: the state the bar draws in `ok`.
    bool clean() const { return clean_ && ! failed_; }
    bool failed() const { return failed_; }

    void setDrawerOpen (bool open);
    bool drawerOpen() const { return open_; }

    /// What the button does. The drawer is the editor's component, so the bar asks rather than owns.
    std::function<void()> onToggleRequested;

    /// The same thing the button does — the route a test drives it by, without a mouse.
    void toggle();

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    juce::TextButton toggle_;
    juce::String summary_, preset_;
    model::Severity worst_ { model::Severity::Info };
    bool clean_ { true };
    bool failed_ { false };
    bool open_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReportBar)
};

/// The report's drawer: one row per entry, and the content roots the preset's assets were resolved from.
///
/// Call `setSize` with the final width **before** `setReport`, then call it again with `preferredHeight()`,
/// because how many lines a row needs depends on the width. `resized()` re-wraps, so the order only has to
/// be roughly that.
class ImportReportView final : public juce::Component
{
public:
    ImportReportView();

    void setReport (const model::ImportReport& report, juce::String lastError,
                    const std::vector<juce::String>& contentRoots);

    int rowCount() const { return static_cast<int> (rows_.size()); }
    model::Severity rowSeverity (int index) const { return rows_[static_cast<std::size_t> (index)].severity; }

    /// One row, composed the way it is painted: severity, module path, item, reason. The test reads this
    /// to prove every entry in the report is an entry on screen.
    juce::String rowText (int index) const;
    std::size_t severityCount (model::Severity severity) const;

    /// The one-line summary: the counts per severity, `Nothing to report` for a clean import, or the load
    /// failure that produced no report at all. The bar shows it; this class owns it because it is the
    /// report's own summary and both surfaces must not compute it twice.
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
    juce::String summary_, lastError_, roots_;
    juce::StringArray rootsWrapped_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImportReportView)
};

} // namespace xerum::plugin::ui

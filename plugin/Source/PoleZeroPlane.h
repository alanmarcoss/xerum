// xerum::plugin — the drawn filter, as the interface shows it.
//
// Ticket `44`, the second of the two surfaces ticket `43` called hard. `PZ_SVF` is not a filter model
// xerum builds: it is a **response drawn in the preset**, as up to four objects of eight second-order
// pole/zero sections each — 32 sections in series (tickets `02`/`04`, and `dsp::PoleZeroDescription` is
// the engine's own shape for them). The prototype drew a plane for it; this is that plane, and it plots
// what the preset actually carries:
//
//   * **x is frequency**, as the preset stores it — a fraction of Nyquist, which is why many factory
//     values snap to a 1/16 grid;
//   * **a pole is drawn above the axis and a zero below**, at its own frequency, because the two are
//     *separate* frequencies in this model rather than one magnitude response. A pole above and a zero
//     below is the reading that keeps both visible; putting them on one line would hide whichever is
//     second;
//   * **marker size is the section's Q**, which is the third number a section carries (`qp`/`qz`) and the
//     only one left to show.
//
// ## What the plane does not claim
//
// These sections are **shown and not applied**: `dsp::VoiceSettings` carries a `FilterSpec` and no
// pole/zero table, so the plugin reports a drawn response at `high` ("the filter's response is drawn in
// the preset…") and the engine plays the filter's *type*. The caption says so where the sections are,
// because a picture of a response next to a filter that is not producing it must not look like a promise.
// It is also why the plane is a view rather than an editor: `dsp::VoiceFilter::setPoleZero` exists, but
// nothing in `plugin/` reaches it, so dragging a section here would change nothing that is heard.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"
#include "filter.h"

namespace xerum::plugin::ui
{

/// Read a voice filter's drawn response out of a loaded document — `VoiceFilter{slot}/PZs[]`, which is
/// where a preset inlines it. Empty when the preset carries none, and never a partial object: the
/// reference stores eight sections per object or none.
dsp::PoleZeroDescription poleZeroFromDocument (const model::PresetDocument& document, int filterSlot);

/// The pole/zero plane: every section the preset drew, up to the engine's own cap of 32.
class PoleZeroPlane final : public juce::Component
{
public:
    PoleZeroPlane();

    /// Which voice filter this plane belongs to, and the type the preset selected for it — the type is
    /// shown because it is what the engine will actually play.
    void setFilter (int filterSlot, juce::String typeName);
    void setSections (dsp::PoleZeroDescription description);

    int filterSlot() const { return filterSlot_; }
    int sectionCount() const { return static_cast<int> (sections_.count); }
    const dsp::PoleZeroDescription& sections() const { return sections_; }

    /// The engine's cap, stated where the plane is: `PoleZeroDescription::kMaxSections`.
    static int sectionCapacity() { return static_cast<int> (dsp::PoleZeroDescription::kMaxSections); }

    /// The line under the plane: how many of the 32 sections are drawn, and that they are shown rather
    /// than applied.
    juce::String caption() const;

    /// Where a section's **pole** is drawn, in this component's coordinates. The test reads it back so
    /// that "the plane shows its sections" is measured rather than asserted about.
    juce::Point<float> polePoint (int section) const;

    /// Where a section's **zero** is drawn.
    juce::Point<float> zeroPoint (int section) const;

    void paint (juce::Graphics& graphics) override;

private:
    int filterSlot_ { 0 };
    juce::String type_;
    dsp::PoleZeroDescription sections_;
};

} // namespace xerum::plugin::ui

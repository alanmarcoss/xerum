// xerum::plugin — the interface.
//
// The interface's shape is designed here; the **binding is model-driven**, which is ticket `13`'s rule:
// every control is created from a `ParamSpec` and bound to the bridge's own parameter object, so adding a
// parameter to the schema adds a control, and the interface cannot be missing one the engine has.
// `ui::planInterface` places the pages; `ui::ParameterControl` builds and binds one parameter's control;
// `ui::ImportReportView` shows ticket `23`'s report.
//
// ## Ticket `44`: the structure is variant 3, "Devices"
//
// Ticket `43` settled the design and this is it, implemented:
//
//   1. **one device per page, behind a strip that always shows the others** (`ui::DeviceStrip`). A device
//      is a plan **panel** — one `ParamSpec::group`, which is the model's own grouping — so the pages are
//      derived from the table and nothing is hand-listed. The matrix is the one page the table does not
//      derive, and the reason is stated where it is built;
//   2. **the two hard surfaces**: the modulation matrix as real columns (`ui::ModulationMatrixView`, 64
//      rows) and the drawn filter as a pole/zero plane (`ui::PoleZeroPlane`, up to 32 sections);
//   3. **the report is a bar plus a drawer, never a modal** (`ui::ReportBar` + `ui::ImportReportView`) —
//      the bar always says what happened, including `Nothing to report` for a clean import;
//   4. **the visual identity is xerum's own** (`InterfaceLook.h`) — structural familiarity, not the
//      reference's skin.
//
// The coverage readout in the header is part of the work rather than decoration: the bridge binds a
// fraction of the schema's leaves, and the interface says so on screen instead of implying that what is
// shown is all there is.
//
// ## The editor must not exist until a host opens it
//
// The headless harness is how the reference is driven and how this plugin is validated, so nothing here
// may be constructed eagerly. This class counts its own constructions (`instancesCreated`) and live
// instances (`instancesAlive`) so that the claim is a measurement rather than a promise: a test drives the
// processor through prepare, render and state restore and requires zero editors, and the count would fail
// if anyone made the editor eager. Nothing in this file is reachable from `processBlock`, and the two
// drawn surfaces read the loaded document on the **message thread** only.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DeviceStrip.h"
#include "ImportReportView.h"
#include "InterfacePlan.h"
#include "ModulationMatrixView.h"
#include "ParameterPanels.h"
#include "PoleZeroPlane.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xerum::plugin
{

class XerumAudioProcessor;

class XerumAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::ChangeListener
{
public:
    explicit XerumAudioProcessorEditor (XerumAudioProcessor& processor);
    ~XerumAudioProcessorEditor() override;

    //==============================================================================
    // The interface's own surface, for the tests and for the next reader.

    /// How many editors this process has constructed, and how many are alive right now. A host opening
    /// the interface is the only thing that moves these.
    static int instancesCreated();
    static int instancesAlive();

    /// The layout the editor built from the parameter table.
    const ui::InterfacePlan& plan() const { return plan_; }

    /// One control per parameter the bridge exposes — the coverage ticket `31` requires be assertable.
    int controlCount() const;
    ui::ParameterControl* controlFor (std::string_view id) const;

    /// `controlCount()` **plus the 512 bound matrix cells**: every control on screen, which is what the
    /// header's coverage readout counts. Ticket `45` — leaving the matrix cells out would understate what
    /// is bound by 512 now that the cells are real controls.
    int boundControlCount() const;

    /// How many schema leaves the interface *does not* bind. Shown in the header, so the gap is visible
    /// rather than implied away.
    int schemaLeafCount() const { return schemaLeaves_; }

    //==============================================================================
    // The structure: one device per page, behind a strip that always shows the others.

    /// One per plan panel, plus the matrix page.
    int deviceCount() const { return static_cast<int> (devices_.size()); }
    juce::String deviceTitle (int index) const;
    juce::String deviceModule (int index) const;
    bool deviceIsMatrix (int index) const;

    /// The strip itself: what a user clicks, and what says which device is showing.
    const ui::DeviceStrip& deviceStrip() const { return strip_; }

    /// Which page is showing, and what a strip click does.
    int currentDevice() const { return currentDevice_; }
    void showDevice (int index);

    /// True while the matrix is the page. The matrix is the one device the table does not derive.
    bool matrixPageVisible() const { return matrixViewport_.isVisible(); }

    /// The panel a device page shows, or nullptr for the matrix page.
    ui::ParameterPanel* panelForDevice (int index) const;
    ui::ParameterPanel* shownPanel() const { return panelForDevice (currentDevice_); }

    //==============================================================================
    // The two surfaces ticket `43` called hard.

    const ui::ModulationMatrixView& matrixView() const { return matrix_; }
    ui::ModulationMatrixView& matrixView() { return matrix_; }
    const ui::PoleZeroPlane& poleZeroPlane (int filterSlot) const;

    /// Non-const, because the plane **edits** since ticket `45`: a test drives a drag through it the way
    /// the mouse does, rather than reaching past it to the processor.
    ui::PoleZeroPlane& poleZeroPlane (int filterSlot);

    //==============================================================================
    // The report surface: a bar plus a drawer, never a modal.

    const ui::ImportReportView& reportView() const { return reportView_; }
    const ui::ReportBar& reportBar() const { return reportBar_; }
    bool reportDrawerOpen() const { return reportBar_.drawerOpen(); }
    void setReportDrawerOpen (bool open);

    /// Read the processor's report and show it. Called on a change message (a preset load or a state
    /// restore) and once at construction; public because the tests drive it without a message loop.
    void refreshReport();

    const juce::TextButton& importButton() const { return import_; }

    void paint (juce::Graphics& graphics) override;
    void resized() override;

private:
    struct Device
    {
        std::string title;   ///< the group, or "Matrix"
        std::string module;  ///< the model module, e.g. `Oscillator0` — what the strip's dot keys on
        int panel { -1 };    ///< index into `panels_`, or -1 for the matrix page
        int plane { -1 };    ///< index into `planes_`, or -1
    };

    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void importPreset();
    void updateDeviceSeverity (const model::ImportReport& report);
    int naturalHeight() const;
    int naturalWidth() const;

    XerumAudioProcessor& processor_;

    ui::InterfacePlan plan_;
    juce::OwnedArray<ui::ParameterPanel> panels_;
    std::vector<Device> devices_;
    ui::DeviceStrip strip_;

    juce::OwnedArray<ui::PoleZeroPlane> planes_;

    ui::ModulationMatrixView matrix_;
    juce::Viewport matrixViewport_;

    ui::ImportReportView reportView_;
    juce::Viewport reportViewport_;
    ui::ReportBar reportBar_;

    juce::Rectangle<int> pageArea_;

    juce::TextButton import_ { "Import .SerumPreset…" };
    juce::Label status_;
    std::unique_ptr<juce::FileChooser> chooser_;

    int currentDevice_ { 0 };
    int schemaLeaves_ { 0 };

    static std::atomic<int> created_;
    static std::atomic<int> alive_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XerumAudioProcessorEditor)
};

} // namespace xerum::plugin

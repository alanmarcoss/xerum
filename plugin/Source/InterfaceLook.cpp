// xerum::plugin — the design system's look-and-feel. See `InterfaceLook.h`.
//
// Ticket `53`. One colour in this interface was not a token: JUCE's own dark-scheme `defaultFill`,
// `#42a2c8`, which `LookAndFeel_V4` puts behind a scroll bar's thumb, a focused picker's outline and a
// dropdown's highlight. The window capture found it at 0.416 % of the window, drawn by the matrix
// viewport's scroll bars. This file is where that can no longer happen: every colour id the interface's
// stock widgets consult is given a token, and the palette has no blue in it at all.
//
// Two halves, and the second is the one that matters:
//
//   1. a `ColourScheme` built entirely from the palette, which is what `LookAndFeel_V4` derives most of
//      its ids from — `widgetBackground`, `outline`, `defaultText`, `defaultFill` (the blue),
//      `highlightedFill`, `menuBackground`, `menuText`, `windowBackground`;
//   2. the ids `LookAndFeel_V4` fixes by hand after its scheme, overridden for every stock class this
//      interface can contain: the pickers, the knobs, the pills, the buttons, the labels, the scroll
//      bars, the dropdowns, the text editors, the tooltips and the alerts.
//
// A class that is *not* listed (a `Toolbar`, a `TreeView`, a `PropertyPanel`) has no instance in this
// interface; if one ever arrives, `plugin/tests/interface_test.cpp`'s tree walk is what will notice, because
// it asks each component in the editor for the ids its class uses and requires a token from each.

#include "InterfaceLook.h"

namespace xerum::plugin::ui::look
{

namespace
{
/// The scheme, from the palette. `defaultFill` is the scroll bar's thumb — the blue that was on screen —
/// and it is `borderHi` here: the design's interactive-chrome colour, which is what a thumb is.
juce::LookAndFeel_V4::ColourScheme schemeFromPalette()
{
    return { bg,        // windowBackground
             surface,   // widgetBackground
             surface,   // menuBackground: a dropdown is a panel over the page
             border,    // outline
             fg,        // defaultText
             borderHi,  // defaultFill: the scroll bar's thumb, the focused outline
             accent,    // highlightedText
             accentBg,  // highlightedFill
             fgDim };   // menuText
}
} // namespace

XerumLookAndFeel::XerumLookAndFeel()
    : juce::LookAndFeel_V4 (schemeFromPalette())
{
    // The pickers: the matrix's source and destination combos, the warp-mode and filter-type pickers.
    setColour (juce::ComboBox::backgroundColourId, bg);
    setColour (juce::ComboBox::textColourId, fg);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::buttonColourId, surface2);
    setColour (juce::ComboBox::arrowColourId, accent);
    setColour (juce::ComboBox::focusedOutlineColourId, accentDim);

    // The dropdown a picker opens: a `surface` panel with the design's own accent highlight.
    setColour (juce::PopupMenu::backgroundColourId, surface);
    setColour (juce::PopupMenu::textColourId, fg);
    setColour (juce::PopupMenu::headerTextColourId, fgMuted);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentBg);
    setColour (juce::PopupMenu::highlightedTextColourId, accent);

    // A slider, for the ids `ui::Knob` does not draw itself: the design's knob body is `surface2` and its
    // arc is `accent`.
    setColour (juce::Slider::backgroundColourId, surface2);
    setColour (juce::Slider::thumbColourId, accent);
    setColour (juce::Slider::trackColourId, border);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, border);
    setColour (juce::Slider::textBoxTextColourId, fg);
    setColour (juce::Slider::textBoxBackgroundColourId, bg);
    setColour (juce::Slider::textBoxOutlineColourId, border);
    setColour (juce::Slider::textBoxHighlightColourId, accentBg);

    // The pills and the buttons: the design's `toggle` and `btn-opt` states.
    setColour (juce::ToggleButton::textColourId, fgDim);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, fgMuted);

    setColour (juce::TextButton::buttonColourId, surface2);
    setColour (juce::TextButton::buttonOnColourId, accentDim);
    setColour (juce::TextButton::textColourOffId, fgDim);
    setColour (juce::TextButton::textColourOnId, fg);

    setColour (juce::Label::textColourId, fg);
    setColour (juce::Label::textWhenEditingColourId, fg);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    // **The scroll bars**, which is where the blue was: the matrix's own viewport. A `border-hi` thumb on
    // the window's own background — chrome, and the design's own chrome colours.
    setColour (juce::ScrollBar::backgroundColourId, bg);
    setColour (juce::ScrollBar::thumbColourId, borderHi);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    // The few remaining surfaces the editor's own tree can reach: the caret, a text editor, a tooltip, an
    // alert, a list, and the window itself (the standalone's wrapper is JUCE's own component).
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::TextEditor::backgroundColourId, bg);
    setColour (juce::TextEditor::textColourId, fg);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accentDim);
    setColour (juce::TextEditor::highlightColourId, accentBg);
    setColour (juce::TextEditor::highlightedTextColourId, accent);
    setColour (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, surface2);
    setColour (juce::TooltipWindow::textColourId, fg);
    setColour (juce::TooltipWindow::outlineColourId, border);
    setColour (juce::AlertWindow::backgroundColourId, surface);
    setColour (juce::AlertWindow::textColourId, fg);
    setColour (juce::AlertWindow::outlineColourId, border);
    setColour (juce::ListBox::backgroundColourId, bg);
    setColour (juce::ListBox::outlineColourId, border);
    setColour (juce::ListBox::textColourId, fg);
    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::textColourId, fg);
    setColour (juce::GroupComponent::outlineColourId, border);
    setColour (juce::GroupComponent::textColourId, fgDim);
}

} // namespace xerum::plugin::ui::look

// xerum::plugin — the interface's palette.
//
// Ticket `10`'s decision: v1's interface is a **designed** interface and it is xerum's own, not a
// reproduction of the reference's look (trade dress, and it has to survive the open-source pivot). So
// these are xerum's colours, chosen here and used nowhere else. Graphite panels, one teal accent, and a
// severity ramp for the import report that is a ramp rather than three words.
//
// Ticket `44` added the report *bar*'s own two colours — `ok` for a clean import and `barFill` for the
// bar the drawer opens out of — because the report surface is a bar plus a drawer and a bar that floats
// over the page needs to read as furniture rather than as page content.
//
// It is a header of constants rather than a `LookAndFeel` subclass on purpose: a custom look-and-feel
// rewrites how every widget draws, and this interface needs to colour its own components and leave the
// standard widgets behaving the way JUCE's tests and validators expect.

#pragma once

#include <juce_graphics/juce_graphics.h>

#include "document.h"

namespace xerum::plugin::ui::look
{

inline const juce::Colour background { 0xff15171a };
inline const juce::Colour panelFill { 0xff1e2228 };
inline const juce::Colour panelBorder { 0xff2e3440 };
inline const juce::Colour rowDivider { 0xff262b33 };
inline const juce::Colour text { 0xffe6e9ee };
inline const juce::Colour dimText { 0xff8d97a6 };
inline const juce::Colour accent { 0xff54c9b4 };
inline const juce::Colour track { 0xff2b313a };

/// The device strip, and the report bar that opens the drawer.
inline const juce::Colour barFill { 0xff1a1e24 };
inline const juce::Colour selectedTab { 0xff262d35 };

/// A clean import's colour, the fourth of the report's states: nothing to report is a *state*, and it
/// must not be drawn in the colour of a severity or in the colour of "no data".
inline const juce::Colour ok { 0xff5fae7a };

inline const juce::Colour severityHigh { 0xffe2604f };
inline const juce::Colour severityMedium { 0xffd9a13c };
inline const juce::Colour severityInfo { 0xff6f9ac9 };

/// The severity's name, as the report prints it. `model::toString` is the single source of the word, so
/// the interface and the harness cannot call a severity two different things.
inline juce::String severityName (const xerum::model::Severity severity)
{
    return juce::String (std::string (xerum::model::toString (severity)));
}

/// The report's severity ramp. One function, so the row colour and the summary colour cannot disagree.
inline juce::Colour severityColour (const xerum::model::Severity severity)
{
    switch (severity)
    {
        case xerum::model::Severity::High:   return severityHigh;
        case xerum::model::Severity::Medium: return severityMedium;
        case xerum::model::Severity::Info:   return severityInfo;
    }

    return severityInfo;
}

} // namespace xerum::plugin::ui::look

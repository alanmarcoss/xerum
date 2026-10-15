// xerum::plugin — the interface's design tokens.
//
// Ticket `51`. This header **is** `docs/design/xerum-design-system.html`: the fourteen roles of the
// palette, the type scale, the spacing grid and the control geometry, transcribed so that every colour,
// size and radius the drawing code uses is named here and nowhere else. No hex lives outside this file,
// which is what makes the cold and phosphor directions a swap of these constants rather than a rewrite:
// the drawing code asks for `surface` or `accent`, never for a colour it chose itself.
//
// Ticket `10`'s decision is unchanged and is why this is xerum's own palette rather than the reference's:
// xerum reads the reference's preset format but is an independent product, so it has its own colours,
// its own type and its own control shapes. `docs/content-policy.md` and the map's Out-of-scope section
// both stand.
//
// A header of constants rather than a `LookAndFeel` subclass, on purpose: a custom look-and-feel rewrites
// how every stock widget draws, and this interface draws its own components (the `ui::Knob`, the device
// tabs, the plane) while leaving `juce::ComboBox` and `juce::Slider` behaving the way JUCE's own tests and
// the plugin validators expect. The two stock widgets that remain are themed through JUCE's colour IDs
// from this file's tokens.
//
// ## Type
//
// The design's families are DM Sans (body) and IBM Plex Mono (numerals), both OFL. **Neither is embedded.**
// The load-bearing use is the monospaced numerals — a value that jitters as it changes is the thing to
// avoid — and `juce::Font::getDefaultMonospacedFontName()` is a monospaced face that ships with JUCE, so
// the numbers do not jitter and no font binary enters the repository (and therefore `THIRD_PARTY.md`
// needs no new entry). Body text uses JUCE's default sans. Recorded here because it is a decision.

#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "document.h"

#include <vector>

namespace xerum::plugin::ui::look
{

//==============================================================================
// 1. Palette — the design system's roles, verbatim.

inline const juce::Colour bg { 0xff111110 };          ///< window background
inline const juce::Colour surface { 0xff1a1917 };     ///< panel background
inline const juce::Colour surface2 { 0xff222120 };    ///< knob body, header gradient, hover fills
inline const juce::Colour border { 0xff2a2826 };      ///< default borders, knob track, grid lines
inline const juce::Colour borderHi { 0xff3a3836 };    ///< hover borders
inline const juce::Colour fg { 0xffd4cec4 };          ///< primary text, knob pointer, readouts
inline const juce::Colour fgDim { 0xff7a756b };       ///< labels, secondary text, inactive tab
inline const juce::Colour fgMuted { 0xff4a4640 };     ///< section titles, disabled text, axis labels
inline const juce::Colour accent { 0xffe89b30 };      ///< active values, value arc, active tab
inline const juce::Colour accentDim { 0xff8a5a18 };   ///< active borders, modulation arc
inline const juce::Colour accentBg { 0x1ae89b30 };    ///< rgba(232,155,48,0.10): active fills

inline const juce::Colour red { 0xffd94848 };         ///< high severity, load failure
inline const juce::Colour orange { 0xffd98848 };      ///< medium severity, warning
inline const juce::Colour green { 0xff50b868 };       ///< info severity, clean import, bypass on

/// The accent ring a focused control draws, and the focus stroke width. The design's "focused" state is a
/// knob whose body stroke becomes the accent; a keyboard user gets the same treatment and the ring is
/// named here so every focusable control draws it the same way.
inline constexpr float kFocusStroke = 1.5f;

//==============================================================================
// 2. Type scale — sizes in px, from the design system. The families are JUCE's defaults (see above).

namespace type
{
inline constexpr float logo = 13.0f;        ///< the wordmark in the header
inline constexpr float section = 9.0f;      ///< panel and section titles
inline constexpr float label = 8.0f;        ///< knob and toggle labels
inline constexpr float tag = 7.0f;          ///< tags, polarity, fx type
inline constexpr float readout = 9.0f;      ///< knob readouts (monospaced)
inline constexpr float readoutSm = 8.0f;    ///< small knob readouts, table cells (monospaced)
inline constexpr float data = 8.0f;         ///< matrix cells, import bar, preset name (monospaced)
inline constexpr float tableHead = 7.0f;    ///< matrix column headers (monospaced)
} // namespace type

/// The body face and the monospaced face, so no call site names a font family.
inline juce::Font bodyFont (const float size, const bool bold = false);
inline juce::Font monoFont (const float size, const bool bold = false);

//==============================================================================
// 3. Spacing grid — base unit 2px.

namespace space
{
inline constexpr int section = 3;    ///< between major rows
inline constexpr int panel = 3;      ///< inside a panel: title, controls, toggles
inline constexpr int knob = 4;       ///< between knobs in a row
inline constexpr int inlineGap = 6;  ///< between inline elements
inline constexpr int row = 6;        ///< between panels in the same row
inline constexpr int fx = 4;         ///< between FX cards
inline constexpr int macro = 6;      ///< between macro knobs
} // namespace space

/// Panel inner padding, from the design's `--pad-panel: 5px 7px`.
inline constexpr int padPanelX = 7;
inline constexpr int padPanelY = 5;
inline constexpr int radiusPanel = 5;
inline constexpr int radiusPill = 12;   ///< toggle and button pills
inline constexpr int radiusTab = 16;    ///< header tabs

//==============================================================================
// 4. Control geometry — the knob, at the design's exact diameters.

inline constexpr int knobDiameter = 44;      ///< the default knob
inline constexpr int knobSmallDiameter = 28; ///< a fixed slot's knob (NOISE, SUB)
inline constexpr int macroDiameter = 26;     ///< a macro knob

/// The arc every knob sweeps: start at 225° (7 o'clock), sweep 270° clockwise to 315° (5 o'clock). Angles
/// are JUCE's — radians, zero at 12 o'clock, positive clockwise — so the SVG's 225° and the 315° the design
/// names are `pi * 1.25` and `pi * 2.75`.
inline constexpr float knobStartRadians = 3.92699081f;   // 225° = pi * 1.25
inline constexpr float knobEndRadians = 8.63937980f;     // 495° = pi * 2.75 (315° + a full turn)

/// The knob's parts as fractions of its diameter, from the 44px spec: body r=18, track r=15, stroke 3.5,
/// pointer r=2 at track radius. Small and macro knobs use the same fractions, which is what the design
/// system's own scaling does.
inline constexpr float knobBodyFraction = 18.0f / 44.0f;
inline constexpr float knobTrackFraction = 15.0f / 44.0f;
inline constexpr float knobStrokeFraction = 3.5f / 44.0f;
inline constexpr float knobPointerFraction = 2.0f / 44.0f;
inline constexpr float knobModStrokeFraction = 2.0f / 44.0f;

//==============================================================================
// 5. The import report's severity ramp.

/// The severity's name, as the report prints it. `model::toString` is the single source of the word, so
/// the interface and the harness cannot call a severity two different things.
inline juce::String severityName (const xerum::model::Severity severity)
{
    return juce::String (std::string (xerum::model::toString (severity)));
}

/// The severity's colour, as the designed surface draws it: **high red, medium orange, info green** — the
/// design system's own three-colour ramp, and the same green the clean bar uses. One function, so a row's
/// colour and the summary's colour cannot disagree.
///
/// `green` carries both the info severity and a clean import, which is the one overload the design system's
/// three colours force: the states are told apart by their *words*, `Nothing to report` against
/// `n items — n info`, which is exactly the distinction the contract requires be visible. A load failure
/// is not a severity at all and is drawn in `red` by the bar.
inline juce::Colour severityColour (const xerum::model::Severity severity)
{
    switch (severity)
    {
        case xerum::model::Severity::High:   return red;
        case xerum::model::Severity::Medium: return orange;
        case xerum::model::Severity::Info:   return green;
    }

    return green;
}

//==============================================================================
// 7. The look-and-feel the interface's stock widgets ask for their colours through
//
// Ticket `51` themed the two stock widgets that remained **per instance**, through JUCE's own colour ids,
// and left everything else at JUCE's default — which is `LookAndFeel_V4`'s dark scheme, whose
// `defaultFill` is `#42a2c8`, a blue that is not in this palette. Ticket `53`'s window capture found
// exactly that colour on screen (0.416 % of the window, drawn by the matrix viewport's own scroll bars),
// and the same scheme draws a focused picker's outline, a dropdown's highlight and a tooltip's frame.
//
// This class is the token list's answer: a `ColourScheme` built **entirely** from the palette above, then
// the colour ids `LookAndFeel_V4` fixes by hand, each of them a token too. The editor owns one and sets it
// on itself, so every drawn surface beneath it — including components JUCE creates later, like a scroll
// bar — resolves a colour from this file or from nothing. `plugin/tests/interface_test.cpp` walks the
// editor's own component tree and asserts that, id by id, rather than trusting it.
class XerumLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    XerumLookAndFeel();
};

/// Every colour this palette names, for a caller that has to show a drawn surface used none other — the
/// test's tree walk. One list, in this file, so a token added here is checkable there without a second
/// list to keep in step.
inline std::vector<juce::Colour> palette()
{
    return { bg,      surface,   surface2,  border,    borderHi, fg,       fgDim,
             fgMuted, accent,    accentDim, accentBg,  red,      orange,   green };
}

//==============================================================================
// 8. The two faces, defined out of line so this header stays a token list.

inline juce::Font bodyFont (const float size, const bool bold)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font monoFont (const float size, const bool bold)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size,
                                          bold ? juce::Font::bold : juce::Font::plain));
}

} // namespace xerum::plugin::ui::look

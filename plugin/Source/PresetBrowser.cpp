// xerum::plugin — the factory bank's browser, in code. See PresetBrowser.h.

#include "PresetBrowser.h"

#include "InterfaceLook.h"
#include "json.h"

#include <algorithm>
#include <cmath>

namespace xerum::plugin::ui
{

namespace
{

/// The design's own numbers, from the tokens and from the two components this borrows its grammar from:
/// the preset field is the header's (a `bg` box on `fg-dim` at 10px mono, the design system's §7), the
/// arrows are the wavetable slot's 16px triangles, the pills are the composition strip's tabs, and a row
/// is the matrix's own row height.
constexpr int kMargin = 10;
constexpr int kArrowSize = 16;
constexpr int kArrowGap = 4;
constexpr int kHeadingHeight = 16;
constexpr int kFieldHeight = 22;
constexpr int kPillHeight = 20;
constexpr int kPillGap = 4;
constexpr int kPillPadding = 10;
constexpr int kFooterHeight = 30;
constexpr int kRowGap = 4;
constexpr int kScrollBarWidth = 3;

juce::String join (const juce::String& left, const juce::String& right)
{
    return left.isEmpty() ? right : left + " \xc2\xb7 " + right;   // the design's ` · ` separator
}

/// The pill's own width: the design's `padding: 2px 7px` at the strip's scale, measured from the text so
/// a longer category is a wider pill rather than a clipped one.
int pillWidth (const juce::String& text, const juce::Font& font)
{
    return static_cast<int> (juce::GlyphArrangement::getStringWidth (font, text)) + kPillPadding * 2;
}

void drawArrow (juce::Graphics& graphics, const juce::Rectangle<int>& box, const int step,
                const bool highlighted)
{
    // The design's arrows are glyphs; drawn as paths so the direction is a shape rather than a character
    // a fallback font might render as a box. The same triangle as the wavetable slot's.
    graphics.setColour (look::bg);
    graphics.fillRoundedRectangle (box.toFloat(), 3.0f);
    graphics.setColour (highlighted ? look::borderHi : look::border);
    graphics.drawRoundedRectangle (box.toFloat().reduced (0.5f), 3.0f, 1.0f);

    const auto centre = box.toFloat().getCentre();
    const auto half = 3.0f;
    juce::Path triangle;

    if (step < 0)
        triangle.addTriangle (centre.x + half, centre.y - half, centre.x + half, centre.y + half,
                              centre.x - half, centre.y);
    else
        triangle.addTriangle (centre.x - half, centre.y - half, centre.x - half, centre.y + half,
                              centre.x + half, centre.y);

    graphics.setColour (highlighted ? look::fg : look::fgDim);
    graphics.fillPath (triangle);
}

} // namespace

//==============================================================================
juce::File BankCatalog::fileFor (const int index) const
{
    if (index < 0 || index >= size() || root == juce::File())
        return {};

    return juce::File (juce::String::fromUTF8 (
        xerum::presets::manifestPathUnder (root.getFullPathName().toStdString(), presets[static_cast<std::size_t> (index)])
            .c_str()));
}

int BankCatalog::indexOfName (const juce::String& name) const
{
    for (int index = 0; index < size(); ++index)
        if (name == juce::String (presets[static_cast<std::size_t> (index)].name))
            return index;

    return -1;
}

BankCatalog readBankCatalog (const std::vector<juce::String>& contentRoots)
{
    BankCatalog out;

    // The bank is content, so it is found the way the product finds content: the roots in their own
    // order, and the first one that carries a manifest **is** the bank. A broken manifest is reported
    // rather than skipped — falling through to another root would silently browse a different bank than
    // the one that is installed.
    for (const auto& root : contentRoots)
    {
        const auto directory = juce::File (root).getChildFile (juce::String (std::string (xerum::presets::kBankDirectory)));
        const auto manifest = directory.getChildFile (juce::String (std::string (xerum::presets::kBankManifestName)));

        if (! manifest.existsAsFile())
            continue;

        out.root = juce::File (root);

        const auto parsed = xerum::presets::readBankManifest (manifest.loadFileAsString().toStdString());

        if (! parsed.ok)
        {
            out.error = parsed.error + " (" + manifest.getFullPathName().toStdString() + ")";
            return out;
        }

        out.ok = true;
        out.categories = parsed.categories;
        out.presets = parsed.presets;
        return out;
    }

    out.error = "no factory bank in any content root (looked for " + std::string (xerum::presets::kBankDirectory)
                + "/" + std::string (xerum::presets::kBankManifestName) + ")";
    return out;
}

//==============================================================================
PresetField::PresetField()
{
    setWantsKeyboardFocus (false);
}

void PresetField::setName (juce::String name)
{
    name_ = std::move (name);
    repaint();
}

void PresetField::setFactoryCategory (juce::String category)
{
    category_ = std::move (category);
    repaint();
}

juce::String PresetField::displayText() const
{
    if (name_.isEmpty())
        return category_.isEmpty() ? juce::String ("init") : category_ + " \xc2\xb7 init";

    // **The factory bank says where it is filed.** A preset that arrived through the import path has no
    // category, and so no prefix — which is the distinction ticket `55` requires be visible.
    return join (category_, name_);
}

juce::Rectangle<int> PresetField::fieldBounds() const
{
    return getLocalBounds();
}

juce::Rectangle<int> PresetField::arrowBounds (const int step) const
{
    const auto top = (getHeight() - kArrowSize) / 2;
    const auto right = getWidth() - 2 - kArrowSize;

    return step < 0 ? juce::Rectangle<int> (right - kArrowSize - kArrowGap, top, kArrowSize, kArrowSize)
                    : juce::Rectangle<int> (right, top, kArrowSize, kArrowSize);
}

juce::Rectangle<int> PresetField::textBounds() const
{
    return fieldBounds().withTrimmedLeft (7).withTrimmedRight (kArrowSize * 2 + kArrowGap + 6);
}

void PresetField::resized()
{
    repaint();
}

void PresetField::paint (juce::Graphics& graphics)
{
    // The design's `preset-name`: 10px IBM Plex Mono in a `bg` box, 1px `border`, radius 5.
    const auto box = fieldBounds().toFloat().reduced (0.5f);

    graphics.setColour (look::bg);
    graphics.fillRoundedRectangle (box, static_cast<float> (look::radiusPanel));
    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (box, static_cast<float> (look::radiusPanel), 1.0f);

    graphics.setColour (look::fgDim);
    graphics.setFont (look::monoFont (10.0f));
    graphics.drawText (displayText(), textBounds(), juce::Justification::centredLeft, true);

    for (const auto step : { -1, 1 })
        drawArrow (graphics, arrowBounds (step), step, hovered_ == step);
}

void PresetField::mouseDown (const juce::MouseEvent& event)
{
    for (const auto step : { -1, 1 })
        if (arrowBounds (step).contains (event.getPosition()))
        {
            if (onStep != nullptr)
                onStep (step);

            return;
        }

    if (onOpen != nullptr)
        onOpen();
}

void PresetField::mouseMove (const juce::MouseEvent& event)
{
    auto found = 0;

    for (const auto step : { -1, 1 })
        if (arrowBounds (step).contains (event.getPosition()))
            found = step;

    if (found == hovered_)
        return;

    hovered_ = found;
    repaint();
}

void PresetField::mouseExit (const juce::MouseEvent&)
{
    if (hovered_ == 0)
        return;

    hovered_ = 0;
    repaint();
}

//==============================================================================
PresetBrowser::PresetBrowser()
{
    setWantsKeyboardFocus (true);
    setInterceptsMouseClicks (true, false);
}

void PresetBrowser::setCatalog (BankCatalog catalog)
{
    catalog_ = std::move (catalog);
    search_.clear();
    category_ = -1;
    selected_ = -1;
    topRow_ = 0;
    hoveredRow_ = -1;
    refilter (false);
    resized();
    repaint();
}

int PresetBrowser::rowOfIndex (const int index) const
{
    const auto found = std::find (visible_.begin(), visible_.end(), index);

    return found == visible_.end() ? -1 : static_cast<int> (std::distance (visible_.begin(), found));
}

int PresetBrowser::rowHeight() const
{
    return kRowHeight;
}

void PresetBrowser::refilter (const bool keepSelection)
{
    // The filter is a **view** over the manifest's order, never a reordering of it: the rows a category
    // shows are the manifest's own subsequence, so "the order a player moves through them" survives.
    visible_.clear();

    for (int index = 0; index < catalog_.size(); ++index)
    {
        const auto& preset = catalog_.presets[static_cast<std::size_t> (index)];

        if (category_ >= 0 && juce::String (preset.category)
                                  != juce::String (catalog_.categories[static_cast<std::size_t> (category_)]))
            continue;

        if (search_.isNotEmpty()
            && juce::String (preset.name).toLowerCase().contains (search_.toLowerCase()) == false)
            continue;

        visible_.push_back (index);
    }

    if (keepSelection)
        clampScroll();
    else
        topRow_ = 0;
}

void PresetBrowser::clampScroll()
{
    rowsVisible_ = juce::jmax (1, list_.getHeight() / kRowHeight);
    const auto rows = visibleCount();
    const auto maximum = juce::jmax (0, rows - rowsVisible_);

    topRow_ = juce::jlimit (0, maximum, topRow_);

    // Keep the cursor on screen: auditioning with the arrow keys is only useful if the row being auditioned
    // is the row being read.
    const auto row = rowOfIndex (selected_);

    if (row < 0)
        return;

    if (row < topRow_)
        topRow_ = row;
    else if (row >= topRow_ + rowsVisible_)
        topRow_ = row - rowsVisible_ + 1;

    topRow_ = juce::jlimit (0, maximum, topRow_);
}

bool PresetBrowser::setSelectedIndex (const int index, const bool notify)
{
    if (index < -1 || index >= catalog_.size())
        return false;

    if (index == selected_)
    {
        clampScroll();
        repaint();
        return false;
    }

    // -1 is a real state: what is loaded is not one of the bank's, so the browser has no row to point at.
    // The list stays where it is; only the cursor goes.
    selected_ = index;
    clampScroll();
    repaint();

    if (notify && selected_ >= 0 && onLoad != nullptr)
        onLoad (selected_);

    return true;
}

bool PresetBrowser::selectNext (const int step)
{
    if (visible_.empty())
        return false;

    const auto row = rowOfIndex (selected_);
    const auto count = visibleCount();
    const auto next = row < 0 ? (step > 0 ? 0 : count - 1)
                              : ((row + step) % count + count) % count;

    setSelectedIndex (visible_[static_cast<std::size_t> (next)]);
    return true;
}

bool PresetBrowser::stepPreset (const int step)
{
    if (catalog_.isEmpty())
        return false;

    // The whole bank, in the manifest's order — its categories and their own order are the manifest's
    // promise, so one step is "the next sound", wherever the filter happens to be pointing.
    const auto index = ((selected_ < 0 ? (step > 0 ? -1 : 0) : selected_) + step) % catalog_.size();

    setSelectedIndex ((index % catalog_.size() + catalog_.size()) % catalog_.size());
    return true;
}

void PresetBrowser::setSearch (juce::String text)
{
    search_ = std::move (text);
    refilter (true);

    // The query never changes what is *loaded* while the loaded preset still matches. When it does not,
    // the first row that does becomes the audition — which is what makes typing a useful way to move —
    // and a query that matches nothing leaves what is playing exactly where it was.
    if (rowOfIndex (selected_) < 0 && ! visible_.empty())
        setSelectedIndex (visible_.front());

    clampScroll();
    repaint();
}

void PresetBrowser::setCategoryIndex (int index, const bool notify)
{
    const auto total = static_cast<int> (catalog_.categories.size()) + 1;   // `All` plus the categories

    if (total <= 1)
    {
        category_ = -1;
    }
    else
    {
        // `All` is -1 and the categories follow, so one step from the last pill is back to `All`: the
        // first cut is a ring, like the presets themselves.
        const auto shifted = ((index + 1) % total + total) % total;
        category_ = shifted - 1;
    }

    refilter (false);

    if (notify && ! visible_.empty())
        setSelectedIndex (visible_.front());
    else
        clampScroll();

    repaint();
}

//==============================================================================
bool PresetBrowser::keyPressed (const juce::KeyPress& key)
{
    // A host's own shortcuts are not ours to swallow.
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
        return false;

    const auto code = key.getKeyCode();

    if (code == juce::KeyPress::upKey)
        return selectNext (-1);

    if (code == juce::KeyPress::downKey)
        return selectNext (1);

    if (code == juce::KeyPress::leftKey)
    {
        setCategoryIndex (category_ - 1);
        return true;
    }

    if (code == juce::KeyPress::rightKey)
    {
        setCategoryIndex (category_ + 1);
        return true;
    }

    if (code == juce::KeyPress::homeKey)
    {
        if (! visible_.empty())
            setSelectedIndex (visible_.front());

        topRow_ = 0;
        repaint();
        return true;
    }

    if (code == juce::KeyPress::endKey)
        return ! visible_.empty() && setSelectedIndex (visible_.back());

    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::escapeKey)
    {
        if (onClose != nullptr)
            onClose();

        return true;
    }

    if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey)
    {
        if (search_.isEmpty())
            return false;

        setSearch (search_.dropLastCharacters (1));
        return true;
    }

    // **`[` and `]` are xerum's previous/next everywhere, including here** — so they are not search text,
    // and they bubble to the editor, which owns that gesture. A preset name has no bracket in it.
    if (code == '[' || code == ']')
        return false;

    const auto character = key.getTextCharacter();

    if (character >= 0x20 && character != 0x7f)
    {
        setSearch (search_ + juce::String::charToString (static_cast<juce::juce_wchar> (character)));
        return true;
    }

    return false;
}

int PresetBrowser::rowAtPoint (const juce::Point<int> position) const
{
    if (! list_.contains (position))
        return -1;

    const auto row = topRow_ + (position.y - list_.getY()) / kRowHeight;

    return row >= 0 && row < visibleCount() ? row : -1;
}

void PresetBrowser::mouseDown (const juce::MouseEvent& event)
{
    for (std::size_t index = 0; index < categoryPills_.size(); ++index)
        if (categoryPills_[index].contains (event.getPosition()))
        {
            setCategoryIndex (static_cast<int> (index) - 1);
            return;
        }

    const auto row = rowAtPoint (event.getPosition());

    if (row >= 0)
        setSelectedIndex (visible_[static_cast<std::size_t> (row)]);
}

void PresetBrowser::mouseMove (const juce::MouseEvent& event)
{
    const auto row = rowAtPoint (event.getPosition());

    if (row == hoveredRow_)
        return;

    hoveredRow_ = row;
    repaint();
}

void PresetBrowser::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (visibleCount() <= rowsVisible_)
        return;

    topRow_ -= static_cast<int> (std::lround (wheel.deltaY * 6.0));

    const auto maximum = juce::jmax (0, visibleCount() - rowsVisible_);
    topRow_ = juce::jlimit (0, maximum, topRow_);
    repaint();
}

juce::Rectangle<int> PresetBrowser::categoryBounds (const int index) const
{
    const auto pill = index + 1;

    return pill >= 0 && pill < static_cast<int> (categoryPills_.size())
               ? categoryPills_[static_cast<std::size_t> (pill)]
               : juce::Rectangle<int>();
}

juce::Rectangle<int> PresetBrowser::rowBounds (const int row) const
{
    if (row < 0 || row >= visibleCount())
        return {};

    return { list_.getX(), list_.getY() + (row - topRow_) * kRowHeight, list_.getWidth(), kRowHeight };
}

juce::String PresetBrowser::footerDescription() const
{
    return selected_ >= 0 && selected_ < catalog_.size()
               ? juce::String (catalog_.presets[static_cast<std::size_t> (selected_)].description)
               : juce::String();
}

//==============================================================================
void PresetBrowser::resized()
{
    auto area = getLocalBounds().reduced (kMargin);

    heading_ = area.removeFromTop (kHeadingHeight);
    area.removeFromTop (kRowGap);
    searchBox_ = area.removeFromTop (kFieldHeight);
    area.removeFromTop (kRowGap);

    // The category row: `All` then the manifest's own categories, in the manifest's order — the first
    // cut, and the reason a hundred presets are not a hundred rows.
    categoryPills_.clear();
    categoryRow_ = area.removeFromTop (kPillHeight);

    {
        auto row = categoryRow_;
        const auto font = look::bodyFont (look::type::label, true);

        std::vector<juce::String> labels { "All" };

        for (const auto& category : catalog_.categories)
            labels.emplace_back (category);

        for (const auto& label : labels)
        {
            const auto width = pillWidth (label, font);
            categoryPills_.push_back (row.removeFromLeft (width));
            row.removeFromLeft (kPillGap);
        }
    }

    area.removeFromTop (kRowGap);

    auto footerArea = area.removeFromBottom (kFooterHeight);
    area.removeFromBottom (kRowGap);
    footer_ = footerArea;

    list_ = area;
    clampScroll();
}

void PresetBrowser::paint (juce::Graphics& graphics)
{
    // The panel: the design's `surface` with its `border`, over the page it covers.
    const auto surface = getLocalBounds().toFloat().reduced (0.5f);

    graphics.setColour (look::surface);
    graphics.fillRoundedRectangle (surface, static_cast<float> (look::radiusPanel));
    graphics.setColour (look::border);
    graphics.drawRoundedRectangle (surface, static_cast<float> (look::radiusPanel), 1.0f);

    // The heading: the composition's own section-heading line, and the bank's own counts as the design's
    // tag — the manifest's numbers, so the panel cannot promise a bank it does not have.
    {
        graphics.setColour (look::fgMuted);
        graphics.setFont (look::bodyFont (look::type::section, true).withExtraKerningFactor (0.18f));
        graphics.drawText ("FACTORY", heading_, juce::Justification::centredLeft, false);

        const auto font = look::monoFont (look::type::tag, true);
        const auto label = juce::String (catalog_.size()) + " presets \xc2\xb7 "
                           + juce::String (catalog_.categories.size()) + " categories";
        const auto width = static_cast<int> (juce::GlyphArrangement::getStringWidth (font, label)) + 14;
        const auto tag = heading_.removeFromRight (width).withSizeKeepingCentre (width, 14).toFloat();

        graphics.setColour (look::accentBg);
        graphics.fillRoundedRectangle (tag, 7.0f);
        graphics.setColour (look::accent);
        graphics.setFont (font);
        graphics.drawText (label, tag.toNearestInt(), juce::Justification::centred, true);
    }

    // The search field: the design's own field, and the caret is a token rather than a system one.
    {
        const auto box = searchBox_.toFloat().reduced (0.5f);

        graphics.setColour (look::bg);
        graphics.fillRoundedRectangle (box, 3.0f);
        graphics.setColour (look::border);
        graphics.drawRoundedRectangle (box, 3.0f, 1.0f);

        auto content = searchBox_.reduced (7, 0);
        const auto font = look::monoFont (9.0f);

        if (search_.isEmpty())
        {
            graphics.setColour (look::fgMuted);
            graphics.setFont (font);
            graphics.drawText ("type to search by name", content, juce::Justification::centredLeft, true);
        }
        else
        {
            const auto matches = juce::String (visibleCount())
                                 + (visibleCount() == 1 ? " match" : " matches");
            const auto font2 = look::monoFont (look::type::tag, true);
            const auto width = static_cast<int> (juce::GlyphArrangement::getStringWidth (font2, matches)) + 4;
            auto counts = content.removeFromRight (width);

            graphics.setColour (visibleCount() == 0 ? look::red : look::fgMuted);
            graphics.setFont (font2);
            graphics.drawText (visibleCount() == 0 ? juce::String ("no match") : matches, counts,
                               juce::Justification::centredRight, true);

            graphics.setColour (look::fg);
            graphics.setFont (font);
            graphics.drawText (search_, content, juce::Justification::centredLeft, true);

            const auto width_ = juce::GlyphArrangement::getStringWidth (font, search_);
            graphics.setColour (look::accent);
            graphics.fillRect (juce::Rectangle<float> (content.getX() + width_ + 1.0f,
                                                       static_cast<float> (content.getY() + 3),
                                                       1.0f, static_cast<float> (content.getHeight() - 6)));
        }
    }

    // The categories, as the strip's own pills.
    for (std::size_t index = 0; index < categoryPills_.size(); ++index)
    {
        const auto selected = static_cast<int> (index) - 1 == category_;
        const auto pill = categoryPills_[index].toFloat().withSizeKeepingCentre (
            static_cast<float> (categoryPills_[index].getWidth()), static_cast<float> (kPillHeight));
        const auto radius = pill.getHeight() * 0.5f;

        if (selected)
        {
            graphics.setColour (look::accentBg);
            graphics.fillRoundedRectangle (pill, radius);
        }

        juce::String label = index == 0 ? juce::String ("All")
                                        : juce::String (catalog_.categories[index - 1]);

        // The count on the pill is the bank's own, per category: what the manifest says is behind it.
        if (index > 0 && catalog_.ok)
        {
            auto count = 0;

            for (const auto& preset : catalog_.presets)
                if (preset.category == catalog_.categories[index - 1])
                    ++count;

            label += " " + juce::String (count);
        }

        graphics.setColour (selected ? look::accent : look::fgDim);
        graphics.setFont (look::bodyFont (look::type::label, true));
        graphics.drawText (label, pill.toNearestInt().reduced (kPillPadding, 0),
                           juce::Justification::centred, true);
    }

    // The list.
    if (catalog_.isEmpty())
    {
        graphics.setColour (look::fgMuted);
        graphics.setFont (look::monoFont (9.0f));
        graphics.drawFittedText (juce::String ("no factory bank: ") + catalog_.error, list_.reduced (7, 0),
                                 juce::Justification::centredLeft, 2);
    }
    else if (visible_.empty())
    {
        // A query that matches nothing shows nothing — and says so — rather than everything.
        graphics.setColour (look::fgMuted);
        graphics.setFont (look::monoFont (9.0f));
        graphics.drawText ("nothing in the bank matches \"" + search_ + "\"", list_.reduced (7, 0),
                           juce::Justification::centredLeft, true);
    }

    const auto nameFont = look::monoFont (9.5f);
    const auto tagFont = look::monoFont (look::type::tag, true);

    for (int row = topRow_; row < visibleCount() && row < topRow_ + rowsVisible_ + 1; ++row)
    {
        const auto bounds = rowBounds (row);

        if (bounds.getBottom() <= list_.getY() || bounds.getY() >= list_.getBottom())
            continue;

        const auto& preset = catalog_.presets[static_cast<std::size_t> (visible_[static_cast<std::size_t> (row)])];
        const auto isSelected = visible_[static_cast<std::size_t> (row)] == selected_;
        const auto isHovered = row == hoveredRow_;
        auto line = bounds;

        if (isSelected)
        {
            graphics.setColour (look::accentBg);
            graphics.fillRect (line);
        }
        else if (isHovered)
        {
            graphics.setColour (look::surface2);
            graphics.fillRect (line);
        }

        auto text = line.reduced (7, 0);

        // With no category selected the category is on the row: the grouping is visible rather than
        // implied by the order.
        if (category_ < 0)
        {
            const auto label = juce::String (preset.category).toUpperCase();
            const auto width = static_cast<int> (juce::GlyphArrangement::getStringWidth (tagFont, label)) + 8;

            graphics.setColour (isSelected ? look::accentDim : look::fgMuted);
            graphics.setFont (tagFont);
            graphics.drawText (label, text.removeFromRight (width), juce::Justification::centredRight, true);
        }

        graphics.setColour (isSelected ? look::fg : look::fgDim);
        graphics.setFont (nameFont);
        graphics.drawText (juce::String (preset.name), text, juce::Justification::centredLeft, true);

        // The design's own row rule, in the table's grid colour.
        graphics.setColour (look::border);
        graphics.fillRect (line.removeFromBottom (1));
    }

    // A scroll indicator when the filter is longer than the panel — chrome, in the chrome colour.
    if (visibleCount() > rowsVisible_ && rowsVisible_ > 0)
    {
        const auto track = list_.toFloat().removeFromRight (kScrollBarWidth);
        const auto fraction = static_cast<float> (rowsVisible_) / static_cast<float> (visibleCount());
        const auto bar = track.withHeight (juce::jmax (12.0f, track.getHeight() * fraction))
                             .withY (track.getY() + track.getHeight()
                                     * (static_cast<float> (topRow_) / static_cast<float> (visibleCount())));

        graphics.setColour (look::borderHi);
        graphics.fillRoundedRectangle (bar, kScrollBarWidth * 0.5f);
    }

    // The footer: the selected preset's own description, and the sentence that keeps the two doors apart.
    {
        auto footer = footer_;

        graphics.setColour (look::fgMuted);
        graphics.setFont (look::monoFont (look::type::readoutSm));
        graphics.drawFittedText (footerDescription(), footer.removeFromTop (14),
                                 juce::Justification::centredLeft, 1);

        graphics.setFont (look::bodyFont (look::type::tag));
        graphics.drawText ("Factory bank \xe2\x80\x94 xerum's own presets. Your Serum presets arrive through "
                           "Import .SerumPreset\xe2\x80\xa6",
                           footer, juce::Justification::centredLeft, true);
    }
}

} // namespace xerum::plugin::ui

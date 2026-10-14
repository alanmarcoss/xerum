// Tests for asset resolution.
//
// The rules here are the import contract's, and each one exists because something in the wild breaks the
// obvious version: leading separators, Windows path spelling, and case.

#include <catch2/catch_test_macros.hpp>

#include "table_library.h"

#include <filesystem>

using namespace xerum::content;

namespace
{

TableLibrary library()
{
    TableLibrary lib;
    lib.addRoot (XERUM_CONTENT_FIXTURE_DIR);
    return lib;
}

} // namespace

TEST_CASE ("a table reference resolves the way a preset spells it", "[content]")
{
    const auto lib = library();

    const auto lookup = lib.loadTable ("/Basic Shapes.wav");

    REQUIRE (lookup.found());
    CHECK (lookup.resolution == Resolution::Exact);
    CHECK (lookup.table.frameCount == 1);
    CHECK (lookup.path.filename() == "Basic Shapes.wav");
}

TEST_CASE ("a reference into a subdirectory resolves", "[content]")
{
    const auto lib = library();
    const auto lookup = lib.loadTable ("Nested/Deeper.wav");

    REQUIRE (lookup.found());
    CHECK (lookup.table.frameCount == 2);
}

TEST_CASE ("a Windows-spelled reference resolves", "[content]")
{
    // Third-party packs are frequently authored on Windows. A resolver that treats the backslash as a
    // literal character finds nothing and reports a missing asset that is right there.
    const auto lib = library();
    const auto lookup = lib.loadTable ("Nested\\Deeper.wav");

    REQUIRE (lookup.found());
    CHECK (lookup.table.frameCount == 2);
}

TEST_CASE ("a reference that differs only in case resolves, and says so", "[content]")
{
    // The file must resolve — a pack that works in the reference has to work here — but a resolver that
    // reports it as an ordinary hit throws away the fact that the pack is only working by luck.
    const auto lib = library();
    const auto lookup = lib.loadTable ("/basic shapes.WAV");

    REQUIRE (lookup.found());
    CHECK (lookup.resolution == Resolution::CaseInsensitive);
}

TEST_CASE ("a missing table is reported rather than substituted", "[content]")
{
    // The rule the whole import contract rests on for assets: no silent default.
    const auto lib = library();
    const auto lookup = lib.loadTable ("/Not Installed.wav");

    CHECK_FALSE (lookup.found());
    CHECK (lookup.resolution == Resolution::Missing);
    CHECK_FALSE (lookup.error.empty());
    CHECK (lookup.table.empty());
}

TEST_CASE ("a file that is not a wavetable is reported as unreadable, not as silent", "[content]")
{
    const auto lib = library();
    const auto lookup = lib.loadTable ("NotATable.wav");

    CHECK_FALSE (lookup.found());
    CHECK (lookup.resolution == Resolution::Unreadable);
    CHECK_FALSE (lookup.error.empty());
}

TEST_CASE ("an empty reference is refused", "[content]")
{
    const auto lib = library();

    CHECK_FALSE (lib.loadTable ("").found());
    CHECK_FALSE (lib.loadTable ("/").found());
}

TEST_CASE ("roots are searched in order and only real ones are kept", "[content]")
{
    TableLibrary lib;
    lib.addRoot ("/definitely/not/a/directory");
    lib.addRoot (XERUM_CONTENT_FIXTURE_DIR);
    lib.addRoot (XERUM_CONTENT_FIXTURE_DIR);      // a duplicate

    CHECK (lib.roots().size() == 1);
    CHECK (lib.loadTable ("Basic Shapes.wav").found());
}

TEST_CASE ("a table needing unimplemented interpolation is flagged, not silently downgraded", "[content]")
{
    // 74 of the 288 factory tables ask for spectral morphing, which the oscillator cannot do yet. A
    // loader that returns them as ordinary hits lets the engine crossfade instead and never mention it.
    const auto lib = library();

    const auto linear = lib.loadTable ("Basic Shapes.wav");
    REQUIRE (linear.found());
    CHECK_FALSE (linear.needsUnimplementedInterpolation);

    const auto spectral = lib.loadTable ("Requires Spectral.wav");
    REQUIRE (spectral.found());
    CHECK (spectral.needsUnimplementedInterpolation);
}

TEST_CASE ("a factory table resolves by the reference a preset uses", "[content][corpus]")
{
    const std::filesystem::path root = "/Library/Audio/Presets/Xfer Records/Serum 2 Presets";

    if (! std::filesystem::exists (root))
        SKIP ("the reference's content tree is not installed here (docs/test-strategy.md)");

    TableLibrary lib;
    lib.addRoot (root);

    // The reference KY - Entropy actually carries, and the fallback shape.
    const auto basic = lib.loadTable ("/Analog/Basic Shapes.wav");
    REQUIRE (basic.found());
    CHECK (basic.resolution == Resolution::Exact);
    CHECK (basic.table.frameCount > 0);
    CHECK(basic.table.frameSize == 2048);

    const auto fallback = lib.loadTable ("S2 Tables/Default Shapes.wav");
    CHECK (fallback.found());
}

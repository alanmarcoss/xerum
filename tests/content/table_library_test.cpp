// Tests for asset resolution.
//
// The rules here are the import contract's, and each one exists because something in the wild breaks the
// obvious version: leading separators, Windows path spelling, and case.

#include <catch2/catch_test_macros.hpp>

#include "table_library.h"

#include <sys/stat.h>

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

TEST_CASE ("a root that denies reads is reported, and the search goes on", "[content]")
{
    // Ticket `56`. `stat` on anything inside a directory with its permissions taken away answers EACCES,
    // and the throwing `exists()` turned that into a `filesystem_error` that reached the plugin's
    // constructor and aborted the host that instantiated it. It is a value here: a path that refused, and
    // a search that carries on to the roots behind it.
    const auto refused = std::filesystem::temp_directory_path() / "xerum-denied-root-test";

    std::filesystem::remove_all (refused);
    std::filesystem::create_directories (refused / "Tables");
    std::filesystem::copy_file (std::filesystem::path (XERUM_CONTENT_FIXTURE_DIR) / "Tables" / "Basic Shapes.wav",
                               refused / "Tables" / "Basic Shapes.wav");
    REQUIRE (::chmod (refused.c_str(), 0) == 0);

    TableLibrary lib;
    lib.addRoot (refused);
    lib.addRoot (XERUM_CONTENT_FIXTURE_DIR);   // the root behind it, which can answer

    const auto lookup = lib.loadTable ("/Basic Shapes.wav");   // must not throw, must not abort

    ::chmod (refused.c_str(), 0755);

    const auto problems = lib.problems();

    std::filesystem::remove_all (refused);

    // The root behind the refused one answered, so a refusal costs only what was in the refused root.
    REQUIRE (lookup.found());
    CHECK (lookup.path.string().rfind (XERUM_CONTENT_FIXTURE_DIR, 0) == 0);

    // And the refusal is on the record, with the path that refused and why.
    REQUIRE_FALSE (problems.empty());
    CHECK (problems.front().path.string().rfind (refused.string(), 0) == 0);
    CHECK (problems.front().reason.find ("could not be read") != std::string::npos);
}

TEST_CASE ("an asset resolves to its bytes under the subdirectory the reference's own layout uses",
           "[content]")
{
    // Ticket `64`: an impulse response is an audio file the convolver decodes, not a wavetable, so what the
    // content layer supplies is the file's *bytes* — and the walk that finds it is the tables' walk, with
    // `Impulses/` where `Tables/` was. The fixture root carries one, measured over nothing: the same
    // rules (leading separator, both separators, case) apply because they are the same code.
    const auto lib = library();

    const auto exact = lib.loadAsset ("/Impulse.wav", kImpulseDirectory);

    REQUIRE (exact.found());
    CHECK (exact.resolution == Resolution::Exact);
    CHECK (exact.path.filename() == "Impulse.wav");
    CHECK_FALSE (exact.bytes.empty());

    // The bytes are the file's, byte for byte — nothing here parses or rewrites them.
    const auto onDisk = std::filesystem::path (XERUM_CONTENT_FIXTURE_DIR) / "Impulses" / "Impulse.wav";
    CHECK (exact.bytes.size() == std::filesystem::file_size (onDisk));

    // Both separators, and a leading one, exactly as a table reference is spelled.
    CHECK (lib.loadAsset ("\\Impulse.wav", kImpulseDirectory).found());
    CHECK (lib.loadAsset ("Sub\\Nested.wav", kImpulseDirectory).found());
    CHECK (lib.loadAsset ("sub/NESTED.wav", kImpulseDirectory).resolution == Resolution::CaseInsensitive);
}

TEST_CASE ("a missing asset and a refused one are told apart, and neither is a table's answer", "[content]")
{
    // The distinction is ticket `56`'s and it is the same one here: nothing there is an ordinary miss,
    // while a path that is present and will not be read is a refusal. What a missing impulse must *not* do
    // is come back as some other asset — the search under `Impulses/` falls back to the reference as the
    // preset spelled it, which is a last chance rather than a substitution.
    const auto lib = library();

    const auto missing = lib.loadAsset ("NotThere.wav", kImpulseDirectory);
    CHECK_FALSE (missing.found());
    CHECK (missing.resolution == Resolution::Missing);
    CHECK (missing.error.find ("not found in") != std::string::npos);
    CHECK (missing.bytes.empty());

    // A table's own name resolves as a *table*, not as an impulse: the two searches are separate, so a
    // convolution cannot silently pick up a wavetable.
    CHECK_FALSE (lib.loadAsset ("Basic Shapes.wav", kImpulseDirectory).found());
    CHECK (lib.loadTable ("Basic Shapes.wav").found());

    // An empty reference, refused the same way a table's is.
    CHECK_FALSE (lib.loadAsset ("", kImpulseDirectory).found());
}

TEST_CASE ("an asset's own root that denies reads is a refusal carrying its path", "[content]")
{
    const auto refused = std::filesystem::temp_directory_path() / "xerum-denied-impulse-root-test";

    std::filesystem::remove_all (refused);
    std::filesystem::create_directories (refused / "Impulses");
    std::filesystem::copy_file (std::filesystem::path (XERUM_CONTENT_FIXTURE_DIR) / "Impulses" / "Impulse.wav",
                               refused / "Impulses" / "Impulse.wav");
    REQUIRE (::chmod (refused.c_str(), 0) == 0);

    TableLibrary lib;
    lib.addRoot (refused);

    const auto lookup = lib.loadAsset ("Impulse.wav", kImpulseDirectory);   // must not throw

    ::chmod (refused.c_str(), 0755);

    const auto problems = lib.problems();
    std::filesystem::remove_all (refused);

    CHECK_FALSE (lookup.found());

    REQUIRE_FALSE (problems.empty());
    CHECK (problems.front().path.string().rfind (refused.string(), 0) == 0);
    CHECK (problems.front().reason.find ("could not be read") != std::string::npos);
}

TEST_CASE ("a table declaring spectral morphing loads like any other", "[content]")
{
    // This case used to be an exception: 74 of the 288 factory tables carry `clm ` mode 2, 3 or 4, the
    // loader flagged them, and the oscillator crossfaded a flagged table underneath the flag. Ticket 37
    // measured the reference and found modes 1..4 all render as a plain linear crossfade (mode 0 alone
    // is discrete), so there is nothing left to flag — the mode is recorded on the table, the table
    // loads, and the crossfade is the faithful reading of it. See tickets/37 and src/dsp/spectral_morph.h.
    const auto lib = library();

    const auto linear = lib.loadTable ("Basic Shapes.wav");
    REQUIRE (linear.found());
    CHECK (linear.table.interpolation == xerum::dsp::FrameInterpolation::Linear);

    const auto spectral = lib.loadTable ("Requires Spectral.wav");
    REQUIRE (spectral.found());
    CHECK (spectral.table.interpolation == xerum::dsp::FrameInterpolation::SpectralMorph);
    CHECK (spectral.table.frameCount > 1);
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

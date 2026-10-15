// Ticket `56`: construction with a content root that will not answer.
//
// Both suites passed on a binary that could not start, because neither of them ever built the processor
// against a content root the filesystem holds up. This one does, and the root it uses needs no privileges
// and no cooperation from the machine:
//
//   **a FIFO standing in for a table.** `mkfifo` makes a path that `exists()`, that is not a directory, and
//   that `open()` for reading **blocks on until a writer appears** — which is the shape of the defect
//   exactly: a filesystem call that does not fail and does not return. On this machine it is also the
//   ticket's real shape: `tccd` held the construction thread inside `open$NOCANCEL` for two hours while a
//   Desktop-folder consent prompt went unanswered.
//
// What is asserted:
//
//   1. `XerumAudioProcessor` **constructs**, on a root like that, within its budget — against the code as
//      it was, the builder never returns and the first assertion fails;
//   2. the report says the root did not answer, so the silence is explained where the import contract puts
//      such facts, rather than left to be discovered;
//   3. nothing is invented for the slots: an unresolved slot stays unresolved;
//   4. destroying the processor does not wait for the stuck read either, so the same root cannot hang a
//      host on the way out;
//   5. when the filesystem *does* answer, the resolution lands and the report gains the entries that say
//      what was found — an unreadable asset, and the silence that follows from it;
//   6. a root that is perfectly readable still resolves before the caller returns, so none of this costs
//      the ordinary case anything.

#include "TestSupport.h"

#include "PresetState.h"
#include "XerumAudioProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace xerum::plugin;
using namespace xerum::plugin::tests;

namespace
{

using Clock = std::chrono::steady_clock;

double seconds (const Clock::time_point from, const Clock::time_point to)
{
    return std::chrono::duration<double> (to - from).count();
}

/// A content root of the test's own making, held as `XERUM_CONTENT_ROOT` for as long as it lives — the
/// same way `third_party_test` pins one, and for the same reason: which root supplies a table must be the
/// test's decision, not the machine's.
///
/// The root holds exactly one thing, `Tables/xerum/Sine.wav`, and the three kinds are the three ways a
/// content root can fail to give it up:
///
///   * `Readable` — xerum's own table, copied in. The ordinary case;
///   * `Stalled` — a **FIFO**. `open()` for reading blocks until a writer appears, so the read does not
///     fail and does not return: the shape of a consent prompt nobody has answered, a volume that has gone
///     to sleep, a drive that is not spinning;
///   * `Denied` — a real root with its permissions taken away. `stat` on anything inside it answers
///     EACCES, which is the case that used to throw a `filesystem_error` through the constructor.
struct StubRoot
{
    enum class Kind { Readable, Stalled, Denied };

    juce::File root;
    Kind kind;

    explicit StubRoot (const Kind wanted)
        : kind (wanted)
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getNonexistentChildFile ("xerum-content-gate", {}, false);

        const auto tables = root.getChildFile ("Tables").getChildFile ("xerum");
        REQUIRE (tables.createDirectory().wasOk());

        const auto table = tables.getChildFile ("Sine.wav");

        if (kind == Kind::Stalled)
        {
            REQUIRE (::mkfifo (table.getFullPathName().toRawUTF8(), 0644) == 0);
        }
        else
        {
            const juce::File shipped (juce::String (XERUM_CONTENT_DIR) + "/Tables/xerum/Sine.wav");
            REQUIRE (shipped.existsAsFile());
            REQUIRE (shipped.copyFileTo (table));
        }

        if (kind == Kind::Denied)
            REQUIRE (::chmod (root.getFullPathName().toRawUTF8(), 0) == 0);

        setenv ("XERUM_CONTENT_ROOT", root.getFullPathName().toRawUTF8(), 1);
    }

    ~StubRoot()
    {
        unsetenv ("XERUM_CONTENT_ROOT");

        // The mode has to come back before the directory can be walked, let alone removed.
        if (kind == Kind::Denied)
            ::chmod (root.getFullPathName().toRawUTF8(), 0755);

        root.deleteRecursively();
    }

    juce::String tablePath() const
    {
        return root.getChildFile ("Tables").getChildFile ("xerum").getChildFile ("Sine.wav").getFullPathName();
    }
};

/// Where the builder thread leaves what it made. On the heap, and referenced by the builder itself, so a
/// builder that never finishes — which is the case this test exists for — cannot write into a test object
/// that has already gone.
struct Handover
{
    std::atomic<bool> done { false };
    std::shared_ptr<XerumAudioProcessor> processor;   ///< published before `done` is set
};

/// Build a processor on another thread and report whether it came back inside `budgetSeconds`.
///
/// **On another thread, deliberately.** The defect is a construction that never returns, and a test that
/// called the constructor inline would hang the suite instead of failing it — which is how this defect
/// stayed green through 356 tests.
struct Built
{
    std::shared_ptr<Handover> handover = std::make_shared<Handover>();
    bool returned { false };
    double elapsed { 0.0 };
    std::thread builder;

    explicit Built (const double budgetSeconds)
    {
        const auto started = Clock::now();
        const auto handover = this->handover;

        builder = std::thread ([handover]
                               {
                                   auto built = std::make_shared<XerumAudioProcessor>();

                                   handover->processor = std::move (built);
                                   handover->done.store (true, std::memory_order_release);
                               });

        while (seconds (started, Clock::now()) < budgetSeconds
               && ! handover->done.load (std::memory_order_acquire))
            std::this_thread::sleep_for (std::chrono::milliseconds (5));

        elapsed = seconds (started, Clock::now());
        returned = handover->done.load (std::memory_order_acquire);

        // A builder that did not come back is left detached rather than joined: joining it is the hang
        // this test exists to catch.
        if (returned)
            builder.join();
        else
            builder.detach();
    }

    ~Built()
    {
        if (builder.joinable())
            builder.detach();
    }

    XerumAudioProcessor& processor() const { return *handover->processor; }

    void reset() { handover->processor.reset(); }
};

} // namespace

TEST_CASE ("construction does not wait for a content root that will not answer", "[plugin][content]")
{
    const StubRoot stub (StubRoot::Kind::Stalled);
    Built built (4.0);

    INFO ("XERUM_CONTENT_ROOT " << stub.root.getFullPathName().toStdString());
    INFO ("the stub's table is a FIFO at " << stub.tablePath().toStdString());

    REQUIRE (built.returned);

    // The budget is 250 ms. Anything approaching the wait a blocked `open()` imposes — which is unbounded —
    // means the construction path is still doing filesystem work.
    CHECK (built.elapsed < 2.0);

    // The root is *offered*, and it is the first one, so it is the one this test is about.
    REQUIRE_FALSE (built.processor().contentRoots().empty());
    CHECK (built.processor().contentRoots().front() == stub.root.getFullPathName());

    // 2. Reported, not hidden. What the interface's report surface shows is what `reportText` builds.
    const auto report = built.processor().reportText();
    INFO (report.toStdString());
    CHECK (report.contains ("did not answer within"));
    CHECK (report.contains ("contentRoots"));

    // 3. Nothing is invented for a slot whose table could not be read.
    CHECK (built.processor().oscillatorTable (0) == nullptr);

    // 4. Going away is bounded too — the read is still outstanding at this moment, and the destructor must
    //    not join the thread it is on.
    const auto before = Clock::now();
    built.reset();
    CHECK (seconds (before, Clock::now()) < 2.0);
}

TEST_CASE ("a content root that answers late still reports what it found", "[plugin][content]")
{
    StubRoot stub (StubRoot::Kind::Stalled);
    Built built (4.0);

    REQUIRE (built.returned);
    REQUIRE (built.processor().reportText().contains ("did not answer within"));

    // Open the FIFO for writing: this is the filesystem *answering* — the read the worker is sitting in
    // returns, and the rest of the resolution runs. It finds a path that exists, that is not sizeable and
    // that yields nothing, so it must say exactly that.
    const juce::File table (stub.tablePath());
    const auto writer = ::open (table.getFullPathName().toRawUTF8(), O_WRONLY);

    REQUIRE (writer >= 0);

    // The message thread's own drain, driven directly rather than through the timer. It is what the timer
    // calls twenty times a second.
    bool landed = false;

    for (int attempt = 0; attempt < 400 && ! landed; ++attempt)
    {
        built.processor().flushContent();
        landed = built.processor().reportText().contains ("could not read");
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    const auto report = built.processor().reportText();
    INFO (report.toStdString());

    CHECK (landed);

    // The report names the file that would not be read and says what follows from it, rather than leaving
    // the silence to be discovered.
    CHECK (report.contains (stub.tablePath().toStdString()));
    CHECK (report.contains ("renders silence"));

    // Still nothing to play — and now the report says why, rather than leaving it as an absence.
    CHECK (built.processor().oscillatorTable (0) == nullptr);

    ::close (writer);
}

TEST_CASE ("a read that outlives its processor finishes into memory it owns", "[plugin][content]")
{
    StubRoot stub (StubRoot::Kind::Stalled);
    juce::String tablePath;

    {
        Built built (4.0);
        REQUIRE (built.returned);

        // The read is still outstanding at this moment — nothing has opened the FIFO for writing — and the
        // host unloads the plugin: this is the destructor a host calls on a plugin it is finished with.
        CHECK (built.processor().oscillatorTable (0) == nullptr);

        tablePath = stub.tablePath();
        built.reset();
    }

    // Now let the read return, **after** the object that asked for it has gone. What the work owns is what
    // it captured: the content library, the plan, the roots and the block it fills, all by `shared_ptr`, and
    // the worker's own reference to the queue state. Nothing in it can name the processor, which is why it
    // may finish: if any of that had been freed with the owner, this is where it would be caught.
    const juce::File table (tablePath);
    const auto writer = ::open (table.getFullPathName().toRawUTF8(), O_WRONLY);

    REQUIRE (writer >= 0);

    // Long enough for the abandoned worker to run the rest of the resolution and exit.
    std::this_thread::sleep_for (std::chrono::milliseconds (500));

    ::close (writer);

    SUCCEED ("the resolution ran to its end with no owner");
}

TEST_CASE ("a readable content root still resolves before the caller returns", "[plugin][content]")
{
    const StubRoot stub (StubRoot::Kind::Readable);
    Built built (4.0);

    REQUIRE (built.returned);

    // The init patch names no table, so slot 0 plays xerum's own — resolved under the stub root, inside the
    // bounded wait, exactly as it was before any of this existed.
    CHECK (built.processor().oscillatorTable (0) != nullptr);
    CHECK_FALSE (built.processor().reportText().contains ("did not answer within"));
}

TEST_CASE ("a content root that denies reads is reported, and the search goes on", "[plugin][content]")
{
    const StubRoot stub (StubRoot::Kind::Denied);
    Built built (4.0);

    INFO ("XERUM_CONTENT_ROOT " << stub.root.getFullPathName().toStdString());
    INFO ("the stub's root is mode 000, so every path inside it answers EACCES");

    // Against the code as it was this never returns: `exists()` in its throwing form raised a
    // `filesystem_error` out of `TableLibrary::loadTable`, through the constructor, into `std::terminate` —
    // the crash report is `__throw_filesystem_error` under `loadTable`, and the process aborts.
    REQUIRE (built.returned);

    const auto report = built.processor().reportText();
    INFO (report.toStdString());

    // Reported, with the path that refused and why.
    CHECK (report.contains ("could not be read"));
    CHECK (report.contains (stub.tablePath().toStdString()));

    // And **skipped rather than fatal**: the roots after the refused one are still asked, so the init
    // patch still plays. A refused root loses only what was in it.
    CHECK (built.processor().oscillatorTable (0) != nullptr);
}

TEST_CASE ("an absent table is a substitution, not a refusal and not a throw", "[plugin][content]")
{
    const StubRoot stub (StubRoot::Kind::Readable);
    Built built (4.0);

    REQUIRE (built.returned);

    // The import contract's ordinary case (ticket 14): a preset naming an asset xerum does not have.
    // Resolving it stats paths that are not there, and that must be a report entry and a substitution.
    //
    // The name is one nothing can supply, rather than the reference instrument's own: on a machine with the
    // reference installed, `/Analog/Basic Shapes.wav` *does* resolve — against that instrument's tree — and
    // a test that assumed otherwise would be testing this machine rather than the code.
    xerum::model::PresetDocument document;
    document.parameters["Oscillator0/WTOsc0/relativePathToWT"] = text ("/definitely/not/installed.wav");

    built.processor().applyDocument (document, {}, 0);

    const auto report = built.processor().reportText();
    INFO (report.toStdString());

    CHECK (report.contains ("/definitely/not/installed.wav"));
    CHECK (report.contains ("not found in"));
    CHECK (report.contains ("substituted xerum's own"));
    CHECK (built.processor().oscillatorTable (0) != nullptr);

    // And **absence is not a refusal.** A path that is not there says nothing beyond the slot's own entry;
    // only a path that *is* there and will not be read is reported as such. That distinction is the whole
    // of ticket 56's reporting, and getting it wrong turns every ordinary miss into an alarming entry.
    CHECK_FALSE (report.contains ("could not be read"));
    CHECK_FALSE (report.contains ("could not be listed"));
}

// xerum::content — work that must not run on the thread that asked for it.
//
// Ticket `56`. The processor's construction path used to walk the content roots where it stood, and on
// macOS that walk can stop for as long as nobody answers a prompt. The sample that opened the ticket is
// a host's main thread inside `open$NOCANCEL`, reached from
// `std::filesystem::directory_iterator` ← `TableLibrary::loadTable` ← `XerumAudioProcessor::resolveContent`
// ← `XerumAudioProcessor::applyDocument` ← `XerumAudioProcessor::XerumAudioProcessor()`, unchanged for
// two hours; `tccd` shows the `kTCCServiceSystemPolicyDesktopFolder` request that held it.
//
// The rule this class exists to hold: **no content-root filesystem call runs on the thread that asked
// for it.** A caller hands over a piece of work, may wait for it *briefly*, and then carries on. The
// work finishes when the filesystem lets it, and the caller collects the result whenever it lands.
//
// Three consequences shape the implementation, and each is deliberate:
//
//   * the worker runs **one piece of work at a time** and the newest submission wins. A submission that
//     arrived while another was running is simply never run — so a superseded request can never report a
//     result, and a caller must key its result to the generation `submit()` gave it;
//   * the worker may sit in a syscall that never returns, so the destructor **does not join a busy
//     worker**. It signals it to stop, and leaves it to finish into state the worker owns a reference to;
//   * because of those two, a piece of work must not touch anything the caller may destroy: everything it
//     needs is what it captured, and what it produces is handed back through something the caller also
//     holds a reference to.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace xerum::content
{

class OffThreadWork
{
public:
    OffThreadWork();
    ~OffThreadWork();

    OffThreadWork (const OffThreadWork&) = delete;
    OffThreadWork& operator= (const OffThreadWork&) = delete;

    /// Hand `work` to the worker, superseding anything in flight. Never blocks: the work starts on the
    /// worker's thread.
    ///
    /// `finished`, when given, is set with a release store once the work has run — **only** for a
    /// submission that actually ran. That is how a caller tells "my work has not finished yet" from "my
    /// work was superseded and never will": it keeps both the flag and the payload the work fills, and the
    /// worker only sets the flag. There is deliberately no handle that identifies a submission any other
    /// way, because the payload is the only thing that can say what the work found.
    void submit (std::function<void()> work, std::shared_ptr<std::atomic<bool>> finished = {});

    /// Wait up to `timeoutMs` for the newest submission to have run to completion. Returns immediately
    /// when it already has — which is the ordinary case, a walk of a handful of local files.
    ///
    /// False means "not yet", never "failed": the work is still running, and the caller decides what to
    /// say and what to publish in the meantime.
    bool waitForNewest (int timeoutMs);

private:
    /// Shared with the worker, which outlives a destructor that cannot join it. Without this the worker's
    /// own reference would not be enough to keep the state alive after its owner has gone.
    struct State
    {
        std::mutex mutex;
        std::condition_variable ready;

        std::function<void()> requested;

        /// Set (release) by the worker when `requested` has run, and only then. Held by the caller too, so
        /// a submission the worker superseded leaves it alone and the caller learns nothing happened.
        std::shared_ptr<std::atomic<bool>> requestedFinished;

        std::uint64_t requestedGeneration { 0 };
        std::uint64_t completedGeneration { 0 };

        bool stopping { false };

        /// True while the worker is waiting for something to do, false while it is doing it. Read under
        /// the mutex by the destructor, which is how it tells "safe to join" from "it is inside a
        /// syscall" without ever joining the second.
        bool idle { false };
    };

    static void run (State& state) noexcept;

    std::shared_ptr<State> state_;
    std::thread worker_;
};

} // namespace xerum::content

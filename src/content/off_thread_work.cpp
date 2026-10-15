#include "off_thread_work.h"

#include <chrono>

namespace xerum::content
{

OffThreadWork::OffThreadWork()
    : state_ (std::make_shared<State>())
{
    // The thread holds the state by value, not through `this`: after the destructor has gone the worker
    // may still be inside a syscall, and it must be able to finish without reading freed memory.
    const auto state = state_;
    worker_ = std::thread ([state] { run (*state); });
}

OffThreadWork::~OffThreadWork()
{
    {
        const std::lock_guard lock (state_->mutex);
        state_->stopping = true;

        // The captures of a submission the worker has not started are the caller's objects, and so is the
        // flag it expects to be set. Let both go now rather than whenever the worker next runs — which it
        // will not.
        state_->requested = {};
        state_->requestedFinished.reset();
    }

    state_->ready.notify_all();

    if (! worker_.joinable())
        return;

    bool idle = false;

    {
        const std::lock_guard lock (state_->mutex);
        idle = state_->idle;
    }

    // Idle means the worker is in `wait` and will see `stopping` at once. Anything else means it is
    // inside a piece of work that has not returned, and joining it would hang this caller for as long as
    // that takes — which is the whole point of the class. Such a worker is left detached: it finishes,
    // finds `stopping`, and exits. A detached thread that has already finished is harmless, so the
    // answer being one step out of date costs nothing.
    if (idle)
        worker_.join();
    else
        worker_.detach();
}

void OffThreadWork::submit (std::function<void()> work, std::shared_ptr<std::atomic<bool>> finished)
{
    const std::lock_guard lock (state_->mutex);

    state_->requested = std::move (work);
    state_->requestedFinished = std::move (finished);
    ++state_->requestedGeneration;

    state_->ready.notify_all();
}

bool OffThreadWork::waitForNewest (const int timeoutMs)
{
    std::unique_lock lock (state_->mutex);

    const auto newest = state_->requestedGeneration;

    return state_->ready.wait_for (lock, std::chrono::milliseconds (timeoutMs),
                                   [this, newest] { return state_->completedGeneration >= newest; });
}

void OffThreadWork::run (State& state) noexcept
{
    for (;;)
    {
        std::function<void()> work;
        std::shared_ptr<std::atomic<bool>> finished;
        std::uint64_t generation = 0;

        {
            std::unique_lock lock (state.mutex);
            state.idle = true;

            state.ready.wait (lock, [&state]
                              { return state.stopping || state.requestedGeneration > state.completedGeneration; });

            state.idle = false;

            if (state.stopping)
                return;

            // The newest submission, whichever that is by now: an older one that this worker never got to
            // is dropped, and its caller learns that from the flag it holds never opening.
            work = state.requested;
            finished = state.requestedFinished;
            generation = state.requestedGeneration;
        }

        // **No lock is held here, deliberately.** This is the call that may never return, and holding the
        // mutex across it would stop `waitForNewest` from ever timing out — turning the bound this class
        // exists to provide into a hang. `noexcept`, because a piece of work that throws would otherwise
        // take the process with it on a thread nobody can catch it from.
        try
        {
            work();
        }
        catch (...)
        {
        }

        // Set however the work ended: a payload half-filled and a caller told nothing would be worse than
        // either outcome on its own.
        if (finished != nullptr)
            finished->store (true, std::memory_order_release);

        {
            const std::lock_guard lock (state.mutex);
            state.completedGeneration = generation;
        }

        state.ready.notify_all();
    }
}

} // namespace xerum::content

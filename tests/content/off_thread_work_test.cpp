// Ticket `56`: the mechanism that keeps a content-root filesystem call off the caller's thread.
//
// What is checked here is the contract the processor depends on, with the filesystem replaced by
// something that simply does not answer:
//
//   * `waitForNewest` gives up on time and says so, leaving the caller free;
//   * the destructor does not wait for a worker that is inside a call which has not returned;
//   * a submission that arrives while another is running supersedes it, and only the newest one produces
//     a result.

#include "off_thread_work.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace
{

using Clock = std::chrono::steady_clock;

/// A gate that work can block in until the test opens it — the simplest stand-in for a directory that
/// answers nothing.
struct Gate
{
    std::mutex mutex;
    std::condition_variable open;
    bool opened { false };

    void wait()
    {
        std::unique_lock lock (mutex);
        open.wait (lock, [this] { return opened; });
    }

    void release()
    {
        {
            const std::lock_guard lock (mutex);
            opened = true;
        }

        open.notify_all();
    }
};

} // namespace

TEST_CASE ("a piece of work that does not answer does not hold its caller", "[content]")
{
    xerum::content::OffThreadWork work;
    Gate gate;

    work.submit ([&gate] { gate.wait(); });

    const auto started = Clock::now();
    const bool done = work.waitForNewest (50);
    const auto elapsed = std::chrono::duration<double> (Clock::now() - started).count();

    CHECK_FALSE (done);
    // A bound, not an exact figure: it returned because it gave up, not because the work finished.
    CHECK (elapsed < 2.0);

    gate.release();
    CHECK (work.waitForNewest (5000));
}

TEST_CASE ("a superseded submission never reports, and the newest one does", "[content]")
{
    xerum::content::OffThreadWork work;
    Gate gate;

    std::atomic<bool> started { false };
    std::atomic<int> first { 0 };
    std::atomic<int> second { 0 };

    // The first submission is *already running* when the second arrives — waited for rather than assumed,
    // because a worker that had not yet picked it up would simply drop it, which is the other case the
    // next test covers. So the worker must finish the first and then run the newest.
    work.submit ([&gate, &started, &first]
                 {
                     started = true;
                     gate.wait();
                     first = 1;
                 });

    while (! started.load())
        std::this_thread::yield();

    work.submit ([&second] { second = 1; });

    gate.release();

    REQUIRE (work.waitForNewest (5000));
    CHECK (first == 1);
    CHECK (second == 1);
}

TEST_CASE ("a submission the worker never ran never opens its flag", "[content]")
{
    xerum::content::OffThreadWork work;
    Gate gate;

    std::atomic<bool> blocking { false };
    auto neverRan = std::make_shared<std::atomic<bool>> (false);
    auto newest = std::make_shared<std::atomic<bool>> (false);

    // Occupy the worker so that the next two submissions are still queued when the second arrives. The
    // first of them is then superseded without ever running, and the flag its caller is holding must stay
    // shut — a flag that opened for work that never ran would report an empty result as a finished one.
    work.submit ([&gate, &blocking]
                 {
                     blocking = true;
                     gate.wait();
                 });

    while (! blocking.load())
        std::this_thread::yield();

    work.submit ([] {}, neverRan);
    work.submit ([] {}, newest);

    gate.release();
    REQUIRE (work.waitForNewest (5000));

    CHECK_FALSE (neverRan->load());
    CHECK (newest->load());
}

TEST_CASE ("destroying the owner does not wait for work that is stuck", "[content]")
{
    Gate gate;
    std::atomic<bool> entered { false };
    std::atomic<bool> left { false };

    std::unique_ptr<xerum::content::OffThreadWork> work
        = std::make_unique<xerum::content::OffThreadWork>();

    work->submit ([&gate, &entered, &left]
                  {
                      entered = true;
                      gate.wait();
                      left = true;
                  });

    // Wait for the work to have started, so the destructor is asked to join a *busy* worker.
    while (! entered.load())
        std::this_thread::yield();

    const auto started = Clock::now();
    work.reset();
    const auto elapsed = std::chrono::duration<double> (Clock::now() - started).count();

    // The work is still in the gate. If the destructor joined it, this line would never be reached — the
    // test would hang rather than fail, which is the shape of the defect the class exists to remove.
    CHECK (elapsed < 2.0);

    // Let the worker out, and wait until it is out, because it is detached and the gate lives here.
    gate.release();

    while (! left.load())
        std::this_thread::yield();
}

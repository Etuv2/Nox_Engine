#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <source_location>

// A resumable unit of work with its own stack (a Windows fiber). The task runs on the thread that resumes it, so
// GL calls made inside it use the current context. Yield points inside the task hand control back to the resumer
// once the budget of the current Resume is spent. Outside Windows the task runs to completion on its first Resume
// and yield points do nothing.
class SliceTask {
public:
    explicit SliceTask(std::function<void()> body);
    ~SliceTask();  // unwinds an unfinished task first
    SliceTask(const SliceTask&) = delete;
    SliceTask& operator=(const SliceTask&) = delete;

    // Runs the task until it finishes or spends budgetMs (budgetMs < 0: no limit). Returns true when finished.
    // An exception thrown by the body is rethrown here.
    bool Resume(double budgetMs);
    bool Finished() const;
    // Unwinds an unfinished task: its next yield point throws and the body's destructors run.
    void Cancel();

    // Inside a task: returns to the resumer when the current budget is spent (or the task was cancelled).
    // Outside a task: does nothing.
    // NOX_SLICE_GAP_MS=<ms> (diagnostic): logs each stretch of work longer than that between two yield points, with
    // the file:line of both ends. The stretch is measured only while the task runs (not across suspended frames).
    static void YieldIfOverBudget(std::source_location at = std::source_location::current());
    static bool InTask();

    // Waits for a result computed on a worker thread. Inside a task the wait yields to the frame between polls, so
    // the loading screen keeps animating while the worker runs; outside a task it blocks.
    template <typename T>
    static T AwaitYielding(std::future<T> future)
    {
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            if (!InTask()) {
                future.wait();
                break;
            }
            future.wait_for(std::chrono::milliseconds(1));
            YieldIfOverBudget();
        }
        return future.get();
    }

    // Implementation detail, public only for the fiber entry point in SliceTask.cpp.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

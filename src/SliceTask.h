#pragma once
#include <functional>
#include <memory>

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
    static void YieldIfOverBudget();
    static bool InTask();

    // Implementation detail, public only for the fiber entry point in SliceTask.cpp.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

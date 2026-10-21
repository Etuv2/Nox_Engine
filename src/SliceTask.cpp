#include "SliceTask.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// Thrown at a yield point of a cancelled task so its stack unwinds; caught by the task entry.
struct SliceCancelled {};

// Budget of the Resume that is running on this thread.
thread_local bool t_unlimited = true;
thread_local Clock::time_point t_deadline{};

}  // namespace

struct SliceTask::Impl {
    std::function<void()> body;
    bool finished = false;
    bool cancel = false;
    std::exception_ptr error;
#ifdef _WIN32
    void* fiber = nullptr;
    void* resumer = nullptr;
    Impl* previousCurrent = nullptr;
#endif
};

namespace {
thread_local SliceTask::Impl* t_current = nullptr;
}

#ifdef _WIN32

static void __stdcall SliceFiberEntry(void* param)
{
    auto* impl = static_cast<SliceTask::Impl*>(param);
    try {
        impl->body();
    }
    catch (const SliceCancelled&) {
    }
    catch (...) {
        impl->error = std::current_exception();
    }
    impl->finished = true;
    SwitchToFiber(impl->resumer);  // a finished fiber is never resumed
}

SliceTask::SliceTask(std::function<void()> body) : m_impl(std::make_unique<Impl>())
{
    m_impl->body = std::move(body);
}

SliceTask::~SliceTask()
{
    if (m_impl && !m_impl->finished && m_impl->fiber) {
        Cancel();
        while (!m_impl->finished) {
            Resume(-1.0);
        }
    }
    if (m_impl && m_impl->fiber) {
        DeleteFiber(m_impl->fiber);
    }
}

bool SliceTask::Resume(double budgetMs)
{
    Impl* impl = m_impl.get();
    if (impl->finished) return true;
    if (!IsThreadAFiber()) {
        ConvertThreadToFiber(nullptr);
    }
    if (!impl->fiber) {
        impl->fiber = CreateFiber(8u * 1024u * 1024u, SliceFiberEntry, impl);
        if (!impl->fiber) {
            impl->error = std::make_exception_ptr(std::runtime_error("CreateFiber failed for a scene build task"));
            impl->finished = true;
            return true;
        }
    }

    // Run with this task's budget; restore the caller's budget afterwards (tasks may resume other tasks).
    const bool savedUnlimited = t_unlimited;
    const Clock::time_point savedDeadline = t_deadline;
    t_unlimited = budgetMs < 0.0;
    t_deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double, std::milli>(t_unlimited ? 0.0 : budgetMs));

    impl->resumer = GetCurrentFiber();
    impl->previousCurrent = t_current;
    t_current = impl;
    SwitchToFiber(impl->fiber);
    t_current = impl->previousCurrent;

    t_unlimited = savedUnlimited;
    t_deadline = savedDeadline;

    if (impl->error) {
        std::exception_ptr error = std::exchange(impl->error, nullptr);
        std::rethrow_exception(error);
    }
    return impl->finished;
}

bool SliceTask::Finished() const
{
    return m_impl->finished;
}

void SliceTask::Cancel()
{
    m_impl->cancel = true;
}

void SliceTask::YieldIfOverBudget()
{
    Impl* impl = t_current;
    if (!impl || !impl->fiber) return;
    if (!impl->cancel && !t_unlimited && Clock::now() < t_deadline) return;
    SwitchToFiber(impl->resumer);
    // Resumed with a fresh budget (or cancelled).
    if (impl->cancel) throw SliceCancelled{};
}

bool SliceTask::InTask()
{
    return t_current != nullptr && t_current->fiber != nullptr;
}

#else  // Not Windows: no fibers. The body runs to completion on the first Resume.

SliceTask::SliceTask(std::function<void()> body) : m_impl(std::make_unique<Impl>())
{
    m_impl->body = std::move(body);
}

SliceTask::~SliceTask() = default;

bool SliceTask::Resume(double)
{
    Impl* impl = m_impl.get();
    if (impl->finished) return true;
    Impl* previous = t_current;
    t_current = impl;
    try {
        impl->body();
    }
    catch (...) {
        t_current = previous;
        impl->finished = true;
        throw;
    }
    t_current = previous;
    impl->finished = true;
    return true;
}

bool SliceTask::Finished() const
{
    return m_impl->finished;
}

void SliceTask::Cancel()
{
    m_impl->cancel = true;
}

void SliceTask::YieldIfOverBudget()
{
}

bool SliceTask::InTask()
{
    return false;
}

#endif

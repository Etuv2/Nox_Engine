#include "editor/ProcessRunner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

constexpr const char* kProgressTag = "[NOX-PROGRESS]";

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

// Keeps valid UTF-8 sequences, replaces invalid bytes with '?', drops ANSI CSI escape sequences
// (cmake and ninja colour codes) and turns other control characters into spaces.
std::string SanitizeOutput(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1B) {
            // ESC [ ... final byte (0x40-0x7E)
            if (i + 1 < n && in[i + 1] == '[') {
                size_t j = i + 2;
                while (j < n && !(static_cast<unsigned char>(in[j]) >= 0x40 && static_cast<unsigned char>(in[j]) <= 0x7E)) ++j;
                i = (j < n) ? j + 1 : n;
            } else {
                i += 1;
            }
            continue;
        }
        if (c < 0x80) {
            out.push_back(c < 0x20 && c != '\t' ? ' ' : static_cast<char>(c));
            ++i;
            continue;
        }
        size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        bool valid = len != 0 && i + len <= n;
        for (size_t k = 1; valid && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            valid = cc >= 0x80 && cc <= 0xBF;
        }
        if (valid) {
            out.append(in, i, len);
            i += len;
        } else {
            out.push_back('?');
            i += 1;
        }
    }
    return out;
}

bool IsErrorLine(const std::string& line) {
    if (line.find(kProgressTag) != std::string::npos) return false;
    if (line.find("FAILED") != std::string::npos) return true;
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.find("error") == std::string::npos) return false;
    // MSBuild/CMake summaries such as "0 Error(s)" or "0 errors" are not failures.
    if (lower.find("0 error") != std::string::npos) return false;
    return true;
}

std::string LastWin32Error(DWORD code) {
    return "error code " + std::to_string(static_cast<unsigned long>(code));
}

}  // namespace

ProcessRunner::~ProcessRunner() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
}

bool ProcessRunner::Start(const std::string& title, const std::string& commandLine, const std::string& workingDir) {
    if (m_running.load()) return false;
    if (m_worker.joinable()) m_worker.join();  // previous worker has finished (m_running is false)

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.clear();
        m_lastError.clear();
        m_firstErrorLine.clear();
        m_progress = -1.0f;
        m_title = title;
        m_commandLine = commandLine;
        m_hasEndTime = false;
        m_startTime = Clock::now();
    }
    m_exitCode = -1;
    m_cancelled = false;
    m_started = true;

    auto fail = [this](const std::string& message) {
        AppendLine(message);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = message;
        m_endTime = Clock::now();
        m_hasEndTime = true;
        return false;
    };

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return fail("CreatePipe failed: " + LastWin32Error(GetLastError()));
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);  // only the child writes into the pipe

    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nulIn == INVALID_HANDLE_VALUE) nulIn = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nulIn;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    std::wstring wideCmd = Utf8ToWide(commandLine);
    std::vector<wchar_t> cmdBuffer(wideCmd.begin(), wideCmd.end());
    cmdBuffer.push_back(L'\0');
    const std::wstring wideCwd = Utf8ToWide(workingDir);
    const wchar_t* cwdPtr = wideCwd.empty() ? nullptr : wideCwd.c_str();

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                                            CREATE_UNICODE_ENVIRONMENT,
                                        nullptr, cwdPtr, &si, &pi);
    const DWORD createError = created ? 0 : GetLastError();

    // The parent must drop its write end, otherwise the reader never sees EOF.
    CloseHandle(writePipe);
    if (nulIn) CloseHandle(nulIn);

    if (!created) {
        CloseHandle(readPipe);
        return fail("CreateProcess failed (" + LastWin32Error(createError) + "): " + commandLine);
    }

    // Job Object: kill-on-close plus TerminateJobObject for Cancel(). Falls back to TerminateProcess
    // if the job cannot be created.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
            !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job = job;
        m_process = pi.hProcess;
    }
    {
        // Echo header for the log. Pushed directly so the command text is not scanned for errors.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back("> " + SanitizeOutput(commandLine));
    }
    m_running = true;
    m_worker = std::thread(&ProcessRunner::WorkerMain, this, static_cast<void*>(readPipe),
                           static_cast<void*>(pi.hProcess), static_cast<void*>(job));
    return true;
}

void ProcessRunner::WorkerMain(void* readPipe, void* process, void* job) {
    HANDLE hRead = static_cast<HANDLE>(readPipe);
    HANDLE hProcess = static_cast<HANDLE>(process);

    std::string pending;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(hRead, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        pending.append(buffer, got);
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            AppendLine(std::move(line));
        }
    }
    if (!pending.empty()) {
        if (pending.back() == '\r') pending.pop_back();
        AppendLine(std::move(pending));
    }
    CloseHandle(hRead);

    WaitForSingleObject(hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(hProcess, &code);
    m_exitCode = static_cast<int>(code);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job = nullptr;
        m_process = nullptr;
        m_endTime = Clock::now();
        m_hasEndTime = true;
        if (m_cancelled.load()) {
            m_lastError = "Cancelled by user.";
        } else if (code != 0) {
            m_lastError = m_firstErrorLine.empty()
                              ? "Process exited with code " + std::to_string(static_cast<long>(code)) + "."
                              : m_firstErrorLine;
        } else {
            m_lastError.clear();
        }
    }

    CloseHandle(hProcess);
    if (job) CloseHandle(static_cast<HANDLE>(job));
    m_running = false;  // last: IsRunning() == false means output is fully drained
}

void ProcessRunner::AppendLine(std::string line) {
    std::string clean = SanitizeOutput(line);
    std::lock_guard<std::mutex> lock(m_mutex);

    const size_t tag = clean.find(kProgressTag);
    if (tag != std::string::npos) {
        const char* start = clean.c_str() + tag + std::strlen(kProgressTag);
        char* end = nullptr;
        const float value = std::strtof(start, &end);
        if (end != start) m_progress = std::clamp(value, 0.0f, 1.0f);
    }
    if (m_firstErrorLine.empty() && IsErrorLine(clean)) m_firstErrorLine = clean;

    m_lines.push_back(std::move(clean));
}

bool ProcessRunner::IsRunning() const {
    return m_running.load();
}

int ProcessRunner::ExitCode() const {
    return m_exitCode.load();
}

bool ProcessRunner::Succeeded() const {
    return !m_running.load() && m_started.load() && m_exitCode.load() == 0 && !m_cancelled.load();
}

float ProcessRunner::ElapsedSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started.load()) return 0.0f;
    const Clock::time_point end = m_hasEndTime ? m_endTime : Clock::now();
    return std::chrono::duration<float>(end - m_startTime).count();
}

std::string ProcessRunner::Title() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_title;
}

size_t ProcessRunner::LineCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lines.size();
}

std::vector<std::string> ProcessRunner::LinesSince(size_t index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_lines.size()) return {};
    return std::vector<std::string>(m_lines.begin() + static_cast<std::ptrdiff_t>(index), m_lines.end());
}

void ProcessRunner::Cancel() {
    if (!m_running.load()) return;
    m_cancelled = true;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_job) {
        TerminateJobObject(static_cast<HANDLE>(m_job), 1);
    } else if (m_process) {
        TerminateProcess(static_cast<HANDLE>(m_process), 1);
    }
}

float ProcessRunner::Progress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

std::string ProcessRunner::LastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

bool ProcessRunner::LaunchDetached(const std::string& commandLine, const std::string& workingDir) {
    std::wstring wideCmd = Utf8ToWide(commandLine);
    std::vector<wchar_t> cmdBuffer(wideCmd.begin(), wideCmd.end());
    cmdBuffer.push_back(L'\0');
    const std::wstring wideCwd = Utf8ToWide(workingDir);
    const wchar_t* cwdPtr = wideCwd.empty() ? nullptr : wideCwd.c_str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE,
                                   CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr, cwdPtr, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

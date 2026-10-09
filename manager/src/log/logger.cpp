#include "logger.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <windows.h>

namespace eam {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    Shutdown();
}

static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static constexpr uint64_t kRotateAtInit = 2ull * 1024 * 1024;   // start a fresh log if the old one is bigger
static constexpr uint64_t kRotateLive   = 8ull * 1024 * 1024;   // and roll over mid-session at this size
static constexpr uint64_t kThrottleWindowMs = 5000;
static constexpr uint32_t kThrottleBurst = 20;

void Logger::OpenFile() {
    m_file.open(m_path, std::ios::out | std::ios::app);
    std::error_code ec;
    const auto size = std::filesystem::file_size(m_path, ec);
    m_fileBytes = ec ? 0 : (uint64_t)size;
}

// Keeps one previous log ("<name>.old") so a long session cannot grow the file without bound.
void Logger::RotateIfNeeded() {
    if (m_fileBytes < kRotateLive) return;
    m_file.close();
    std::error_code ec;
    std::filesystem::path oldPath = m_path;
    oldPath += L".old";
    std::filesystem::rename(m_path, oldPath, ec);   // (replaces an older .old; the old one is not deleted first, so a failed rename loses nothing)
    if (ec) std::filesystem::resize_file(m_path, 0, ec);   // held open by something else: keep this one from growing for ever
    OpenFile();
}

void Logger::Init(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;

    m_path = logFilePath;
    std::error_code ec;
    const auto size = std::filesystem::file_size(m_path, ec);
    if (!ec && size > kRotateAtInit) {
        std::filesystem::path oldPath = m_path;
        oldPath += L".old";
        std::filesystem::rename(m_path, oldPath, ec);   // (replaces an older .old)
    }
    OpenFile();
    m_initialized = true;
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.close();
    }
    m_initialized = false;
}

void Logger::Log(LogLevel level, const char* source, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(level, source, fmt, args);
    va_end(args);
}

void Logger::LogV(LogLevel level, const char* source, const char* fmt, va_list args) {
    if (level < MinLevel()) return;   // cheap exit before any formatting

    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    const uint64_t now = NowMs();
    const char* src = source ? source : "";

    std::lock_guard<std::mutex> lock(m_mutex);

    // Cap repeats of chatty non-error lines.
    if (level <= LogLevel::Info) {
        std::string key = src;
        key += ':';
        for (const char* c = buffer; *c && (c - buffer) < 48 && !(*c >= '0' && *c <= '9'); ++c) key += *c;
        if (m_throttle.size() > 512) m_throttle.clear();
        Throttle& t = m_throttle[key];
        if (now - t.windowStartMs > kThrottleWindowMs) {
            if (t.suppressed) {
                char note[160];
                snprintf(note, sizeof(note), "(%u similar messages suppressed: %.60s)", t.suppressed, key.c_str());
                m_entries.push_back({ LogLevel::Info, "Logger", note, now });
                if (m_file.is_open()) { m_file << "[INFO] [Logger] " << note << "\n"; m_fileBytes += strlen(note) + 20; }
            }
            t.windowStartMs = now;
            t.count = 0;
            t.suppressed = 0;
        }
        if (++t.count > kThrottleBurst) { t.suppressed++; return; }
    }

    m_entries.push_back({ level, src, buffer, now });
    if (m_entries.size() > MAX_ENTRIES) {
        m_entries.pop_front();
    }
    m_revision.fetch_add(1, std::memory_order_relaxed);

    if (m_file.is_open()) {
        SYSTEMTIME lt; GetLocalTime(&lt);
        char stamp[16]; snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
        m_file << "[" << stamp << "] [" << LevelToString(level) << "] [" << src << "] " << buffer << "\n";
        m_fileBytes += strlen(buffer) + strlen(src) + 27;
        // Flushing every line puts a disk round trip on whatever thread logged, including the
        // render thread. Warnings and errors flush at once (and take everything before them along);
        // the rest at most every 100 ms.
        if (level >= LogLevel::Warn || now - m_lastFlushMs >= 100) {
            m_file.flush();
            m_lastFlushMs = now;
        }
        RotateIfNeeded();
    }

    if (IsDebuggerPresent()) {
        char dbgBuf[2200];
        snprintf(dbgBuf, sizeof(dbgBuf), "[LSAddonManager][%s][%s] %s\n", LevelToString(level), src, buffer);
        OutputDebugStringA(dbgBuf);
    }
}

std::vector<LogEntry> Logger::GetEntries(LogLevel minLevel) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<LogEntry> result;
    result.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        if (e.level >= minLevel) {
            result.push_back(e);
        }
    }
    return result;
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) m_file.flush();
}

void Logger::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_revision.fetch_add(1, std::memory_order_relaxed);
}

const char* Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------------------
// Crash logging: abort() / std::terminate / invalid CRT parameter all end the host process
// with 0xc0000409 and no trace. Write module+offset for each frame so it can be resolved
// against the linker map that ships with the Release build.
// ---------------------------------------------------------------------------------------

// "module.dll+0xOFFSET" for an address, written into `out`.
static void DescribeAddress(void* addr, char* out, size_t outSize) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)addr, &mod);
    if (mod) {
        wchar_t wide[MAX_PATH];
        if (GetModuleFileNameW(mod, wide, MAX_PATH))
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, MAX_PATH, nullptr, nullptr);
        if (const char* slash = strrchr(name, '\\')) memmove(name, slash + 1, strlen(slash));
    }
    snprintf(out, outSize, "%s+0x%llX", name, (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
}

static void LogBacktrace(const char* why) {
    LOG_ERROR("Crash", "%s", why);
    void* frames[48] = {};
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        char where[MAX_PATH + 32];
        DescribeAddress(frames[i], where, sizeof(where));
        LOG_ERROR("Crash", "  #%u %s", (unsigned)i, where);
    }
}

static void __cdecl OnAbort(int) { LogBacktrace("abort() called"); }

static void OnTerminate() {
    if (std::exception_ptr ep = std::current_exception()) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { LOG_ERROR("Crash", "uncaught C++ exception: %s", e.what()); }
        catch (...) { LOG_ERROR("Crash", "uncaught non-std exception"); }
    }
    LogBacktrace("std::terminate");
    std::abort();
}

static std::atomic<uint32_t> g_invalidParamTotal{ 0 };
uint32_t InvalidParameterCount() { return g_invalidParamTotal.load(std::memory_order_relaxed); }

static void __cdecl OnInvalidParameter(const wchar_t* expr, const wchar_t* func, const wchar_t* file,
                                       unsigned line, uintptr_t) {
    g_invalidParamTotal.fetch_add(1, std::memory_order_relaxed);
    // A bad argument to a CRT `_s` function (usually inside an addon) is not worth killing
    // Lossless Scaling over: log it with a backtrace and return, so the CRT call just fails
    // with EINVAL. Only the first few are logged to keep a per-frame offender from flooding.
    static std::atomic<int> s_count{ 0 };
    const int n = ++s_count;
    if (n <= 5) {
        LOG_ERROR("Crash", "invalid CRT parameter (ignored, #%d): %ls in %ls (%ls:%u)", n,
                  expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
        LogBacktrace("invalid parameter");
    } else if (n == 6) {
        LOG_ERROR("Crash", "further invalid CRT parameter reports suppressed");
    }
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

// An access violation or similar in Lossless Scaling or an addon otherwise leaves no trace at all.
// Log where it happened, then hand over to whatever filter was installed before ours.
static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> s_reported{ false };
    if (ep && ep->ExceptionRecord && !s_reported.exchange(true)) {
        char where[MAX_PATH + 32];
        DescribeAddress(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        LogBacktrace("unhandled exception");
        LOG_ERROR("Crash", "code 0x%08lX at %s", ep->ExceptionRecord->ExceptionCode, where);
        Logger::Instance().Flush();
    }
    return (g_prevFilter && ep) ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogging() {
    g_prevFilter = SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
}

} // namespace eam

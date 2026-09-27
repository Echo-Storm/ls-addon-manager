// The recorder: the last few seconds of the frames the addon receives, kept in memory (losslessly compressed) and saved as a .lsrec file on
// request (the panel's button, Ctrl+Shift+F1). For bug reports that can be replayed offline (nr_hosttest replay=), tests and tuning on real
// footage, and comparisons. It records the frames as they come in, before anything of ours is done to them.
//
// Nothing ever waits: each frame is copied on the GPU into one of a few staging textures; a later call maps it without waiting (the GPU has
// long finished by then), a worker thread compresses it straight from the mapped memory, and a later call unmaps it. A frame that finds every
// staging texture busy is left out (and counted). Off, it costs nothing and holds no memory.
#pragma once
#include "addon/lsrec.h"
#include <d3d11.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nr {

class Recorder {
public:
    using LogFn = std::function<void(const char*)>;
    void SetLog(LogFn log) { m_log = std::move(log); }
    // on: keep recording; seconds and budgetMb: how much is kept (the older frames go first). Off frees everything.
    void Configure(bool on, float seconds, uint32_t budgetMb);
    // A frame the addon received (on the render thread that owns ctx). source: lsrec::Source.
    void Offer(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content = 0);   // content: lsrec::Content
    // The device is going away (under the lock the frame path holds): the staging textures are released once the workers are done with them.
    void Forget();
    void Shutdown();   // the end: the threads stop (Forget first)
    // Writes what is held to folder\<game>_<date>.lsrec on a thread of its own. False when there is nothing to save or a save is running.
    bool Save(const std::wstring& folder, const std::string& game);

    struct Status { bool on = false; double seconds = 0; uint32_t frames = 0; uint64_t bytes = 0; uint64_t missed = 0; uint32_t w = 0, h = 0; bool saving = false; };
    Status GetStatus() const;
    std::string LastResult(bool& ok) const;

private:
    struct Slot {
        ID3D11Texture2D* staging = nullptr;
        enum State { Free, Copied, Working } state = Free;
        std::atomic<bool> done{ false };   // the worker is finished with the mapped memory
        uint64_t index = 0; int64_t qpc = 0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
    };
    struct Job { Slot* slot; uint32_t generation, w, h, bpp; };
    void Log(const char* fmt, ...);
    void Worker();
    void Collect(bool waitForWorkers);   // on m_ctx
    void DropStaging();
    void Keep(std::shared_ptr<lsrec::Frame> frame);
    void StartWorkers();
    void StopWorkers();

    LogFn m_log;
    static constexpr int kSlots = 6;
    Slot m_slots[kSlots];
    ID3D11Device* m_dev = nullptr; ID3D11DeviceContext* m_ctx = nullptr;   // the device the staging textures are on, and its context (maps them)
    uint32_t m_w = 0, m_h = 0, m_bpp = 0, m_source = 0; DXGI_FORMAT m_fmt = DXGI_FORMAT_UNKNOWN, m_viewFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t m_content = 0;   // lsrec::Content of the frames kept
    uint64_t m_offered = 0;

    std::atomic<bool> m_on{ false };
    std::atomic<float> m_seconds{ 5.0f };
    std::atomic<uint64_t> m_budget{ 0 };
    std::atomic<uint64_t> m_missed{ 0 };
    int64_t m_freq = 0;

    // the workers
    std::vector<std::thread> m_workers;
    std::mutex m_jobMutex; std::condition_variable m_jobCv; std::deque<Job> m_jobs; bool m_stop = false;

    // what is kept, oldest first
    mutable std::mutex m_keepMutex;
    std::deque<std::shared_ptr<lsrec::Frame>> m_kept;
    uint64_t m_keptBytes = 0;
    std::atomic<uint32_t> m_keptGeneration{ 0 };   // a new size or format starts the kept frames again

    // saving
    std::thread m_saver;
    std::atomic<bool> m_saving{ false };
    mutable std::mutex m_resultMutex; std::string m_result; bool m_resultOk = false;
};

} // namespace nr

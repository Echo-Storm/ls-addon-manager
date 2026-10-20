// The recorder service: the one recorder of the manager (the Recording feature), and what the offline test host uses in its place.
//
// It keeps the last few seconds of the frames Lossless Scaling captured (found by watching its compute passes, as the addons' own tap does) and of
// frames that addons offer, losslessly compressed in memory (addon/recorder.h), and saves them as a .lsrec file on request (a hotkey, a button),
// with a note beside the file (<file>.json: the game, the picture, the addons and their settings) and a beep, so a press is known to have worked.
// It needs no addon to be switched on: with frame generation on, the captured frames are found from the passes. What only an addon can see (the
// presented frames before its picture goes on them, NIS's input, EchoFG's frames) the addon offers through IHost::RecorderOffer.
//
// Off, it costs one atomic read per dispatch. No ImGui and no config here: the manager's feature (recorder_feature.cpp) has those.
#pragma once
#include "addon/recorder.h"
#include "addon/frame_tap.h"
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace eam {
namespace rec {

struct Settings {
    bool on = false;
    bool shown = false;        // record what is shown: the addons offer the presented frames, and the captured-frame tap is off
    float seconds = 5.0f;      // how much is kept
    int budgetMb = 3072;       // and at most this much memory
    std::wstring folder;       // where files go; empty: Videos\Lossless Scaling
    bool hotkeyOn = true;
    int key = 0x70;            // VK_F1, with Ctrl and Shift
    float afterSeconds = 0.0f; // after the key: keep recording this long, then save (0: save at once)
    bool compact = true;       // HDR frames are kept as an 8-bit SDR picture (small); off: exactly as captured (lossless half-floats: huge)
    bool beep = true;          // a short sound when a file is saved (or could not be)
    bool note = true;          // the note beside the file
    int saveAfter = 0;         // tests only: save by itself once this many frames are held
};

enum Flags : uint32_t { kFlagOn = 1, kFlagShown = 2 };   // IHost::RecorderFlags

class Service {
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    using LogFn = std::function<void(const char*)>;
    void SetLog(LogFn log);
    // What the manager knows beside the pictures, as the inside of a JSON object ("\"addons\": [...]"), put in the note; called on the polling thread.
    void SetInfoProvider(std::function<std::string()> provider);
    void Configure(const Settings& s);          // any thread
    Settings Current() const;
    uint32_t Flags() const;                     // kFlagOn | kFlagShown: what the addons check before they offer frames

    // Every compute pass of Lossless Scaling, before it runs (its render thread; ctx is the pass's context). Finds the captured frame.
    void OnDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z);
    // A frame an addon has (any thread that owns ctx). The captured frames are the service's own: an offer of source captured is ignored.
    void Offer(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropW, uint32_t cropH);

    // The hotkey (Ctrl+Shift+key): a save when it is on, a ten-second clip when it is off. Call it a few dozen times a second from a thread of
    // your own; it also ends a clip, runs the wait after the key, and writes the note and beeps after a save.
    void Poll();
    bool Request();                             // what the key does (the panel's button too)
    bool Save();                                // now, for the game in front; false when there is nothing to save or a save is running
    int SaveNamed(const std::string& name, bool clear);   // IHost::RecorderSave: 1 saving, 0 nothing to save, 2 a save is running
    void SetSavedCallback(std::function<void(const std::wstring&)> fn) { m_saved = std::move(fn); }   // after a file is written (the polling thread)
    void Clip();                                // the recorder is off: record the next ten seconds, save them by themselves, and be off again
    void TestSound();                           // the tick and the saved tone, for the Features tab's button
    bool Clipping() const { return m_clipUntil.load() != 0; }
    // A fault in the frame tap or the recorder (caught, so Lossless Scaling goes on) left locks held that nothing can release: the service is off for good and must be leaked, not destroyed.
    bool Faulted() const { return m_faulted.load(); }
    bool Pending() const { return m_saveAt.load() != 0; }   // the wait after the key is running

    nr::Recorder::Status GetStatus() const { return m_recorder.GetStatus(); }
    std::string LastResult(bool& ok) const { return m_recorder.LastResult(ok); }
    std::string Summary() const;                // one line for the status bar and the Features tab
    void Shutdown();                            // the threads stop and the memory goes

private:
    struct TapState { std::unique_ptr<FrameTap> tap; std::atomic<uint64_t> firstMs{ 0 }, lastMs{ 0 }; };   // shared: a dispatch uses its tap outside the map's lock
    void Log(const char* fmt, ...);
    void ApplyToRecorder();
    void NoteForeground();
    std::string GameName();
    std::wstring Folder() const;
    void OnDispatchImpl(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z);
    void OfferLocked(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropW, uint32_t cropH);
    void AfterSave();
    void WriteNote(const nr::Recorder::SaveInfo& info);

    mutable std::mutex m_settingsMutex;
    Settings m_settings;
    std::atomic<bool> m_on{ false }, m_shown{ false };
    std::atomic<int> m_saveAfter{ 0 };
    std::atomic<uint64_t> m_clipUntil{ 0 }, m_saveAt{ 0 }, m_lastForegroundMs{ 0 };
    LogFn m_log;
    std::function<std::string()> m_info;
    std::function<void(const std::wstring&)> m_saved;

    std::mutex m_recMutex;                      // the recorder is one thread's at a time
    nr::Recorder m_recorder;
    std::atomic<bool> m_savedForTest{ false };
    uint32_t m_seenSaves = 0, m_seenFails = 0;

    std::mutex m_tapMutex;
    std::map<ID3D11DeviceContext*, std::shared_ptr<TapState>> m_taps;   // one tap per context Lossless Scaling dispatches on (a few); the lock covers the map, not the taps
    std::atomic<ID3D11DeviceContext*> m_owner{ nullptr };   // the context whose captured frames are recorded; another one takes over when it goes quiet
    std::atomic<uint64_t> m_ownerMs{ 0 }, m_lastTapOfferMs{ 0 };
    uint64_t m_lastCleanMs = 0;   // (under the map's lock)

    mutable std::mutex m_gameMutex;
    std::string m_game;                         // the program in front before Lossless Scaling's window took the focus
    bool m_wasDown = false;
    std::atomic<bool> m_faulted{ false };
};

} // namespace rec
} // namespace eam

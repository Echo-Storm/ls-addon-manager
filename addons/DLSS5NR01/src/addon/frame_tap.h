// FrameTap: reads Lossless Scaling's compute passes (every Dispatch, on its render thread) and decides when Neural Rendering gets a frame.
//
// LSFG 3, per real frame: a capture pass that reads the newly captured frame and builds a luma pyramid (the TAP), flow passes from coarse to fine,
// then one pass per generated frame. The TAP is found automatically by the shape of what is bound (or set by hand in the panel). Neural Rendering
// is handed the captured frame together with LSFG's optical flow, which it uses as motion vectors. By default it waits for this frame's own flow:
// the frame is held from its TAP until LSFG has issued the frame's finest flow pass and handed over on the next dispatch. With that off it is
// handed over at the TAP with the flow of the frame before (one frame late). The presents are bookkept too: which of them show a generated frame
// and where each sits between two real frames.
#pragma once
#include <d3d11.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The shape of one bound view: size and format of the texture behind it.
struct ViewShape { uint32_t w = 0, h = 0; uint32_t fmt = 0; bool valid = false; bool tex2d = false; };

// What a pass looks like: its thread groups and the shapes of the eight SRVs and four UAVs bound to it. Kept in the settings as text
// ("x,y,z|w:h:fmt|..." with twelve view fields), so the text form must not change.
struct DispatchSig {
    uint32_t x = 0, y = 0, z = 0;
    ViewShape srv[8]; ViewShape uav[4];
    std::string Serialize() const;
    bool Parse(const std::string& s);
    bool operator==(const DispatchSig& o) const;
    uint64_t Hash() const;
    bool Empty() const { return x == 0 && y == 0 && z == 0; }
};

// One row of the table of passes seen (for the panel and the automatic role choice).
struct DispatchEntry {
    DispatchSig sig; uint64_t key = 0;
    uint32_t count = 0;
    uint64_t lastSeen = 0;    // the dispatch counter when it last ran
    int roleAuto = 0;         // 1 TICK, 2 TAP, as chosen automatically
};

struct TapDecision {
    bool isTick = false, isTap = false;
    ID3D11Texture2D* frame = nullptr;   // the captured frame (AddRef'd; the caller releases)
    int frameSlot = -1;
    // LSFG's finest optical flow (AddRef'd; the caller releases). RGBA16F: xy = displacement from the current to the previous frame, zw = the
    // reverse; units = pixels of a texture twice this size. This frame's own (k-1 -> k) with fresh flow on, the frame before's otherwise.
    ID3D11Texture2D* flow = nullptr; uint32_t flowW = 0, flowH = 0;
    bool freshFlow = false;             // the flow handed over is this frame's own
};

// One of Lossless Scaling's presents, placed between real frames: with real frame k just captured, LS shows k-1, the generated frames between
// k-1 and k, and k.
struct PresentInfo {
    uint64_t tap = 0;        // taps so far = the index k of the newest real frame
    int index = 0;           // ordinal of this present since that tap
    int perFrame = 0;        // presents per real frame, learned from the last complete interval (0 = not yet)
    bool gen = false;        // a generated frame: a generated-frame pass ran since the previous present
    double target = -1;      // the frame this present shows, in real-frame units (k-1 .. k); < 0 = before any tap
};

class FrameTap {
public:
    enum Mode { Auto = 0, Manual = 1 };
    FrameTap() = default;
    ~FrameTap();
    FrameTap(const FrameTap&) = delete;
    FrameTap& operator=(const FrameTap&) = delete;

    void Reset();                                  // the device changed: let go of every texture and start learning again
    void SetRoles(const DispatchSig& tick, const DispatchSig& tap, Mode mode, int frameSlotPref /* -1 auto */);
    void GetRoles(DispatchSig& tick, DispatchSig& tap) const;
    // Wait for this frame's own flow before handing the frame over (on, the default), or hand it over at the TAP with the frame before's flow (off).
    void SetFreshFlow(bool on);

    // Every dispatch on Lossless Scaling's render thread, before it runs. Fills `d`; true when Neural Rendering should run on d.frame now.
    bool Observe(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, TapDecision& d);
    // Every present of Lossless Scaling's swap chain.
    PresentInfo NotePresent();
    // True when a generated-frame pass ran since the last call (cleared): asked at each NIS pass, it says that the picture NIS is about to scale is a generated one
    bool TakeGenerated() { std::lock_guard<std::mutex> lk(m_mu); const bool g = m_genSincePresent; m_genSincePresent = false; return g; }
    // The flow LSFG wrote most recently (AddRef'd; the caller releases).
    ID3D11Resource* NewestFlow(uint32_t& w, uint32_t& h);

    const char* PresentPattern() const { return m_pattern; }
    std::vector<DispatchEntry> Snapshot() const;   // for the panel
    void ClearTable();
    uint64_t Ticks() const { return m_ticks; }
    uint64_t Taps() const { return m_taps; }
    uint64_t Dispatches() const { return m_dispatches; }
    const char* GateName() const { return m_gateName; }
    // How the handed-over frames got their motion: this frame's flow, the frame before's, and frames dropped because LSFG ran no flow pass for
    // them before the next frame arrived.
    uint64_t FreshRuns() const { return m_freshRuns; }
    uint64_t StaleRuns() const { return m_staleRuns; }
    uint64_t DroppedWaiting() const { return m_droppedWaiting; }

    // Flow probe (diagnostic): over the next three TAPs, write the captured frame and every RGBA16F texture the finest flow pass wrote since the
    // TAP before to <dir>\flowprobe_*.bin, with a log of the pointers.
    void ArmProbe(const std::wstring& dir);
    std::string ProbeStatus() const;

private:
    struct Bound;                                   // the views bound to one pass, read once
    void Record(const DispatchSig& sig, uint64_t key);
    void ChooseTapAutomatically();
    bool OnTap(const Bound& b, TapDecision& d);     // true when the frame is handed over right away
    void OnFlowPass(const Bound& b);
    bool HandOverHeld(TapDecision& d);
    void ProbePass(ID3D11DeviceContext* ctx, const Bound& b, const TapDecision& d);
    void ProbeLog(const char* fmt, ...);
    void ProbeDump(ID3D11DeviceContext* ctx, ID3D11Resource* res, const wchar_t* tag, int tapIdx, int sub);
    void ReleaseAll();

    mutable std::mutex m_mu;

    // roles and the table of passes
    std::unordered_map<uint64_t, DispatchEntry> m_table;
    DispatchSig m_tick, m_tap; uint64_t m_tickKey = 0, m_tapKey = 0; Mode m_mode = Auto; int m_frameSlotPref = -1;
    uint32_t m_frameW = 0, m_frameH = 0;            // the captured frame's size (the largest colour texture in recent passes)
    uint64_t m_lastChoice = 0;                      // the dispatch counter at the last automatic choice
    uint64_t m_ticks = 0, m_taps = 0, m_dispatches = 0, m_lastGatedTick = ~0ull;
    const char* m_gateName = "none";

    // flow: the finest level written since the last TAP, and the one handed out at the last TAP (both AddRef'd)
    ID3D11Resource* m_flowNew = nullptr; uint32_t m_flowNewW = 0, m_flowNewH = 0; uint64_t m_flowNewArea = 0;
    ID3D11Resource* m_flowLast = nullptr; uint32_t m_flowLastW = 0, m_flowLastH = 0;
    // fresh flow: the frame held from its TAP (AddRef'd), handed over on the dispatch after this frame's finest flow pass
    bool m_freshFlow = true;
    ID3D11Texture2D* m_held = nullptr; int m_heldSlot = -1; bool m_handOverNext = false;
    uint64_t m_freshRuns = 0, m_staleRuns = 0, m_droppedWaiting = 0;
    uint32_t m_dropStreak = 0;          // frames in a row that got no flow pass: past a few, frame generation is off and nothing is waited for

    // presents
    int m_presentsSinceTap = 0; bool m_genSincePresent = false; int m_perFrame = 0; bool m_realFirst = false; char m_pattern[64] = "learning";

    // probe
    bool m_probeArmed = false; std::wstring m_probeDir; int m_probeTaps = 0; std::string m_probeStatus = "idle";
    uint64_t m_probeFlowArea = 0;
    struct ProbeTex { ID3D11Resource* res; uint64_t dispatch; };
    std::vector<ProbeTex> m_probeFlows;
};

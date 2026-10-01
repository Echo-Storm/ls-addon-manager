#include "addon/frame_tap.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>

// ------------------------------------------------------------------------------------------------------------------------------------ helpers
namespace {

constexpr uint32_t kFlowFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // LSFG's flow levels
constexpr uint64_t kStaleAfter = 3000;                            // dispatches (~20-30 real frames): a pass not seen for this long is out of date
constexpr uint64_t kRechooseEvery = 600;                          // dispatches between automatic role choices
constexpr uint32_t kGiveUpWaitingAfter = 3;                       // frames in a row without a flow pass before this frame's flow is no longer waited for
constexpr size_t kMaxTableRows = 256;

// Formats a captured frame can have (what the bridge accepts, and the HDR ones).
bool IsColour(uint32_t f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: return true;
    default: return false;
    }
}

ViewShape ShapeOf(ID3D11Resource* r) {
    ViewShape s;
    if (!r) return s;
    s.valid = true;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    ID3D11Texture2D* t = nullptr;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) && t) {
        D3D11_TEXTURE2D_DESC desc; t->GetDesc(&desc); t->Release();
        s.w = desc.Width; s.h = desc.Height; s.fmt = (uint32_t)desc.Format; s.tex2d = true;
    } else {
        s.w = 1; s.h = 1; s.fmt = 0xFFFF;   // a buffer or another dimension: marks the slot as used
    }
    return s;
}

// Holds a new reference in `slot`, letting go of the old one.
template <class T> void Hold(T*& slot, T* value) {
    if (value) value->AddRef();
    if (slot) slot->Release();
    slot = value;
}
template <class T> void Drop(T*& slot) { if (slot) { slot->Release(); slot = nullptr; } }

// The texture behind a resource, AddRef'd; null when it is not a 2D texture.
ID3D11Texture2D* AsTexture(ID3D11Resource* r) {
    ID3D11Texture2D* t = nullptr;
    return r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t))) ? t : nullptr;
}

uint64_t Area(const ViewShape& s) { return (uint64_t)s.w * s.h; }

} // namespace

// ------------------------------------------------------------------------------------------------------------------------------------ signatures
std::string DispatchSig::Serialize() const {
    std::string out = std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
    auto field = [&](const ViewShape& v) {
        out += "|" + std::to_string(v.valid ? v.w : 0) + ":" + std::to_string(v.valid ? v.h : 0) + ":" + std::to_string(v.valid ? v.fmt : 0);
    };
    for (const auto& v : srv) field(v);
    for (const auto& v : uav) field(v);
    return out;
}

bool DispatchSig::Parse(const std::string& text) {
    DispatchSig parsed;
    const char* p = text.c_str();
    int used = 0;
    if (sscanf(p, "%u,%u,%u%n", &parsed.x, &parsed.y, &parsed.z, &used) != 3) return false;
    p += used;
    for (int i = 0; i < 12; ++i) {
        unsigned w = 0, h = 0, f = 0;
        if (sscanf(p, "|%u:%u:%u%n", &w, &h, &f, &used) != 3) return false;
        p += used;
        ViewShape& v = i < 8 ? parsed.srv[i] : parsed.uav[i - 8];
        v.w = w; v.h = h; v.fmt = f; v.valid = w != 0; v.tex2d = v.valid;
    }
    *this = parsed;
    return true;
}

bool DispatchSig::operator==(const DispatchSig& o) const {
    auto same = [](const ViewShape& a, const ViewShape& b) { return a.valid == b.valid && (!a.valid || (a.w == b.w && a.h == b.h && a.fmt == b.fmt)); };
    if (x != o.x || y != o.y || z != o.z) return false;
    for (int i = 0; i < 8; ++i) if (!same(srv[i], o.srv[i])) return false;
    for (int i = 0; i < 4; ++i) if (!same(uav[i], o.uav[i])) return false;
    return true;
}

uint64_t DispatchSig::Hash() const {   // FNV-1a over the same fields operator== compares
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    auto view = [&](const ViewShape& v) { mix(v.valid); if (v.valid) { mix(v.w); mix(v.h); mix(v.fmt); } };
    mix(x); mix(y); mix(z);
    for (const auto& v : srv) view(v);
    for (const auto& v : uav) view(v);
    return h ? h : 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------ one pass
// The views bound to a pass, read once. Holds a reference to each resource for as long as the pass is being looked at.
struct FrameTap::Bound {
    DispatchSig sig;
    ID3D11Resource* srv[8] = {};
    ID3D11Resource* uav[4] = {};
    Bound(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
        sig.x = x; sig.y = y; sig.z = z;
        ID3D11ShaderResourceView* sv[8] = {};
        ID3D11UnorderedAccessView* uv[4] = {};
        ctx->CSGetShaderResources(0, 8, sv);
        ctx->CSGetUnorderedAccessViews(0, 4, uv);
        for (int i = 0; i < 8; ++i) if (sv[i]) { sv[i]->GetResource(&srv[i]); sv[i]->Release(); sig.srv[i] = ShapeOf(srv[i]); }
        for (int i = 0; i < 4; ++i) if (uv[i]) { uv[i]->GetResource(&uav[i]); uv[i]->Release(); sig.uav[i] = ShapeOf(uav[i]); }
    }
    ~Bound() { for (auto* r : srv) if (r) r->Release(); for (auto* r : uav) if (r) r->Release(); }
    Bound(const Bound&) = delete;
    Bound& operator=(const Bound&) = delete;

    // LSFG's flow: an RGBA16F level written with the next coarser level bound at S4
    bool IsFlowPass() const { return uav[0] && sig.uav[0].valid && sig.uav[0].fmt == kFlowFormat && srv[4]; }
    // A generated frame being put together: a full-size colour output fed by two full-size colour frames, or by one plus a flow field
    // (the real frame reaches the screen through a copy or a single-input pass)
    bool IsGeneratedFramePass(uint32_t frameW, uint32_t frameH) const {
        const ViewShape& out = sig.uav[0];
        if (!uav[0] || !out.valid || !out.tex2d || !IsColour(out.fmt) || !frameW || out.w != frameW || out.h != frameH) return false;
        int fullColour = 0; bool flowIn = false;
        // (in HDR the frames are RGBA16F too: a flow field is the smaller one, so that a full-size frame read by a copy or a conversion does not pass for it)
        for (const auto& v : sig.srv) if (v.valid && v.tex2d) { if (IsColour(v.fmt) && v.w == frameW && v.h == frameH) ++fullColour; if (v.fmt == kFlowFormat && Area(v) < (uint64_t)frameW * frameH) flowIn = true; }
        return fullColour >= 2 || flowIn;
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------ lifetime
FrameTap::~FrameTap() { ReleaseAll(); }

void FrameTap::ReleaseAll() {
    Drop(m_flowNew); Drop(m_flowLast); Drop(m_held);
    for (auto& f : m_probeFlows) f.res->Release();
    m_probeFlows.clear();
}

void FrameTap::Reset() {
    std::lock_guard<std::mutex> lk(m_mu);
    ReleaseAll();
    m_flowNewW = m_flowNewH = 0; m_flowNewArea = 0; m_flowLastW = m_flowLastH = 0;
    m_heldSlot = -1; m_handOverNext = false; m_lastGatedTick = ~0ull; m_dropStreak = 0;
    m_presentsSinceTap = 0; m_genSincePresent = false; m_perFrame = 0; m_realFirst = false; m_lastGenKey = 0; m_lastTapFrame = nullptr; m_genPasses = 0; m_hintW = m_hintH = 0;
    snprintf(m_pattern, sizeof m_pattern, "learning");
}

void FrameTap::ClearTable() { std::lock_guard<std::mutex> lk(m_mu); m_table.clear(); }

void FrameTap::SetRoles(const DispatchSig& tick, const DispatchSig& tap, Mode mode, int frameSlotPref) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_tick = tick; m_tap = tap; m_mode = mode; m_frameSlotPref = frameSlotPref;
    m_tickKey = tick.Empty() ? 0 : tick.Hash();
    m_tapKey = tap.Empty() ? 0 : tap.Hash();
}

void FrameTap::GetRoles(DispatchSig& tick, DispatchSig& tap) const { std::lock_guard<std::mutex> lk(m_mu); tick = m_tick; tap = m_tap; }

void FrameTap::SetFreshFlow(bool on) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_freshFlow = on;
    if (!on) { Drop(m_held); m_handOverNext = false; }
}

std::vector<DispatchEntry> FrameTap::Snapshot() const {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<DispatchEntry> rows;
    rows.reserve(m_table.size());
    for (const auto& kv : m_table) rows.push_back(kv.second);
    return rows;
}

// ------------------------------------------------------------------------------------------------------------------------------------ roles
void FrameTap::Record(const DispatchSig& sig, uint64_t key) {
    if (m_table.size() > kMaxTableRows) m_table.clear();
    DispatchEntry& e = m_table[key];
    if (e.count == 0) { e.sig = sig; e.key = key; }
    ++e.count;
    e.lastSeen = m_dispatches;
    if (m_mode != Auto) return;
    const auto tap = m_tapKey ? m_table.find(m_tapKey) : m_table.end();
    const bool tapGone = m_tapKey && (tap == m_table.end() || m_dispatches - tap->second.lastSeen > kStaleAfter);
    if (!m_tapKey || tapGone || m_dispatches - m_lastChoice > kRechooseEvery) { ChooseTapAutomatically(); m_lastChoice = m_dispatches; }
}

// As observed on Lossless Scaling 3.x: the captured real frame is the largest colour texture in recent passes, and the first thing LSFG does with a
// new one is a pass that reads it full size and writes only smaller textures (a luma pyramid). That pass runs once per real frame, before the flow
// and the generated frames, so it is the TAP; the rarest such pass once the counts have settled, the most frequent one before that. A pass that
// writes a full-size colour texture (a generated frame, a copy to the screen) is never it. Only recent passes count, so that the passes of an
// earlier resolution or window mode cannot win.
void FrameTap::ChooseTapAutomatically() {
    auto recent = [&](const DispatchEntry& e) { return m_dispatches - e.lastSeen < kStaleAfter; };
    uint32_t w = 0, h = 0;
    for (const auto& kv : m_table) {
        if (!recent(kv.second)) continue;
        for (const auto& v : kv.second.sig.srv) if (v.valid && v.tex2d && IsColour(v.fmt) && Area(v) > (uint64_t)w * h) { w = v.w; h = v.h; }
    }
    if (!w) return;
    m_frameW = w; m_frameH = h;
    DispatchEntry* rarest = nullptr; DispatchEntry* busiest = nullptr;
    for (auto& kv : m_table) {
        DispatchEntry& e = kv.second;
        e.roleAuto = 0;
        if (!recent(e)) continue;
        bool readsFrame = false, writes = false, writesFullColour = false;
        for (const auto& v : e.sig.srv) if (v.valid && v.w == w && v.h == h && IsColour(v.fmt)) readsFrame = true;
        for (const auto& v : e.sig.uav) if (v.valid) { writes = true; if (v.tex2d && v.w >= w && v.h >= h && IsColour(v.fmt)) writesFullColour = true; }
        if (!readsFrame || !writes || writesFullColour) continue;
        if (!busiest || e.count > busiest->count) busiest = &e;
        if (e.count >= 10 && (!rarest || e.count < rarest->count)) rarest = &e;
    }
    DispatchEntry* tap = rarest ? rarest : busiest;
    if (!tap) return;
    tap->roleAuto = 2;
    m_tap = tap->sig; m_tapKey = tap->key;
    m_tick = DispatchSig{}; m_tickKey = 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------ the frame
bool FrameTap::HandOverHeld(TapDecision& d) {
    d.frame = m_held; m_held = nullptr;   // the reference moves to the caller
    d.frameSlot = m_heldSlot;
    m_handOverNext = false;
    if (ID3D11Texture2D* flow = AsTexture(m_flowNew)) { d.flow = flow; d.flowW = m_flowNewW; d.flowH = m_flowNewH; d.freshFlow = true; }
    ++m_freshRuns;
    return true;
}

bool FrameTap::OnTap(const Bound& b, TapDecision& d) {
    d.isTap = true;
    ++m_taps;
    if (m_presentsSinceTap > 0) m_perFrame = m_presentsSinceTap;
    m_presentsSinceTap = 0; m_genSincePresent = false;

    // The frame: the preferred slot, or else the highest slot holding a colour texture of the pass's largest size
    uint32_t w = 0, h = 0;
    for (const auto& v : b.sig.srv) if (v.valid && IsColour(v.fmt) && Area(v) > (uint64_t)w * h) { w = v.w; h = v.h; }
    int slot = -1;
    if (m_frameSlotPref >= 0 && m_frameSlotPref < 8 && b.srv[m_frameSlotPref]) slot = m_frameSlotPref;
    else for (int i = 7; i >= 0 && slot < 0; --i) if (b.srv[i] && b.sig.srv[i].w == w && b.sig.srv[i].h == h && IsColour(b.sig.srv[i].fmt)) slot = i;

    if (slot >= 0) m_lastTapFrame = b.srv[slot];

    // With a TICK set by hand, once per tick; otherwise (LSFG) every TAP is a new real frame
    const bool gate = m_tickKey ? m_lastGatedTick != m_ticks : true;
    m_gateName = m_tickKey ? "tick" : "every";
    bool runNow = false;
    // LSFG wrote flow since the TAP before, so it is running: this frame's own flow can be waited for again. Several frames in a row without
    // any (frame generation switched off, with the capture going on) and the frames are handed over at once, with no motion, rather than
    // every one being dropped.
    const bool flowSinceTap = m_flowNew != nullptr;
    if (flowSinceTap) m_dropStreak = 0;
    if (slot >= 0 && gate) {
        m_lastGatedTick = m_ticks;
        if (ID3D11Texture2D* frame = AsTexture(b.srv[slot])) {
            bool wait = m_freshFlow && m_flowLastW && m_flowLastH && m_dropStreak < kGiveUpWaitingAfter;   // the finest flow size is known
            if (wait && m_held) {   // LSFG ran no flow pass for the frame before
                Drop(m_held); ++m_droppedWaiting; ++m_dropStreak;
                wait = m_dropStreak < kGiveUpWaitingAfter;
            }
            if (wait) {   // wait for this frame's own flow
                m_held = frame; m_heldSlot = slot; m_handOverNext = false;
            } else if (!d.frame) {
                d.frame = frame; d.frameSlot = slot; runNow = true; ++m_staleRuns;
            } else {
                frame->Release();   // a held frame was handed over at this very dispatch already
            }
        }
    }

    // The finest flow written since the TAP before becomes the frame before's flow; with no flow pass in between (a duplicated capture) the one
    // before that is kept, rather than flipping to no motion for a frame
    if (m_flowNew) { Hold(m_flowLast, m_flowNew); m_flowLastW = m_flowNewW; m_flowLastH = m_flowNewH; Drop(m_flowNew); }
    m_flowNewArea = 0;
    if (runNow && !d.flow && m_flowLast && m_dropStreak < kGiveUpWaitingAfter)
        if (ID3D11Texture2D* flow = AsTexture(m_flowLast)) { d.flow = flow; d.flowW = m_flowLastW; d.flowH = m_flowLastH; }
    return runNow;
}

void FrameTap::OnFlowPass(const Bound& b) {
    const uint64_t area = Area(b.sig.uav[0]);
    if (area <= m_flowNewArea) return;   // coarser than what this frame already wrote, or the auxiliary field at the same level
    Hold(m_flowNew, b.uav[0]); m_flowNewW = b.sig.uav[0].w; m_flowNewH = b.sig.uav[0].h; m_flowNewArea = area;
    // The first pass at (or above) the frame before's finest size is this frame's main flow: hand the held frame over on the next dispatch
    if (m_held && area >= (uint64_t)m_flowLastW * m_flowLastH) m_handOverNext = true;
}

bool FrameTap::Observe(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, TapDecision& d) {
    d = {};
    ++m_dispatches;
    const Bound b(ctx, x, y, z);
    const uint64_t key = b.sig.Hash();
    bool run = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        // The previous dispatch was this frame's finest flow pass, so the flow is in the command stream ahead of anything issued now
        if (m_handOverNext && m_held) run = HandOverHeld(d);
        Record(b.sig, key);
        if (m_tickKey && key == m_tickKey) { d.isTick = true; ++m_ticks; }
        if (m_tapKey && key == m_tapKey) run = OnTap(b, d) || run;
        else if (b.IsFlowPass()) OnFlowPass(b);
        else if (b.IsGeneratedFramePass(m_hintW ? m_hintW : m_frameW, m_hintW ? m_hintH : m_frameH)) { m_genSincePresent = true; ++m_genPasses; if (key != m_lastGenKey) { m_lastGenKey = key; m_lastGenSig = b.sig; } }
    }
    if (m_probeArmed) ProbePass(ctx, b, d);
    return run;
}

// ------------------------------------------------------------------------------------------------------------------------------------ presents
PresentInfo FrameTap::NotePresent() {
    std::lock_guard<std::mutex> lk(m_mu);
    PresentInfo p;
    p.tap = m_taps; p.index = m_presentsSinceTap++; p.perFrame = m_perFrame;
    p.gen = m_genSincePresent; m_genSincePresent = false;
    if (m_taps == 0) return p;
    const int n = m_perFrame > 0 ? m_perFrame : 3;   // X3 until an interval has been counted
    if (!p.gen) {
        // A real frame: the one just captured when it comes after the generated frames, the frame before when it comes first
        m_realFirst = p.index == 0;
        p.target = m_realFirst ? (double)m_taps - 1.0 : (double)m_taps;
    } else {
        const double t = (double)(m_realFirst ? p.index : p.index + 1) / n;
        p.target = (double)m_taps - 1.0 + (t > 1.0 ? 1.0 : t);
    }
    snprintf(m_pattern, sizeof m_pattern, "%d per real frame, real frame %s", n, m_realFirst ? "first" : "last");
    return p;
}

ID3D11Resource* FrameTap::NewestFlow(uint32_t& w, uint32_t& h) {
    std::lock_guard<std::mutex> lk(m_mu);
    ID3D11Resource* r = m_flowNew ? m_flowNew : m_flowLast;
    w = m_flowNew ? m_flowNewW : r ? m_flowLastW : 0;
    h = m_flowNew ? m_flowNewH : r ? m_flowLastH : 0;
    if (r) r->AddRef();
    return r;
}

// ------------------------------------------------------------------------------------------------------------------------------------ flow probe
void FrameTap::ArmProbe(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& f : m_probeFlows) f.res->Release();
    m_probeFlows.clear();
    m_probeDir = dir; m_probeTaps = 0; m_probeFlowArea = 0; m_probeStatus = "armed: waiting for 3 taps"; m_probeArmed = true;
}

std::string FrameTap::ProbeStatus() const { std::lock_guard<std::mutex> lk(m_mu); return m_probeStatus; }

void FrameTap::ProbePass(ID3D11DeviceContext* ctx, const Bound& b, const TapDecision& d) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_probeArmed) return;
    if (b.uav[0] && b.sig.uav[0].valid && b.sig.uav[0].fmt == kFlowFormat) {   // keep every write at the finest RGBA16F size seen
        const uint64_t area = Area(b.sig.uav[0]);
        if (area > m_probeFlowArea) { m_probeFlowArea = area; for (auto& f : m_probeFlows) f.res->Release(); m_probeFlows.clear(); }
        if (area == m_probeFlowArea && m_probeFlows.size() < 8) {
            b.uav[0]->AddRef();
            m_probeFlows.push_back({ b.uav[0], m_dispatches });
            ProbeLog("flow pass #%llu U0=%p (%ux%u) S4=%p", (unsigned long long)m_dispatches, (void*)b.uav[0], b.sig.uav[0].w, b.sig.uav[0].h, (void*)b.srv[4]);
        }
    }
    if (d.isTap && d.frame) {
        ProbeLog("TAP #%llu frame=%p (%ux%u): dumping the frame and %zu flow texture(s)", (unsigned long long)m_dispatches, (void*)d.frame, m_frameW, m_frameH, m_probeFlows.size());
        ProbeDump(ctx, d.frame, L"frame", m_probeTaps, 0);
        int k = 0;
        for (auto& f : m_probeFlows) { ProbeDump(ctx, f.res, L"flow", m_probeTaps, k++); f.res->Release(); }
        m_probeFlows.clear();
        if (++m_probeTaps >= 3) { m_probeArmed = false; m_probeStatus += "\nprobe done: files in the Lossless Scaling folder (flowprobe_*.bin)"; }
    }
}

void FrameTap::ProbeLog(const char* fmt, ...) {
    char line[512];
    va_list a; va_start(a, fmt); vsnprintf(line, sizeof line, fmt, a); va_end(a);
    if (m_probeStatus.size() < 6000) { m_probeStatus += "\n"; m_probeStatus += line; }
    if (FILE* f = _wfopen((m_probeDir + L"\\flowprobe_log.txt").c_str(), L"a")) { fprintf(f, "%s\n", line); fclose(f); }
}

// A staging copy of the texture, written tightly packed to flowprobe_<tag><tap>_<sub>_<w>x<h>_fmt<f>.bin
void FrameTap::ProbeDump(ID3D11DeviceContext* ctx, ID3D11Resource* r, const wchar_t* tag, int tapIdx, int sub) {
    ID3D11Texture2D* t = AsTexture(r);
    if (!t) return;
    D3D11_TEXTURE2D_DESC desc; t->GetDesc(&desc);
    const uint32_t bpp = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : desc.Format == DXGI_FORMAT_R8_UNORM ? 1 : 4;
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.BindFlags = 0; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    ID3D11Texture2D* staging = nullptr;
    const HRESULT made = dev ? dev->CreateTexture2D(&sd, nullptr, &staging) : E_FAIL;
    if (SUCCEEDED(made) && staging) {
        ctx->CopyResource(staging, t);
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT mapped = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        if (SUCCEEDED(mapped)) {
            wchar_t name[MAX_PATH];
            swprintf(name, MAX_PATH, L"%s\\flowprobe_%s%d_%d_%ux%u_fmt%u.bin", m_probeDir.c_str(), tag, tapIdx, sub, desc.Width, desc.Height, (unsigned)desc.Format);
            FILE* f = _wfopen(name, L"wb");
            if (f) { for (UINT y = 0; y < desc.Height; ++y) fwrite((const uint8_t*)m.pData + (size_t)y * m.RowPitch, 1, (size_t)desc.Width * bpp, f); fclose(f); }
            ctx->Unmap(staging, 0);
            ProbeLog("  wrote %ls (%s)", name, f ? "ok" : "OPEN FAILED");
        } else {
            ProbeLog("  Map failed 0x%08x for %ls (deferred context?)", (unsigned)mapped, tag);
        }
        staging->Release();
    } else {
        ProbeLog("  staging CreateTexture2D failed 0x%08x", (unsigned)made);
    }
    if (dev) dev->Release();
    t->Release();
}

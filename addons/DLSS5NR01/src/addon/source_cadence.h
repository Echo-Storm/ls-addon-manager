// Source cadence: does the picture Lossless Scaling captures repeat? A 30 fps game seen through a capture card at 60 or 120 Hz arrives as A A B B (or A A A A B B B B); frame generation then
// makes its pictures between pairs that are the same, and the motion advances unevenly (docs/capture-card-sources.md). This only notices and says so; dropping the repeats is for Lossless
// Scaling or the capture program.
//
//   Detector  pure: fed one flag per captured frame (a repeat of the one before, or not), it keeps the lengths of the stays of each picture and says when the stays are all the same
//             length of two or more (a still scene is one very long stay, a game at its real rate is all stays of one: neither counts).
//   Probe     the graphics part: a few thousand points of each captured frame's luma by a small compute pass, read back a few frames later, compared with the frame before.
#pragma once
#include <cstdint>
#include <d3d11.h>
#include <deque>

namespace nr::cadence {

struct Result {
    int repeats = 0;       // each picture is seen this many times in a row (2: every other captured frame repeats); 0 = none noticed
    int stays = 0;         // how many stays the verdict rests on
};

class Detector {
public:
    static constexpr int kKeep = 48, kNeed = 24, kMaxRepeat = 8;
    void Push(bool repeatOfBefore);
    void Break();                              // frames were lost between two reads
    void Reset();
    Result Verdict() const;                    // the stays all of one length L in 2..kMaxRepeat (85 % or more of the last kKeep, at least kNeed of them)
    uint64_t Frames() const { return m_frames; }
private:
    std::deque<int> m_stays; int m_run = 0; uint64_t m_frames = 0;
};

// The compare of two reads: the points' mean change (of l / (1 + l), l the luma) under this is the same picture again (a repeat is bit for bit the same, 0; the smallest change of real frames measured on recordings of WoW was 0.00056).
constexpr float kSameMean = 0.00015f;

class Probe {
public:
    ~Probe() { Shutdown(); }
    // Called at each captured frame, on the render thread of `dev`'s immediate context `ctx`: runs the sample pass on `frame`, restoring the compute state it touches, and reads
    // back the samples of earlier frames that the card has finished (a few frames late, in order), pushing for each whether it repeats the one before into `det`. Returns how many were pushed.
    int Observe(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, Detector& det);
    void Shutdown();
    void Forget() { Shutdown(); m_failed = false; m_missed = 0; }   // (the device is gone or changed: a failure on the one before is no reason not to try on the next)
    uint64_t Missed() const { return m_missed; }
    bool Failed() const { return m_failed; }
private:
    bool Init(ID3D11Device* dev);
    ID3D11ShaderResourceView* ViewOf(ID3D11Texture2D* frame, uint32_t* w, uint32_t* h);
    static constexpr int kGx = 64, kGy = 36, kPoints = kGx * kGy, kRing = 64;
    ID3D11Device* m_dev = nullptr;
    ID3D11ComputeShader* m_cs = nullptr; ID3D11Buffer* m_cb = nullptr;
    ID3D11Buffer* m_samples[2] = {}; ID3D11UnorderedAccessView* m_sampleUav[2] = {}; int m_flip = 0;   // this frame's points and the frame before's, swapped each time
    ID3D11Buffer* m_result = nullptr; ID3D11UnorderedAccessView* m_resultUav = nullptr;               // the mean change between them (one float), made on the card
    ID3D11Buffer* m_stage[kRing] = {}; uint64_t m_written = 0, m_read = 0;
    struct View { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; uint32_t w = 0, h = 0; } m_views[8]; int m_nextView = 0;
    uint64_t m_missed = 0; bool m_failed = false;
};

} // namespace nr::cadence

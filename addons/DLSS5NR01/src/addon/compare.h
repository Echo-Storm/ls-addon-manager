// Comparing the upscalers by eye: a key (Ctrl+Shift+F9) steps through Lossless Scaling's own NIS and each upscaler addon that is loaded, one at a time, and a label in the top left
// of the picture says which one is drawing. The addons are separate DLLs in one process, so the state lives in a named shared-memory block (Local\EAM_Compare_<pid>); each addon
// registers its modes at start, polls the key in its pass (the first to see a press takes it), and acts only in its own turn.
//
//   NIS, DLSS, FSR, XeSS, VSR (gated), VSR (everywhere), then off: the press after the last mode ends the comparison.
//
// While the comparison is off, one addon acts, the lowest registered (DLSS, FSR, XeSS, VSR): with the usual single upscaler that is just it. The addon whose turn it is also records
// what is shown, in short bursts (so a minute of each is a few hundred frames, not tens of thousands), and the previous mode's recording is saved when the mode changes; every change is
// written to logs\compare_timeline.csv with the time.
#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

namespace nr::compare {

enum Mode : int { kNis = 0, kDlss = 1, kFsr = 2, kXess = 3, kVsrGated = 4, kVsrAlways = 5, kModes = 6 };

// An addon registers each of its modes at start (DLSS 1; VSR 4 and 5) and withdraws them at shutdown.
void Register(int mode, const char* name, uint32_t rgb);
void Unregister(int mode);

// Reads the key; every addon calls it in its pass (cheap).
void Poll();

bool Active();
int Current();
const char* Name(int mode);

// May the addon whose modes are lo..hi (DLSS 1..1, VSR 4..5) upscale now?
bool MyTurn(int lo, int hi);
// Should it draw the label (and record) now: its mode is the current one, or NIS is and it is the lowest registered?
bool WantsLabel(int lo, int hi);

// Should the addon with this mode record what is shown now (the comparison is on, and it is the lowest registered of DLSS, FSR, XeSS: they have the recorder, VSR has none)?
bool Records(int mode);

// For the tests (no key to press): start in this mode.
void StartIn(int mode);

// Draws the label into the output picture the pass bound as u0 (the pass's own output viewport: x, y, w, h), in the frame's encoding. A no-op while the comparison is off.
void DrawLabel(ID3D11DeviceContext* ctx, uint32_t outX, uint32_t outY, uint32_t outW, uint32_t outH, uint32_t encoding, float whiteNits);

// Recording in bursts while comparing: a window of `burstMs` every `periodMs`, starting `settleMs` after the mode began (the engines need a moment). Set once by whichever addon
// starts first (the same values from every addon's settings: compareEveryMs, compareBurstMs).
void SetCapture(uint32_t periodMs, uint32_t burstMs, uint32_t settleMs);
bool BurstOpen();
// How long the current mode has been on, in ms.
uint64_t ModeAgeMs();

} // namespace nr::compare

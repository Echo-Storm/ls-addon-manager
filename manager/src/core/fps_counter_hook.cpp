#include "fps_counter_hook.h"
#include "hook_util.h"
#include "../log/logger.h"
#include <MinHook.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace eam::fpscounter {

namespace {

// The one build this knows: Lossless_original.dll of 2025-08-05 (PE time stamp 0x68914EFB, image size 0x732000).
constexpr DWORD kStamp = 0x68914EFB, kImageSize = 0x732000;
constexpr uintptr_t kFunctionRva = 0x1C950;   // the counter's once-a-frame function: counts to rates when a second has passed, then draws

// The start of that function: mov [rsp+18],rbp; mov [rsp+20],rsi; push rdi; sub rsp,270; ... movsd xmm4,[rcx+28]; xor ebp,ebp; movsd xmm3,[rip+..]; mov rdi,rcx; comisd xmm4,xmm3; jb ...
// (the bytes that hold a rip-relative offset are left out of the check)
struct Piece { size_t at, size; const uint8_t* bytes; };
const uint8_t kA[] = { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x70, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x05 };
const uint8_t kB[] = { 0xF2, 0x0F, 0x10, 0x61, 0x28, 0x33, 0xED, 0xF2, 0x0F, 0x10, 0x1D };
const uint8_t kC[] = { 0x48, 0x8B, 0xF9, 0x66, 0x0F, 0x2F, 0xE3, 0x72, 0x4A };
const Piece kPieces[] = { { 0, sizeof kA, kA }, { 36, sizeof kB, kB }, { 51, sizeof kC, kC } };

// The counter object (this = rcx): what the function reads and writes.
struct Counter {
    int64_t captured;      // +0x00 frames captured in this second
    int64_t presented;     // +0x08 frames presented in this second
    double capturedRate;   // +0x10
    double capturedMs;     // +0x18 time the captured frames took
    double presentedRate;  // +0x20
    double presentedMs;    // +0x28 time the presented frames took (>= 1000: the rates are worked out now)
};

using CounterFn = void(__fastcall*)(Counter*);
CounterFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<float> g_multiplier{ 1.0f };
std::atomic<bool> g_hooked{ false };

void __fastcall OnCounter(Counter* c) {
    const float m = g_multiplier.load(std::memory_order_relaxed);
    if (m > 1.0f && c && c->presentedMs >= 1000.0 && c->presented > 0) c->presented = static_cast<int64_t>(std::llround(static_cast<double>(c->presented) * m));
    g_original(c);
}

} // namespace

bool Install(HMODULE lossless) {
    if (g_hooked || !lossless) return g_hooked;
    const auto* base = reinterpret_cast<const uint8_t*>(lossless);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kStamp || nt->OptionalHeader.SizeOfImage != kImageSize) {
        LOG_INFO("FpsCounter", "Lossless_original.dll is not the build the counter hook knows (stamp %08lX): Lossless Scaling's frame rate counter is left as it is", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
        return false;
    }
    const uint8_t* fn = base + kFunctionRva;
    for (const Piece& p : kPieces)
        if (memcmp(fn + p.at, p.bytes, p.size) != 0) { LOG_WARN("FpsCounter", "The counter function's code is not what was expected: its hook is not installed"); return false; }
    if (!hooks::Begin()) return false;
    g_target = const_cast<uint8_t*>(fn);
    if (MH_CreateHook(g_target, reinterpret_cast<void*>(&OnCounter), reinterpret_cast<void**>(&g_original)) != MH_OK || MH_EnableHook(g_target) != MH_OK) {
        MH_RemoveHook(g_target); g_target = nullptr; hooks::End();
        LOG_WARN("FpsCounter", "The counter hook could not be created");
        return false;
    }
    g_hooked = true;
    LOG_INFO("FpsCounter", "Lossless Scaling's frame rate counter is hooked (an addon can make it show the rate the screen gets)");
    return true;
}

void Uninstall() {
    if (!g_hooked) return;
    g_multiplier = 1.0f;
    MH_DisableHook(g_target); MH_RemoveHook(g_target);
    g_target = nullptr; g_original = nullptr; g_hooked = false;
    hooks::End();
}

bool SetMultiplier(float multiplier) {
    if (!g_hooked) return false;
    g_multiplier = std::isfinite(multiplier) ? std::fmin(std::fmax(multiplier, 1.0f), 16.0f) : 1.0f;
    return true;
}

} // namespace eam::fpscounter

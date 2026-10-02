// Offline test of the addon's settings (settings.cpp): looks as text and back, their ranges, the HUD areas, names, and the settings file
// through a stand-in host. No graphics card, no model.
#include "addon/settings.h"
#include "addon/screenshot.h"
#include <eam/addon_sdk.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

// The settings file as the manager keeps it: text under a key.
struct StandInHost : IHost {
    std::map<std::string, std::string> values; int saves = 0;
    void Log(EamLogLevel, const char*) override {}
    const char* GetConfig(const char*, const char* key, const char* dflt) override { auto it = values.find(key); return it == values.end() ? dflt : it->second.c_str(); }
    void SetConfig(const char*, const char* key, const char* value) override { values[key] = value; }
    void SaveConfig() override { ++saves; }
    uint32_t GetHostVersion() override { return EAM_API_VERSION_INT; }
    void SubscribeEvent(uint32_t, EamEventCallback, void*) override {}
    void UnsubscribeEvent(uint32_t, EamEventCallback) override {}
    void PublishEvent(uint32_t, const void*, uint32_t) override {}
    void* GetD3D11Device() override { return nullptr; }
    void* GetD3D11DeviceContext() override { return nullptr; }
    void SetPreDispatchCallback(EamPreDispatchCallback, void*) override {}
    void SetPostDispatchCallback(EamPostDispatchCallback, void*) override {}
    void* GetCurrentComputeShader() override { return nullptr; }
    uint32_t GetDispatchCount() override { return 0; }
    void SetStatus(const char*, const char*, int) override {}
    void PublishMetric(const char*, const char*, double, const char*) override {}
    void* GetDispatchingContext() override { return nullptr; }
    void* CreateImage(const void*, uint32_t, uint32_t, uint32_t) override { return nullptr; }
    void ReleaseImage(void*) override {}
};

static bool Same(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    using namespace nr;
    printf("== looks as text\n");
    NrParams a;
    a.style = 2; a.intensity = 0.7f; a.localStructure = -1.5f; a.passes = 3; a.useFlow = false; a.workingScale = 0.6f; a.sharpen = 0.1f;
    a.shadows = -0.4f; a.grain = 0.25f; a.grainSize = 2; a.deltaSmooth = 0.3f; a.ghostGuard = 0.8f; a.hudCount = 2;
    a.hud[0][0] = 0.1f; a.hud[0][1] = 0.2f; a.hud[0][2] = 0.3f; a.hud[0][3] = 0.4f; a.hud[1][0] = 0.5f; a.hud[1][1] = 0.6f; a.hud[1][2] = 0.9f; a.hud[1][3] = 1.0f;
    NrParams b;
    Check("a look written as text reads back as the same look", ApplyLook(LookToText(a), b) && LookToText(b) == LookToText(a), LookToText(b));
    Check("...every setting of it", b.style == 2 && Same(b.intensity, 0.7f) && Same(b.localStructure, -1.5f) && b.passes == 3 && !b.useFlow && Same(b.workingScale, 0.6f) &&
          Same(b.shadows, -0.4f) && Same(b.grainSize, 2) && Same(b.deltaSmooth, 0.3f) && Same(b.ghostGuard, 0.8f) && b.hudCount == 2 && Same(b.hud[1][2], 0.9f));
    NrParams c;
    ApplyLook("sharpen=0.5", c);
    Check("a look that names only some settings changes only those", Same(c.sharpen, 0.5f) && Same(c.intensity, NrParams().intensity) && c.passes == NrParams().passes);
    NrParams d;
    ApplyLook("passes=9;workingScale=0.05;saturation=7;grainSize=0;deltaSmooth=1;style=5;brightness=-3", d);
    Check("values out of range are kept to their range", d.passes == 4 && Same(d.workingScale, 0.25f) && Same(d.saturation, 2.0f) && Same(d.grainSize, 1.0f) &&
          Same(d.deltaSmooth, 0.95f) && d.style == 2 && Same(d.brightness, -0.3f));
    NrParams e;
    Check("text that names no known setting is not a look", !ApplyLook("colour=5;;=;nothing", e) && LookToText(e) == LookToText(NrParams()));
    Check("the settings that are not part of a look stay out of it", LookToText(a).find("debugView") == std::string::npos && LookToText(a).find("flowUnit") == std::string::npos);

    printf("== HUD areas\n");
    NrParams h;
    HudFromText("0,0,0.5,0.5/0.2,0.2,0.2,0.9/-1,-1,2,2/junk/0.6,0.6,1,1/0.1,0.1,0.2,0.2/0.3,0.3,0.4,0.4/0.5,0.5,0.6,0.6/0.7,0.7,0.8,0.8", h);
    Check("areas are read, one with no width is left out, and at most six are kept", h.hudCount == 6 && Same(h.hud[1][0], 0.0f) && Same(h.hud[1][2], 1.0f), std::to_string(h.hudCount));
    Check("...and one reaching past the screen is pulled inside it", Same(h.hud[1][3], 1.0f));

    printf("== names\n");
    Check("a name loses the characters the stores use, trailing spaces, and anything past 40 characters",
          CleanName("a|b;c=d  ") == "a-b-c-d" && CleanName(std::string(60, 'x')).size() == 40);

    printf("== the settings file\n");
    StandInHost host;
    Loaded fresh = LoadSettings(&host, "DLSS5NR01");
    Check("with nothing saved, every setting is its default", LookToText(fresh.config.p) == LookToText(NrParams()) && fresh.config.enabled && fresh.config.freshFlow &&
          fresh.config.keyAB == VK_F7 && fresh.config.keyRecord == VK_F1 && fresh.config.keyShot == VK_F4 && fresh.looks.empty());
    {   // the hotkeys: settings saved before 0.9.8 with the old defaults move to the safer ones once; a key chosen by hand stays
        StandInHost old;
        old.values["keyAB"] = std::to_string(VK_F6); old.values["keySharpUp"] = std::to_string(VK_F9); old.values["keyRecord"] = std::to_string(VK_F5);
        old.values["keyShot"] = std::to_string(VK_F11); old.values["keySplit"] = std::to_string(VK_F12);   // (F12: chosen by hand)
        const Loaded moved = LoadSettings(&old, "DLSS5NR01");
        Check("old default keys move to the safer ones (F6 to F7, F9 to F3, F5 to F1, F11 to F4)", moved.config.keyAB == VK_F7 && moved.config.keySharpUp == VK_F3 &&
              moved.config.keyRecord == VK_F1 && moved.config.keyShot == VK_F4);
        Check("...and a key chosen by hand stays", moved.config.keySplit == VK_F12);
        StandInHost now;
        now.values["keysVersion"] = "3"; now.values["keyAB"] = std::to_string(VK_F6); now.values["keyRecord"] = std::to_string(VK_F5);
        const Loaded kept = LoadSettings(&now, "DLSS5NR01");
        Check("once moved (keysVersion 3), F6 or F5 chosen again later stay", kept.config.keyAB == VK_F6 && kept.config.keyRecord == VK_F5);
    }
    Config config; config.p = a; config.enabled = false; config.keySplit = VK_F11; config.games = { { "wowb.exe", "Night" } };
    config.tapMode = 1; config.tapSig = "1,2,3"; config.snippetPath = "D:\\model.dll"; config.watchdogMs = 55;
    SaveSettings(&host, "DLSS5NR01", config, { { "Night", LookToText(a) }, { "Day", "sharpen=0.2" } });
    Check("saving writes the file once", host.saves == 1);
    Loaded back = LoadSettings(&host, "DLSS5NR01");
    Check("what was saved loads back", LookToText(back.config.p) == LookToText(a) && !back.config.enabled && back.config.keySplit == VK_F11 && back.config.tapMode == 1 &&
          back.config.tapSig == "1,2,3" && back.config.snippetPath == "D:\\model.dll" && Same(back.config.watchdogMs, 55));
    Check("...with the looks, in order, and the programs", back.looks.size() == 2 && back.looks[0].name == "Night" && back.looks[1].data == "sharpen=0.2" &&
          back.config.games.size() == 1 && back.config.games[0].second == "Night");
    host.values["saturation"] = "9"; host.values["passes"] = "-4"; host.values["debugView"] = "12";
    Loaded wild = LoadSettings(&host, "DLSS5NR01");
    Check("values edited out of range in the file are kept to their range", Same(wild.config.p.saturation, 2.0f) && wild.config.p.passes == 1 && wild.config.p.debugView == 5);
    {   // how often the model runs: one by default, kept to 1..4, and it round trips
        Check("the model runs on every 2nd frame by default (since 0.9.28)", Config().modelEvery == 2 && LoadSettings(&host, "DLSS5NR01").config.modelEvery == 2);
        Config m; m.modelEvery = 3; SaveSettings(&host, "DLSS5NR01", m, {});
        Check("how often the model runs is kept in the settings", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 3);
        host.values["modelEvery"] = "9"; Check("...and kept to 1 to 4", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 4);
        host.values["modelEvery"] = "0"; Check("...from 1", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 1);
        host.values["modelEvery"] = "1";
        host.values.erase("modelEvery");   // a config saved before the setting existed (an upgrade): the default, not a stale 1
        Check("a saved config without the setting takes the default (every 2nd frame)", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 2);
        host.values["modelEvery"] = "1";
    }
    {   // 0.9.28 saved a 1 into configs that never had the setting: a file under no defaults version gets the default once; a 1 chosen after that stays
        host.values["modelEvery"] = "1"; host.values.erase("defaultsVersion");
        Check("a 1 from a file written before the defaults' version becomes the default (the 0.9.28 upgrade)", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 2);
        host.values["modelEvery"] = "1"; host.values["defaultsVersion"] = "1";
        Check("...but a 1 in a file under the current version is a choice and stays", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 1);
        host.values["modelEvery"] = "3"; host.values.erase("defaultsVersion");
        Check("...and a 3 is never touched", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 3);
        Config m = LoadSettings(&host, "DLSS5NR01").config; m.modelEvery = 1; SaveSettings(&host, "DLSS5NR01", m, {});
        Check("a 1 chosen and saved stays after the next load", LoadSettings(&host, "DLSS5NR01").config.modelEvery == 1);
        host.values["modelEvery"] = "2"; host.values["defaultsVersion"] = "1";
    }
    {   // keeping the card under a limit
        Check("the dark guard is 0.04 by default, kept to 0..0.15, and round trips", Same(NrParams().darkGuard, 0.04f) && Same(Config().p.darkGuard, 0.04f));
        { Config d; d.p.darkGuard = 0.08f; SaveSettings(&host, "DLSS5NR01", d, {}); Check("...(0.08 saved loads back as 0.08)", Same(LoadSettings(&host, "DLSS5NR01").config.p.darkGuard, 0.08f)); host.values["darkGuard"] = "5"; Check("...(5 is kept to 0.15)", Same(LoadSettings(&host, "DLSS5NR01").config.p.darkGuard, 0.15f)); host.values["darkGuard"] = "0.04"; }
        Check("the card limit is off by default, at 95 %", !Config().gpuLimit && Same(Config().gpuLimitPercent, 95.0f));
        Config g; g.gpuLimit = true; g.gpuLimitPercent = 90.0f; SaveSettings(&host, "DLSS5NR01", g, {});
        const Config back2 = LoadSettings(&host, "DLSS5NR01").config;
        Check("the card limit round trips", back2.gpuLimit && Same(back2.gpuLimitPercent, 90.0f));
        host.values["gpuLimitPercent"] = "10"; Check("...and is kept to 80 to 100", Same(LoadSettings(&host, "DLSS5NR01").config.gpuLimitPercent, 80.0f));
        host.values["gpuLimitPercent"] = "500"; Check("...from above", Same(LoadSettings(&host, "DLSS5NR01").config.gpuLimitPercent, 100.0f));
        host.values["gpuLimit"] = "0"; host.values["gpuLimitPercent"] = "95";
    }
    {   // the upscalers' settings per game
        Config u; u.p.sharpen = 0.4f; u.scalerStability = 0.3f; u.scalerEdges = 0.6f; u.dlaaPreset = 13; u.motionSource = 1;
        KeepForGame(u, "falloutnv.exe");
        u.p.sharpen = 0.8f; u.scalerStability = 0.0f;
        KeepForGame(u, "wowb.exe");
        u.p.sharpen = 0.5f; KeepForGame(u, "falloutnv.exe");   // a change for a game replaces what it had
        SaveSettings(&host, "DLSS5NR01", u, {});
        const Loaded ub = LoadSettings(&host, "DLSS5NR01");
        const auto& g = ub.config.scalerGames;
        Check("the upscalers' settings per game load back, a change replacing a game's own", g.size() == 2 && g[0].first == "falloutnv.exe" &&
              Same(g[0].second.sharpen, 0.5f) && Same(g[0].second.stability, 0.0f) && Same(g[0].second.edges, 0.6f) && g[0].second.preset == 13 &&
              g[0].second.motion == 1 && g[1].first == "wowb.exe" && Same(g[1].second.sharpen, 0.8f) && ub.config.scalerPerGame,
              std::to_string(g.size()) + " " + host.values["scalerGameList"] + " " + host.values["scalerGame.falloutnv.exe"]);
        Config v; ApplyProfile(v, g[1].second);
        Check("...and a game's settings go back into the settings in use", Same(v.p.sharpen, 0.8f) && Same(v.scalerStability, 0.0f) && Same(v.scalerEdges, 0.6f) &&
              v.dlaaPreset == 13);
        host.values["scalerGame.wowb.exe"] = "garbage";
        Check("a game whose line cannot be read is left out", LoadSettings(&host, "DLSS5NR01").config.scalerGames.size() == 1);
    }
    ForgetLook(&host, "DLSS5NR01", "Night");
    Check("a deleted look's text is cleared", host.values["preset.Night"].empty());

    printf("== screenshot pixels\n");
    {
        unsigned char out[8];
        const unsigned char bgra[8] = { 10, 20, 30, 0, 40, 50, 60, 7 };
        Check("BGRA8 is kept as it is, with full alpha", screenshot::ToBgra8(DXGI_FORMAT_B8G8R8A8_UNORM, bgra, 2, out) && out[0] == 10 && out[2] == 30 && out[3] == 255 && out[6] == 60 && out[7] == 255);
        const unsigned char rgba[4] = { 10, 20, 30, 0 };
        Check("RGBA8 has red and blue swapped", screenshot::ToBgra8(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, rgba, 1, out) && out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 255);
        const uint32_t tenBit = 1023u | (512u << 10) | (0u << 20);   // red full, green half, blue none
        Check("10-bit colour comes down to 8 bits", screenshot::ToBgra8(DXGI_FORMAT_R10G10B10A2_UNORM, &tenBit, 1, out) && out[2] == 255 && out[1] == 128 && out[0] == 0);
        const uint16_t half[4] = { 0x3C00, 0x3800, 0x4000, 0x3C00 };   // 1.0, 0.5, 2.0 (cut to 1), 1.0
        Check("half-float colour is kept to 0..1", screenshot::ToBgra8(DXGI_FORMAT_R16G16B16A16_FLOAT, half, 1, out) && out[2] == 255 && out[1] == 128 && out[0] == 255);
        Check("a format it cannot convert is refused", !screenshot::ToBgra8(DXGI_FORMAT_R32_FLOAT, half, 1, out));
    }

    printf("== a settings file full of garbage\n");
    {
        // every key answers with the same bad text: whatever loads must be finite and inside what the engines can take
        struct GarbageHost : StandInHost { std::string text; const char* GetConfig(const char*, const char*, const char*) override { return text.c_str(); } };
        const char* garbage[] = { "nan", "-nan", "inf", "-inf", "1e308", "-1e308", "99999999999", "-99999999999", "-5", "0", "", "abc", "0x7fffffff", "1;2;3", "||;;==", "true", "-0" };
        bool allFinite = true, ranges = true; std::string first;
        for (const char* g : garbage) {
            GarbageHost host; host.text = g;
            Config c = LoadSettings(&host, "DLSS5NR01").config;
            const float fs[] = { c.scalerLeanRest, c.scalerMoveCut, c.scalerSteadySharp, c.scalerLeanFrom, c.scalerStability, c.scalerEdges, c.recordSeconds, c.autoBudgetMs, c.autoFloor, c.autoScaleLast, c.watchdogMs,
                                 c.p.intensity, c.p.localStructure, c.p.localTone, c.p.skinStructure, c.p.flowUnit, c.p.workingScale, c.p.smoothStable, c.p.deltaSmooth, c.p.composeIntensity, c.p.maxDelta,
                                 c.p.ghostGuard, c.p.hiProtect, c.p.sharpen, c.p.saturation, c.p.vibrance, c.p.brightness, c.p.contrast, c.p.gamma, c.p.shadows, c.p.highlights, c.p.grain, c.p.grainSize, c.p.hudFeather };
            for (size_t i = 0; i < sizeof fs / sizeof fs[0]; ++i) if (!std::isfinite(fs[i]) || std::fabs(fs[i]) > 1e4f) { allFinite = false; if (first.empty()) first = std::string("text '") + g + "', setting number " + std::to_string(i); }
            const bool ok = c.scalerLeanRest >= 0 && c.scalerLeanRest <= 1 && c.scalerMoveCut >= 0 && c.scalerMoveCut <= 1 && c.scalerSteadySharp >= 0 && c.scalerSteadySharp <= 0.9f &&
                            c.p.workingScale >= 0.25f && c.p.workingScale <= 1.0f && c.p.passes >= 1 && c.p.passes <= 4 && c.p.flowUnit > 0.0f && c.p.grainSize >= 1 && c.p.hudCount <= (uint32_t)NrParams::kMaxHud &&
                            c.autoBudgetMs > 0 && c.autoFloor >= 0.25f && c.autoFloor <= 1.0f && c.watchdogMs > 0 && c.recordSeconds > 0 && c.recordBudgetMb > 0 &&
                            c.p.deltaSmooth >= 0 && c.p.deltaSmooth < 1 && c.p.style <= 2;
            if (!ok) { ranges = false; if (first.empty()) first = std::string("text '") + g + "'"; }
        }
        Check("garbage in every key loads as finite numbers", allFinite, first);
        Check("...inside the ranges the engines take", ranges, first);
    }

    printf("\n%s\n", g_failed ? "SETTINGS TEST FAILED" : "SETTINGS TEST PASSED");
    return g_failed ? 1 : 0;
}

#include "addon/settings.h"
#include "addon/product.h"
#include <eam/addon_sdk.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr {

namespace {

// The settings of NrParams, each once: its key, its range (FLT_MAX: the model takes any value), and whether it is part of a look.
struct FloatSetting { const char* key; float NrParams::* field; float lo, hi; bool inLook; };
struct UIntSetting { const char* key; uint32_t NrParams::* field; uint32_t lo, hi; bool inLook; };

const FloatSetting kFloats[] = {
    { "intensity",        &NrParams::intensity,        -FLT_MAX, FLT_MAX, true },   // the model keeps it to 0..1 itself
    { "localStructure",   &NrParams::localStructure,   -FLT_MAX, FLT_MAX, true },
    { "localTone",        &NrParams::localTone,        -FLT_MAX, FLT_MAX, true },
    { "skinStructure",    &NrParams::skinStructure,    -FLT_MAX, FLT_MAX, true },
    { "workingScale",     &NrParams::workingScale,     0.25f, 1.0f,  true },
    { "composeIntensity", &NrParams::composeIntensity, -FLT_MAX, FLT_MAX, true },
    { "maxDelta",         &NrParams::maxDelta,         -FLT_MAX, FLT_MAX, true },
    { "hiProtect",        &NrParams::hiProtect,        -FLT_MAX, FLT_MAX, true },
    { "sharpen",          &NrParams::sharpen,          0.0f, 1.0f,   true },
    { "saturation",       &NrParams::saturation,       0.0f, 2.0f,   true },
    { "vibrance",         &NrParams::vibrance,         0.0f, 1.0f,   true },
    { "brightness",       &NrParams::brightness,       -0.3f, 0.3f,  true },
    { "contrast",         &NrParams::contrast,         0.5f, 1.5f,   true },
    { "gamma",            &NrParams::gamma,            0.5f, 2.0f,   true },
    { "shadows",          &NrParams::shadows,          -1.0f, 1.0f,  true },
    { "highlights",       &NrParams::highlights,       -1.0f, 1.0f,  true },
    { "grain",            &NrParams::grain,            0.0f, 1.0f,   true },
    { "grainSize",        &NrParams::grainSize,        1.0f, 4.0f,   true },
    { "deltaSmooth",      &NrParams::deltaSmooth,      0.0f, 0.95f,  true },
    { "ghostGuard",       &NrParams::ghostGuard,       0.0f, 1.0f,   true },
    { "hudFeather",       &NrParams::hudFeather,       0.0f, 0.05f,  true },
    { "flowUnit",         &NrParams::flowUnit,         -FLT_MAX, FLT_MAX, false },
};
const UIntSetting kUInts[] = {
    { "passes",    &NrParams::passes,      1, 4, true },
    { "style",     &NrParams::style,       0, 2, true },
    { "autoMask",  &NrParams::useAutoMask, 0, 1, true },
    { "debugView", &NrParams::debugView,   0, 5, false },
    { "modelMotion", &NrParams::modelMotion, 0, 1, false },
};

float Limit(float v, const FloatSetting& s) { return std::clamp(v, s.lo, s.hi); }
uint32_t Limit(long long v, const UIntSetting& s) { return static_cast<uint32_t>(std::clamp<long long>(v, s.lo, s.hi)); }

std::string Number(float v) { char text[32]; snprintf(text, sizeof text, "%g", v); return text; }

// "a|b|c" and back.
std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('|', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& s : items) { if (!out.empty()) out += '|'; out += s; }
    return out;
}

} // namespace

std::string HudToText(const NrParams& p) {
    std::string text;
    for (uint32_t i = 0; i < p.hudCount && i < static_cast<uint32_t>(NrParams::kMaxHud); ++i) {
        char area[96];
        snprintf(area, sizeof area, "%s%g,%g,%g,%g", i ? "/" : "", p.hud[i][0], p.hud[i][1], p.hud[i][2], p.hud[i][3]);
        text += area;
    }
    return text;
}

// Areas outside 0..1 are pulled in; an area with no width or height is left out.
void HudFromText(const std::string& text, NrParams& p) {
    p.hudCount = 0;
    for (size_t start = 0; start < text.size() && p.hudCount < static_cast<uint32_t>(NrParams::kMaxHud);) {
        size_t end = text.find('/', start);
        if (end == std::string::npos) end = text.size();
        float r[4] = {};
        if (sscanf(text.substr(start, end - start).c_str(), "%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4) {
            for (float& v : r) v = std::clamp(v, 0.0f, 1.0f);
            if (r[2] > r[0] + 0.001f && r[3] > r[1] + 0.001f) memcpy(p.hud[p.hudCount++], r, sizeof r);
        }
        start = end + 1;
    }
}

std::string LookToText(const NrParams& p) {
    std::string text;
    for (const UIntSetting& s : kUInts) if (s.inLook) text += std::string(s.key) + "=" + std::to_string(p.*s.field) + ";";
    text += std::string("useFlow=") + (p.useFlow ? "1" : "0") + ";";
    for (const FloatSetting& s : kFloats) if (s.inLook) text += std::string(s.key) + "=" + Number(p.*s.field) + ";";
    return text + "hud=" + HudToText(p);
}

bool ApplyLook(const std::string& text, NrParams& p) {
    bool any = false;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find(';', start);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(start, end - start);
        start = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        bool known = false;
        if (key == "hud") { HudFromText(value, p); known = true; }
        else if (key == "useFlow") { p.useFlow = atof(value.c_str()) != 0.0; known = true; }
        for (const FloatSetting& s : kFloats) if (s.inLook && key == s.key) { p.*s.field = Limit(static_cast<float>(atof(value.c_str())), s); known = true; }
        for (const UIntSetting& s : kUInts) if (s.inLook && key == s.key) { p.*s.field = Limit(static_cast<long long>(atof(value.c_str())), s); known = true; }
        any |= known;
    }
    return any;
}

std::string CleanName(std::string name) {
    for (char& c : name) if (c == '|' || c == ';' || c == '=') c = '-';
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() > 40) name.resize(40);
    return name;
}

NrParams ProductDefaults() {
    NrParams p;
    // the upscalers stand in for NIS, which sharpens: without it DLSS or FSR looks softer next to NIS (0.5 = strength 0.8, where it looked
    // right in Fallout: New Vegas at 1440p -> 4K)
    if (kScalerAddon) p.sharpen = 0.5f;
    return p;
}

Loaded LoadSettings(IHost* host, const char* id) {
    auto text = [&](const std::string& key, const char* dflt = "") { return std::string(host ? host->GetConfig(id, key.c_str(), dflt) : dflt); };
    auto number = [&](const char* key, double dflt) { const std::string s = text(key); return s.empty() ? dflt : atof(s.c_str()); };
    auto flag = [&](const char* key, bool dflt) { return number(key, dflt ? 1 : 0) != 0.0; };
    auto integer = [&](const char* key, int dflt) { return static_cast<int>(number(key, dflt)); };

    Loaded out;
    Config& c = out.config;
    const NrParams defaults = ProductDefaults();
    for (const FloatSetting& s : kFloats) c.p.*s.field = Limit(static_cast<float>(number(s.key, defaults.*s.field)), s);
    for (const UIntSetting& s : kUInts) c.p.*s.field = Limit(static_cast<long long>(number(s.key, defaults.*s.field)), s);
    c.p.useFlow = flag("useFlow", true);
    HudFromText(text("hud"), c.p);
    // A sharpening saved before the slider's scale (kScalerSharpenScale): the same strength on the new scale
    // (written back at once: a Lossless Scaling that ends without shutting the addon down must not have it converted a second time)
    if (kScalerAddon && text("sharpenScale").empty() && !text("sharpen").empty()) {
        c.p.sharpen = c.p.sharpen / kScalerSharpenScale;
        if (host) { host->SetConfig(id, "sharpen", std::to_string(c.p.sharpen).c_str()); host->SetConfig(id, "sharpenScale", "1.6"); host->SaveConfig(); }
    }

    c.enabled = flag("enabled", true);
    c.model = std::clamp(integer("model", 0), 0, 1);
    c.dlaaPreset = static_cast<unsigned>(std::clamp(integer("dlaaPreset", 16), 0, 16));
    c.scalerHandoff = std::clamp(integer("scalerHandoff", 0), 0, 3);
    c.motionSource = std::clamp(integer("motionSource", 0), 0, 2);
    c.scalerGpuWait = flag("scalerGpuWait", true);
    c.frameGen = flag("frameGen", false);
    c.scalerFastMotion = flag("scalerFastMotion", true);
    c.motionShapes = flag("motionShapes", false);
    c.frameGenGuard = flag("frameGenGuard", true);
    c.scalerStability = std::clamp(static_cast<float>(number("scalerStability", 0.0)), 0.0f, 1.0f);
    c.scalerEdges = std::clamp(static_cast<float>(number("scalerEdges", 0.0)), 0.0f, 1.0f);
    c.scalerLeanFrom = std::clamp(static_cast<float>(number("scalerLeanFrom", 0.0)), 0.0f, 1.0f);
    c.lsFirst = flag("lsFirst", true);
    c.freshFlow = flag("freshFlow", true);
    c.presentMode = flag("presentMode", true);
    c.presentWait = flag("presentWait", false);
    c.frameEncoding = std::clamp(integer("frameEncoding", 0), 0, 2);
    c.hotkeys = flag("hotkeys", true);
    c.keyAB = integer("keyAB", VK_F7); c.keySplit = integer("keySplit", VK_F8); c.keySharpDn = integer("keySharpDn", VK_F2);
    c.keySharpUp = integer("keySharpUp", VK_F3); c.keyPreset = integer("keyPreset", VK_F10); c.keyShot = integer("keyShot", VK_F4);
    c.keyRecord = integer("keyRecord", VK_F1);
    if (c.keyRecord == VK_F12) c.keyRecord = VK_F5;
    // Ctrl+Shift+F11 was the screenshot key until 0.9.8, but games still see the F11 (the keys are only watched), and F11 toggles full screen
    // in Unreal Engine games and many others: moved to F4 once (keysVersion 2), so a later choice of F11 stays
    if (integer("keysVersion", 1) < 2 && c.keyShot == VK_F11) c.keyShot = VK_F4;
    // 0.9.8's safer defaults (keysVersion 3): the old ones clashed with what games do on an F-key alone (F5 and F9 quicksave and quickload in
    // Bethesda's and many other games, F6 quicksave in Source games). A key still on its old default moves; one chosen by hand stays.
    if (integer("keysVersion", 1) < 3) {
        if (c.keyAB == VK_F6) c.keyAB = VK_F7;
        if (c.keySplit == VK_F7) c.keySplit = VK_F8;
        if (c.keySharpDn == VK_F8) c.keySharpDn = VK_F2;
        if (c.keySharpUp == VK_F9) c.keySharpUp = VK_F3;
        if (c.keyRecord == VK_F5) c.keyRecord = VK_F1;
    }   // the first default, saved by builds before 0.9.5: Ctrl+Shift+F12 is the manager's own (show its window)
    c.recordOn = flag("recordOn", false);
    c.recordShown = flag("recordShown", false);
    c.recordSeconds = std::clamp(static_cast<float>(number("recordSeconds", 5.0)), 1.0f, 60.0f);
    c.recordBudgetMb = std::clamp(integer("recordBudgetMb", 3072), 256, 65536);
    c.recordFolder = text("recordFolder");
    c.recordSaveAfter = std::max(0, integer("recordSaveAfter", 0));
    c.screenshotFolder = text("screenshotFolder");
    c.autoQuality = flag("autoQuality", false);
    c.autoBudgetMs = std::clamp(static_cast<float>(number("autoBudgetMs", 5.0)), 2.0f, 15.0f);
    c.autoFloor = std::clamp(static_cast<float>(number("autoFloor", 0.25)), 0.25f, 1.0f);
    c.gameAuto = flag("gameAuto", true);
    c.scalerPerGame = flag("scalerPerGame", true);
    for (const std::string& exe : SplitList(text("scalerGameList"))) {
        ScalerProfile p; float sharpen = 0, stability = 0, edges = 0, brightness = 0, contrast = 1, gamma = 1, shadows = 0, highlights = 0, saturation = 1, vibrance = 0;
        unsigned preset = 0; int motion = 0;
        const int read = sscanf_s(text("scalerGame." + exe).c_str(), "%f,%f,%f,%u,%d,%f,%f,%f,%f,%f,%f,%f", &sharpen, &stability, &edges, &preset, &motion,
                                  &brightness, &contrast, &gamma, &shadows, &highlights, &saturation, &vibrance);
        if (read != 5 && read != 8 && read != 12) continue;   // 5 and 8: saved before the picture controls came (the rest stay unchanged)
        p.brightness = std::clamp(brightness, -0.3f, 0.3f); p.contrast = std::clamp(contrast, 0.5f, 1.5f); p.gamma = std::clamp(gamma, 0.5f, 2.0f);
        p.shadows = std::clamp(shadows, -1.0f, 1.0f); p.highlights = std::clamp(highlights, -1.0f, 1.0f);
        p.saturation = std::clamp(saturation, 0.0f, 2.0f); p.vibrance = std::clamp(vibrance, 0.0f, 1.0f);
        p.sharpen = std::clamp(sharpen, 0.0f, 1.0f); p.stability = std::clamp(stability, 0.0f, 1.0f); p.edges = std::clamp(edges, 0.0f, 1.0f);
        p.preset = std::min(preset, 16u); p.motion = std::clamp(motion, 0, 2);
        c.scalerGames.push_back({ exe, p });
    }
    for (const std::string& exe : SplitList(text("gameList"))) {
        const std::string look = text("game." + exe);
        if (!look.empty()) c.games.push_back({ exe, look });
    }
    c.tapMode = integer("tapMode", 0); c.frameSlot = integer("frameSlot", -1);
    c.tickSig = text("tickSig"); c.tapSig = text("tapSig");
    c.watchdogMs = static_cast<float>(number("watchdogMs", 80.0));
    c.snippetPath = text("snippetPath");
    for (const std::string& name : SplitList(text("presetNames"))) {
        const std::string data = text("preset." + name);
        if (!data.empty()) out.looks.push_back({ name, data });
    }
    // the view to start in: for the offline test host only (the panel and the hotkeys change it, nothing saves it)
    out.compareStart = std::clamp(integer("compareStart", 0), 0, 2);
    out.splitStart = std::clamp(static_cast<float>(number("splitStart", 0.5)), 0.05f, 0.95f);
    return out;
}

void SaveSettings(IHost* host, const char* id, const Config& c, const std::vector<Look>& looks) {
    if (!host) return;
    auto put = [&](const std::string& key, const std::string& value) { host->SetConfig(id, key.c_str(), value.c_str()); };
    auto putFlag = [&](const char* key, bool v) { put(key, v ? "1" : "0"); };
    for (const FloatSetting& s : kFloats) put(s.key, Number(c.p.*s.field));
    for (const UIntSetting& s : kUInts) put(s.key, std::to_string(c.p.*s.field));
    putFlag("useFlow", c.p.useFlow);
    if (kScalerAddon) put("sharpenScale", "1.6");   // the slider's scale this value is on (see LoadSettings)
    put("hud", HudToText(c.p));

    put("model", std::to_string(c.model)); put("dlaaPreset", std::to_string(c.dlaaPreset)); put("scalerHandoff", std::to_string(c.scalerHandoff)); put("motionSource", std::to_string(c.motionSource)); putFlag("scalerGpuWait", c.scalerGpuWait); putFlag("frameGen", c.frameGen); putFlag("scalerFastMotion", c.scalerFastMotion); putFlag("motionShapes", c.motionShapes); putFlag("frameGenGuard", c.frameGenGuard); put("scalerStability", Number(c.scalerStability)); put("scalerEdges", Number(c.scalerEdges)); put("scalerLeanFrom", Number(c.scalerLeanFrom));
    putFlag("enabled", c.enabled); putFlag("lsFirst", c.lsFirst); putFlag("freshFlow", c.freshFlow); putFlag("presentMode", c.presentMode); putFlag("presentWait", c.presentWait); putFlag("hotkeys", c.hotkeys);
    put("frameEncoding", std::to_string(c.frameEncoding));
    put("keyAB", std::to_string(c.keyAB)); put("keySplit", std::to_string(c.keySplit)); put("keySharpDn", std::to_string(c.keySharpDn));
    put("keySharpUp", std::to_string(c.keySharpUp)); put("keyPreset", std::to_string(c.keyPreset)); put("keyShot", std::to_string(c.keyShot));
    put("keyRecord", std::to_string(c.keyRecord)); put("keysVersion", "3");
    putFlag("recordOn", c.recordOn); putFlag("recordShown", c.recordShown); put("recordSeconds", Number(c.recordSeconds)); put("recordBudgetMb", std::to_string(c.recordBudgetMb));
    put("recordFolder", c.recordFolder);
    // the recorder is one setting for all our addons: the others' configs get it too (each addon follows its own, see FollowSharedRecorder), so it can
    // be switched on in any panel and the addon that works on the frames records
    for (const char* other : { "DLSS5NR01", "DLSS4DLAA", "FSR3UPSC", "XESSUPSC" }) {
        if (std::string(other) == id) continue;
        host->SetConfig(other, "recordOn", c.recordOn ? "1" : "0"); host->SetConfig(other, "recordSeconds", Number(c.recordSeconds).c_str());
        host->SetConfig(other, "recordBudgetMb", std::to_string(c.recordBudgetMb).c_str()); host->SetConfig(other, "recordFolder", c.recordFolder.c_str());
    }
    put("screenshotFolder", c.screenshotFolder);
    putFlag("autoQuality", c.autoQuality); put("autoBudgetMs", Number(c.autoBudgetMs)); put("autoFloor", Number(c.autoFloor));
    putFlag("gameAuto", c.gameAuto);
    putFlag("scalerPerGame", c.scalerPerGame);
    {
        std::vector<std::string> scalerExes;
        for (const auto& [exe, p] : c.scalerGames) {
            char v[224]; snprintf(v, sizeof v, "%g,%g,%g,%u,%d,%g,%g,%g,%g,%g,%g,%g", p.sharpen, p.stability, p.edges, p.preset, p.motion, p.brightness, p.contrast,
                                  p.gamma, p.shadows, p.highlights, p.saturation, p.vibrance);
            scalerExes.push_back(exe); put("scalerGame." + exe, v);
        }
        put("scalerGameList", JoinList(scalerExes));
    }
    std::vector<std::string> exes;
    for (const auto& [exe, look] : c.games) { exes.push_back(exe); put("game." + exe, look); }
    put("gameList", JoinList(exes));
    put("tapMode", std::to_string(c.tapMode)); put("frameSlot", std::to_string(c.frameSlot));
    put("tickSig", c.tickSig); put("tapSig", c.tapSig);
    put("watchdogMs", Number(c.watchdogMs)); put("snippetPath", c.snippetPath);
    std::vector<std::string> names;
    for (const Look& look : looks) { names.push_back(look.name); put("preset." + look.name, look.data); }
    put("presetNames", JoinList(names));
    host->SaveConfig();
}

void ForgetLook(IHost* host, const char* id, const std::string& name) { if (host) host->SetConfig(id, ("preset." + name).c_str(), ""); }
void ForgetGame(IHost* host, const char* id, const std::string& exe) { if (host) host->SetConfig(id, ("game." + exe).c_str(), ""); }

ScalerProfile ProfileOf(const Config& c) {
    ScalerProfile p; p.sharpen = c.p.sharpen; p.stability = c.scalerStability; p.edges = c.scalerEdges; p.preset = c.dlaaPreset; p.motion = c.motionSource;
    p.brightness = c.p.brightness; p.contrast = c.p.contrast; p.gamma = c.p.gamma;
    p.shadows = c.p.shadows; p.highlights = c.p.highlights; p.saturation = c.p.saturation; p.vibrance = c.p.vibrance;
    return p;
}

void ApplyProfile(Config& c, const ScalerProfile& p) {
    c.p.sharpen = p.sharpen; c.scalerStability = p.stability; c.scalerEdges = p.edges; c.dlaaPreset = p.preset; c.motionSource = p.motion;
    c.p.brightness = p.brightness; c.p.contrast = p.contrast; c.p.gamma = p.gamma;
    c.p.shadows = p.shadows; c.p.highlights = p.highlights; c.p.saturation = p.saturation; c.p.vibrance = p.vibrance;
}

void KeepForGame(Config& c, const std::string& exe) {
    if (exe.empty()) return;
    for (auto& [e, p] : c.scalerGames) if (e == exe) { p = ProfileOf(c); return; }
    c.scalerGames.push_back({ exe, ProfileOf(c) });
}

} // namespace nr

#include "addon_discovery.h"
#include "addon_manifest.h"
#include "../log/logger.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <system_error>
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace eam {

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), bytes, nullptr, nullptr);
    return out;
}

namespace {

bool HasExtension(const fs::path& p, const wchar_t* wanted) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return ext == wanted;
}

bool IsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// The first file in `folder` with the given extension, in the order the file system lists them. Empty when there is none.
fs::path FirstWithExtension(const fs::path& folder, const wchar_t* ext) {
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && HasExtension(it->path(), ext)) return it->path();
    return {};
}

fs::path PickDll(const fs::path& folder, const AddonManifest& manifest) {
    if (!manifest.dll.empty()) {
        const fs::path named = folder / manifest.dll;
        if (IsFile(named)) return named;
    }
    const fs::path byFolderName = folder / (folder.filename().wstring() + L".dll");
    if (IsFile(byFolderName)) return byFolderName;
    return FirstWithExtension(folder, L".dll");
}

fs::path PickIcon(const fs::path& folder, const AddonManifest& manifest) {
    if (!manifest.icon.empty()) {
        const fs::path named = folder / manifest.icon;
        if (IsFile(named)) return named;
    }
    for (const wchar_t* name : { L"icon.svg", L"icon.png", L"icon.jpg", L"icon.jpeg", L"icon.bmp" }) {
        const fs::path guess = folder / name;
        if (IsFile(guess)) return guess;
    }
    return {};
}

} // namespace

namespace {

// The value of an attribute in a tag's text (name="value" or name='value', spaces around the = allowed); false when it has none.
bool SvgAttribute(const std::string& tag, const char* name, std::string& out) {
    const size_t n = strlen(name);
    for (size_t at = tag.find(name); at != std::string::npos; at = tag.find(name, at + 1)) {
        if (at == 0 || !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;   // (not the end of another name)
        size_t i = at + n;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || tag[i] != '=') continue;
        ++i;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) continue;
        const size_t close = tag.find(tag[i], i + 1);
        if (close == std::string::npos) return false;
        out = tag.substr(i + 1, close - i - 1);
        return true;
    }
    return false;
}

float SvgNumber(const std::string& tag, const char* name, float fallback = 0.0f) {
    std::string v;
    return SvgAttribute(tag, name, v) && !v.empty() ? static_cast<float>(atof(v.c_str())) : fallback;
}

std::string Num(float v) { char t[32]; snprintf(t, sizeof t, "%g", v); return t; }

// The basic shapes as path data (the manager draws paths only): a rect (with rounded corners), circle, ellipse, line, polyline and polygon.
std::string SvgShapeAsPath(const std::string& name, const std::string& tag) {
    std::string d;
    if (name == "rect") {
        const float x = SvgNumber(tag, "x"), y = SvgNumber(tag, "y"), w = SvgNumber(tag, "width"), h = SvgNumber(tag, "height");
        if (w <= 0 || h <= 0) return {};
        float rx = SvgNumber(tag, "rx", -1.0f), ry = SvgNumber(tag, "ry", -1.0f);
        if (rx < 0 && ry < 0) rx = ry = 0; else if (rx < 0) rx = ry; else if (ry < 0) ry = rx;
        rx = (std::min)(rx, w / 2); ry = (std::min)(ry, h / 2);
        if (rx <= 0 || ry <= 0) return "M" + Num(x) + " " + Num(y) + "h" + Num(w) + "v" + Num(h) + "h" + Num(-w) + "Z";
        const std::string a = "a" + Num(rx) + " " + Num(ry) + " 0 0 1 ";
        return "M" + Num(x + rx) + " " + Num(y) + "h" + Num(w - 2 * rx) + a + Num(rx) + " " + Num(ry) + "v" + Num(h - 2 * ry) + a + Num(-rx) + " " + Num(ry) +
               "h" + Num(-(w - 2 * rx)) + a + Num(-rx) + " " + Num(-ry) + "v" + Num(-(h - 2 * ry)) + a + Num(rx) + " " + Num(-ry) + "Z";
    }
    if (name == "circle" || name == "ellipse") {
        const float cx = SvgNumber(tag, "cx"), cy = SvgNumber(tag, "cy");
        const float rx = name == "circle" ? SvgNumber(tag, "r") : SvgNumber(tag, "rx"), ry = name == "circle" ? rx : SvgNumber(tag, "ry");
        if (rx <= 0 || ry <= 0) return {};
        const std::string a = "a" + Num(rx) + " " + Num(ry) + " 0 1 0 ";
        return "M" + Num(cx - rx) + " " + Num(cy) + a + Num(2 * rx) + " 0" + a + Num(-2 * rx) + " 0Z";
    }
    if (name == "line") return "M" + Num(SvgNumber(tag, "x1")) + " " + Num(SvgNumber(tag, "y1")) + "L" + Num(SvgNumber(tag, "x2")) + " " + Num(SvgNumber(tag, "y2"));
    if (name == "polyline" || name == "polygon") {
        std::string points;
        if (!SvgAttribute(tag, "points", points)) return {};
        std::vector<float> v;
        for (const char* p = points.c_str(); *p;) {
            char* after = nullptr; const float f = strtof(p, &after);
            if (after == p) { ++p; continue; }
            v.push_back(f); p = after;
        }
        if (v.size() < 4) return {};
        for (size_t i = 0; i + 1 < v.size(); i += 2) d += (i ? "L" : "M") + Num(v[i]) + " " + Num(v[i + 1]);
        return name == "polygon" ? d + "Z" : d;
    }
    return {};
}

} // namespace

// A vector icon: the d="..." of every path in the file (in order; a rect, circle, ellipse, line, polyline or polygon as the path it makes), and the width of its viewBox (24 when it has none). Colours and other
// attributes are left out: the manager draws the shapes in its own colours, like its built-in icons.
bool ReadSvgIcon(const fs::path& file, std::vector<std::string>& paths, float& view) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer; buffer << in.rdbuf();
    const std::string text = buffer.str();
    if (text.size() > 64 * 1024) return false;   // an icon, not a picture
    view = 24.0f;
    if (const size_t vb = text.find("viewBox=\""); vb != std::string::npos) {
        float x = 0, y = 0, w = 0, h = 0;
        if (sscanf_s(text.c_str() + vb + 9, "%f %f %f %f", &x, &y, &w, &h) == 4 && w > 0) view = w;
    }
    for (size_t at = text.find('<'); at != std::string::npos; at = text.find('<', at + 1)) {
        const size_t end = text.find('>', at);
        if (end == std::string::npos) break;
        const std::string tag = text.substr(at + 1, end - at - 1);   // "path d=... /", "rect x=...", ...
        size_t nameEnd = 0; while (nameEnd < tag.size() && !std::isspace(static_cast<unsigned char>(tag[nameEnd])) && tag[nameEnd] != '/') ++nameEnd;
        const std::string name = tag.substr(0, nameEnd);
        std::string d;
        if (name == "path") { if (!SvgAttribute(tag, "d", d) || d.empty()) continue; }
        else d = SvgShapeAsPath(name, tag);
        if (!d.empty()) paths.push_back(d);
    }
    return !paths.empty();
}

bool DiscoverAddon(const fs::path& folder, AddonInfo& info) {
    info.folderName = folder.filename().wstring();
    info.id = WideToUtf8(info.folderName);

    const fs::path manifestFile = folder / "addon.json";
    if (IsFile(manifestFile)) {
        std::string problem;
        if (!ReadManifest(manifestFile, info.manifest, &problem))
            LOG_WARN("AddonManager", "Ignoring addon.json of '%s': %s", info.id.c_str(), problem.c_str());
    }

    const fs::path dll = PickDll(folder, info.manifest);
    if (dll.empty()) return false;
    info.dllPath = dll.wstring();

    const fs::path ini = FirstWithExtension(folder, L".ini");
    if (!ini.empty()) info.configPath = ini.wstring();

    const fs::path icon = PickIcon(folder, info.manifest);
    if (!icon.empty() && _wcsicmp(icon.extension().c_str(), L".svg") == 0) {
        if (!ReadSvgIcon(icon, info.iconSvg, info.iconSvgView)) LOG_WARN("AddonManager", "The icon of '%s' has no shapes the manager can draw", info.id.c_str());
    } else if (!icon.empty()) {
        info.iconPath = icon.wstring();
    }
    return true;
}

} // namespace eam

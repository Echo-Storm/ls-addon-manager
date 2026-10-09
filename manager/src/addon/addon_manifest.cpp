#include "addon_manifest.h"
#include "../../third_party/nlohmann/json.hpp"
#include <cstdlib>
#include <fstream>

namespace eam {

namespace {

using Json = nlohmann::json;

void TakeText(const Json& obj, const char* key, std::string& into) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_string()) into = it->get<std::string>();
}

void TakeList(const Json& obj, const char* key, std::vector<std::string>& into) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return;
    for (const Json& item : *it)
        if (item.is_string()) into.push_back(item.get<std::string>());
}

} // namespace

bool ReadManifest(const std::filesystem::path& file, AddonManifest& out, std::string* problem) {
    auto fail = [&](const char* why) { if (problem) *problem = why; return false; };

    std::ifstream in(file);
    if (!in) return fail("the file could not be opened");
    const Json doc = Json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return fail("it is not valid JSON");
    if (!doc.is_object()) return fail("it is not a JSON object");

    TakeText(doc, "name", out.name);
    TakeText(doc, "version", out.version);
    TakeText(doc, "author", out.author);
    TakeText(doc, "description", out.description);
    TakeText(doc, "min_host_version", out.minHostVersion);
    TakeText(doc, "dll", out.dll);
    TakeText(doc, "icon", out.icon);
    TakeList(doc, "dependencies", out.dependencies);
    TakeList(doc, "renamed_from", out.renamedFrom);
    TakeList(doc, "conflicts", out.conflicts);
    if (const auto it = doc.find("wip"); it != doc.end() && it->is_boolean()) out.wip = it->get<bool>();
    if (const auto it = doc.find("enabled_by_default"); it != doc.end() && it->is_boolean()) out.enabledByDefault = it->get<bool>();
    TakeList(doc, "tags", out.tags);
    if (const auto it = doc.find("runtimes"); it != doc.end() && it->is_array())
        for (const Json& item : *it) {
            if (!item.is_object()) continue;
            AddonManifest::Runtime r;
            TakeText(item, "name", r.name); TakeText(item, "file", r.file); TakeText(item, "config_key", r.configKey);
            TakeText(item, "shipped_sha256", r.shippedSha256); TakeText(item, "shipped_label", r.shippedLabel);
            TakeList(item, "exports", r.exports);
            if (!r.name.empty() && !r.file.empty()) out.runtimes.push_back(r);
        }
    out.parsed = true;
    return true;
}

uint32_t ParseApiVersion(const std::string& text) {
    uint32_t part[3] = { 0, 0, 0 };
    int have = 0;
    const char* at = text.c_str();
    while (have < 3) {
        char* after = nullptr;
        const unsigned long v = std::strtoul(at, &after, 10);
        if (after == at) break;
        part[have++] = (uint32_t)v;
        at = after;
        if (*at != '.') break;
        ++at;
    }
    if (have < 2) return 0;
    return (std::min<uint32_t>(part[0], 0xFFFF) << 16) | (std::min<uint32_t>(part[1], 0xFF) << 8) | std::min<uint32_t>(part[2], 0xFF);   // (clamped, not wrapped: 1.256.0 is not 1.0.0)
}

} // namespace eam

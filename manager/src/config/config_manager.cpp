#include "config_manager.h"
#include "../log/logger.h"
#include <filesystem>
#include <fstream>
#include <system_error>
#include <windows.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace eam {

namespace {

constexpr const char* kOwnFlag = "_enabled";      // the host's on/off flag for an addon
constexpr const char* kOldFlag = "enabled";       // where older versions kept it

json EmptySettings() { return json{ { "global", json::object() }, { "addons", json::object() } }; }

// addons/<id> when it exists and is an object.
const json* AddonNode(const json& all, const std::string& id) {
    const auto addons = all.find("addons");
    if (addons == all.end() || !addons->is_object()) return nullptr;
    const auto node = addons->find(id);
    return node != addons->end() && node->is_object() ? &*node : nullptr;
}

// The object an addon's settings live in, made so when the file had something else there ("addons": [..], or an addon's entry that is text).
// nlohmann's operator[] throws on a value that is not an object, and SetConfig is called from inside addon code.
json& AddonObject(json& all, const std::string& id) {
    json& addons = all["addons"];
    if (!addons.is_object()) addons = json::object();
    json& addon = addons[id];
    if (!addon.is_object()) addon = json::object();
    return addon;
}

// Text an addon stored may not be valid UTF-8 (a path in the ANSI code page): replace such bytes instead of throwing, which dump() does by default.
std::string Dump(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), bytes, nullptr, nullptr);
    return out;
}

// Writes `text` to a temporary file beside `path`, then swaps it in. Returns false, after logging why, when that did not work.
bool WriteAtomically(const std::wstring& path, const std::string& text) {
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            LOG_ERROR("Config", "Could not write %s", Utf8(temp).c_str());
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_ERROR("Config", "Could not replace config.json (error %lu)", GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

} // namespace

ConfigManager& ConfigManager::Instance() {
    static ConfigManager instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------------------------------------------------------------

void ConfigManager::Load(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = path;
    m_basePath = fs::path(path).parent_path().wstring();
    m_lastWritten.clear();   // nothing is known about the disk yet
    m_frozen = false;        // a fresh start: an import before it has been applied
    m_frozenNoted = false;
    m_data = EmptySettings();

    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::ifstream in(path);
        for (int tries = 0; !in.is_open() && tries < 5; ++tries) { Sleep(100); in.clear(); in.open(path); }   // (a virus scanner or an editor may hold it for a moment)
        if (!in.is_open()) {
            // not "corrupt": the file is there and we cannot read it. Nothing is written over it this session (Save needs a loaded config).
            LOG_ERROR("Config", "config.json exists but cannot be opened (another program has it?): the settings of this session are not saved, and the file is left as it is");
            return;
        }
        json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_object()) {
            m_data = std::move(parsed);
            m_loaded = true;
            // A hand-edited file may hold something other than an object in the two sections the manager writes to: keep a copy, start those afresh
            bool reshaped = false;
            for (const char* section : { "addons", "global" }) {
                const auto it = m_data.find(section);
                if (it != m_data.end() && !it->is_object()) { *it = json::object(); reshaped = true; }
            }
            if (reshaped) {
                CopyFileW(path.c_str(), (path + L".corrupt").c_str(), FALSE);
                LOG_WARN("Config", "config.json had 'addons' or 'global' in an unexpected form; kept a copy as config.json.corrupt and started those afresh");
            }
            LOG_INFO("Config", "Loaded config from config.json");
            return;
        }
        // Somebody may want to repair this file, and the next Save would replace it: keep a copy.
        CopyFileW(path.c_str(), (path + L".corrupt").c_str(), FALSE);
        LOG_ERROR("Config", "config.json is %s; kept a copy as config.json.corrupt and started fresh",
                  parsed.is_discarded() ? "not valid JSON" : "not a JSON object");
    }

    MigrateFromIni();
    m_loaded = true;
}

void ConfigManager::MigrateFromIni() {
    const fs::path ini = fs::path(m_basePath) / "addons_config.ini";
    std::error_code ec;
    if (!fs::exists(ini, ec)) return;

    LOG_INFO("Config", "Migrating from addons_config.ini...");
    // The old file kept one switch per addon under [Addons]; the addon folders next to it say which addons there are.
    for (fs::directory_iterator it(m_basePath, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::wstring name = it->path().filename().wstring();
        const bool on = GetPrivateProfileIntW(L"Addons", name.c_str(), 1, ini.c_str()) != 0;
        AddonObject(m_data, Utf8(name))[kOwnFlag] = on;
    }

    const std::string text = Dump(m_data);
    if (WriteAtomically(m_path, text)) {
        m_lastWritten = text;
        LOG_INFO("Config", "Migration complete, saved config.json");
    }
}

void ConfigManager::Save() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_loaded || m_path.empty()) {
        LOG_WARN("Config", "Save called before config was loaded, ignoring");
        return;
    }
    std::string text = Dump(m_data);
    if (text.size() <= 4) {   // "{}" or "null": never replace the user's file with nothing
        LOG_WARN("Config", "Save aborted: config data is empty");
        return;
    }
    if (text == m_lastWritten) return;
    if (WriteAtomically(m_path, text)) m_lastWritten = std::move(text);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Addon settings
// ---------------------------------------------------------------------------------------------------------------------------------

std::string ConfigManager::Get(const std::string& addonId, const std::string& key, const std::string& defaultVal) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const json* addon = AddonNode(m_data, addonId);
    if (!addon) return defaultVal;
    const auto value = addon->find(key);
    if (value == addon->end()) return defaultVal;
    if (value->is_string()) return value->get<std::string>();
    if (value->is_boolean()) return value->get<bool>() ? "1" : "0";
    if (value->is_number()) return value->dump();
    return defaultVal;
}

bool ConfigManager::Frozen() {
    if (!m_frozen) return false;
    if (!m_frozenNoted) { m_frozenNoted = true; LOG_INFO("Config", "Settings were loaded from a file: changes wait until Lossless Scaling restarts"); }
    return true;
}

bool ConfigManager::RestartPending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frozen;
}

void ConfigManager::Set(const std::string& addonId, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Frozen()) return;
    AddonObject(m_data, addonId)[key] = value;
}

bool ConfigManager::IsAddonEnabled(const std::string& addonId, bool defaultVal) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const json* addon = AddonNode(m_data, addonId);
    if (!addon) return defaultVal;
    for (const char* flag : { kOwnFlag, kOldFlag }) {   // the old place counts only if it holds a boolean; text is the addon's own
        const auto value = addon->find(flag);
        if (value != addon->end() && value->is_boolean()) return value->get<bool>();
    }
    return defaultVal;
}

void ConfigManager::SetAddonEnabled(const std::string& addonId, bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Frozen()) return;
    json& addon = AddonObject(m_data, addonId);
    addon[kOwnFlag] = enabled;
    const auto old = addon.find(kOldFlag);
    if (old != addon.end() && old->is_boolean()) addon.erase(old);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Everything at once, and the host's own settings
// ---------------------------------------------------------------------------------------------------------------------------------

bool ConfigManager::RenameAddonSection(const std::string& from, const std::string& to) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto addons = m_data.find("addons");
    if (from == to || addons == m_data.end() || !addons->is_object()) return false;
    const auto old = addons->find(from);
    if (old == addons->end() || !old->is_object()) return false;
    const auto fresh = addons->find(to);
    if (fresh != addons->end() && fresh->is_object() && !fresh->empty()) return false;   // the new name already has its own settings
    (*addons)[to] = std::move(*old);
    addons->erase(from);
    return true;
}

json ConfigManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data;
}

void ConfigManager::Replace(const json& all, bool untilRestart) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = all.is_object() ? all : json::object();
        m_frozen = untilRestart;
        m_frozenNoted = false;
    }
    Save();
}

json ConfigManager::GlobalGet(const char* section, const char* key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto global = m_data.find("global");
    if (global == m_data.end() || !global->is_object()) return nullptr;

    const json* where = &*global;
    if (section) {
        const auto inner = where->find(section);
        if (inner == where->end() || !inner->is_object()) return nullptr;
        where = &*inner;
    }
    const auto value = where->find(key);
    return value == where->end() ? json(nullptr) : *value;
}

void ConfigManager::GlobalSet(const char* section, const char* key, json value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Frozen()) return;
    json& global = m_data["global"];
    if (!global.is_object()) global = json::object();

    json* where = &global;
    if (section) {
        json& inner = global[section];
        if (!inner.is_object()) inner = json::object();
        where = &inner;
    }
    (*where)[key] = std::move(value);
}

} // namespace eam

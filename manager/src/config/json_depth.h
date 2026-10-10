// How deeply a piece of JSON text nests, found without parsing it (so without recursing): the manager's settings are written back with a recursive dump, and a
// file of tens of thousands of '[' (damaged, or made to be) would overflow the stack there. Real settings nest three or four deep.
#pragma once
#include <cstddef>
#include <string>

namespace eam {

// True when `text` nests objects and arrays deeper than `limit` (brackets inside strings do not count).
inline bool JsonNestsDeeperThan(const std::string& text, int limit = 64) {
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '[' || c == '{') { if (++depth > limit) return true; }
        else if ((c == ']' || c == '}') && depth > 0) --depth;
    }
    return false;
}

} // namespace eam

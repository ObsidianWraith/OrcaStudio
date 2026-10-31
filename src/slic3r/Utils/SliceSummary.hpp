#pragma once
#include <istream>
#include <map>
#include <string>

namespace Slic3r::SliceSummary {
inline std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
// Read metadata only, never interpret or execute machine commands. Bound retained values.
inline std::map<std::string, std::string> read(std::istream& input) {
    std::map<std::string, std::string> result;
    std::string line;
    bool config = false;
    while (std::getline(input, line)) {
        if (line.size() > 16384 || line.empty() || line[0] != ';') continue;
        line = trim(line.substr(1));
        if (line == "CONFIG_BLOCK_START" || line == "prusaslicer_config = begin") { config = true; continue; }
        if (line == "CONFIG_BLOCK_END" || line == "prusaslicer_config = end") { config = false; continue; }
        auto split = line.find('=');
        if (split == std::string::npos) split = line.find(':');
        if (split == std::string::npos) continue;
        auto key = trim(line.substr(0, split));
        bool stat = key.find("estimated printing time") == 0 || key.find("total estimated time") == 0
            || key.find("filament used") == 0 || key.find("total filament") == 0
            || key == "total layer number";
        if ((config || stat) && key.size() <= 200 && result.size() < 3000)
            result[key] = trim(line.substr(split + 1));
    }
    return result;
}
inline std::string compare(const std::map<std::string, std::string>& a,
                           const std::map<std::string, std::string>& b) {
    std::map<std::string, int> keys;
    for (auto& item : a) keys[item.first] = 0;
    for (auto& item : b) keys[item.first] = 0;
    std::string out;
    for (auto& item : keys) {
        auto ai = a.find(item.first), bi = b.find(item.first);
        auto av = ai == a.end() ? "[not recorded]" : ai->second;
        auto bv = bi == b.end() ? "[not recorded]" : bi->second;
        if (av != bv) out += item.first + "\n  A: " + av + "\n  B: " + bv + "\n\n";
    }
    return out.empty() ? "No differences in recorded settings and estimates. Toolpaths may still differ.\n" : out;
}
}

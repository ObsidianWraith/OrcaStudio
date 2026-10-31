#pragma once
// OrcaStudio revision 003: local successful-send archive. No GUI/thread-affine state.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r::WorkflowArchive {
namespace fs = std::filesystem;
inline std::mutex archive_mutex;
inline bool enabled(const fs::path& root) { return fs::exists(root / "enabled"); }
inline void set_enabled(const fs::path& root, bool value) {
    std::lock_guard<std::mutex> guard(archive_mutex);
    fs::create_directories(root);
    if (value) { std::ofstream out(root / "enabled"); if (!out) throw std::runtime_error("Cannot enable archive"); }
    else fs::remove(root / "enabled");
}
// Return false on storage failure; an archive failure must not fail a completed upload.
inline bool save(const fs::path& root, const fs::path& source, const std::string& printer,
                 const std::string& title) noexcept {
    try {
        std::lock_guard<std::mutex> guard(archive_mutex);
        if (!enabled(root) || !fs::is_regular_file(source)) return false;
        auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        fs::path dir;
        do { dir = root / ("job-" + std::to_string(stamp++)); } while (!fs::create_directory(dir));
        try {
            // Fixed basename prevents user/project names becoming paths.
            auto file = dir / ("print" + source.extension().u8string());
            fs::copy_file(source, file);
            nlohmann::json meta = {{"schema", 1}, {"status", "sent"}, {"printer", printer},
                {"title", title}, {"file", file.filename().u8string()}, {"notes", ""},
                {"sent_unix_seconds", (stamp - 1) / 1000000}, {"bytes", fs::file_size(file)}};
            std::ofstream out(dir / "record.json"); out << meta.dump(2); out.close();
            if (!out) throw std::runtime_error("Cannot write archive metadata");
        } catch (...) { fs::remove_all(dir); throw; }
        std::vector<fs::path> records;
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.is_directory() && entry.path().filename().u8string().rfind("job-", 0) == 0
                && fs::exists(entry.path() / "record.json")) records.push_back(entry.path());
        std::sort(records.begin(), records.end());
        while (records.size() > 100) { fs::remove_all(records.front()); records.erase(records.begin()); }
        return true;
    } catch (...) { return false; }
}
}

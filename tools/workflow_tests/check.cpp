// Small dependency-free runner for the archive and G-code metadata reader.
#include <cassert>
#include <sstream>
#include <iostream>
#include "src/slic3r/Utils/SliceSummary.hpp"
#include "src/slic3r/Utils/WorkflowArchive.hpp"
int main() {
    using namespace Slic3r;
    std::istringstream a("; estimated printing time (normal mode) = 1h 2m\n; CONFIG_BLOCK_START\r\n; support_top_z_distance = 0.08\n; notes = a=b\n; CONFIG_BLOCK_END\n; G1 = not a setting\nG1 X2\n");
    auto first = SliceSummary::read(a);
    assert(first.at("support_top_z_distance") == "0.08");
    assert(first.at("notes") == "a=b");
    assert(first.count("G1") == 0);
    auto second = first; second["support_top_z_distance"] = "0.2";
    auto diff = SliceSummary::compare(first, second);
    assert(diff.find("0.08") != std::string::npos && diff.find("0.2") != std::string::npos);
    assert(diff.find("estimated printing time") == std::string::npos);
    second.erase("notes"); assert(SliceSummary::compare(first, second).find("[not recorded]") != std::string::npos);
    std::istringstream missing("G1 X1 E2\n; random comment\n"); assert(SliceSummary::read(missing).empty());
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / ("orca-workflow-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{dir};
    auto source = dir / "source.gcode"; std::ofstream(source) << "G1 X1 E2\n";
    auto root = dir / "archive";
    assert(!WorkflowArchive::save(root, source, "printer", "test"));
    WorkflowArchive::set_enabled(root, true);
    assert(!WorkflowArchive::save(root, dir / "missing", "printer", "test"));
    assert(WorkflowArchive::save(root, source, "printer", "../../test"));
    for (auto& item : fs::directory_iterator(root)) if (item.is_directory()) {
        std::ifstream in(item.path() / "record.json"); nlohmann::json doc; in >> doc;
        assert(doc["status"] == "sent"); assert(doc["title"] == "../../test");
        std::ifstream data(item.path() / "print.gcode"); std::string line; std::getline(data, line); assert(line == "G1 X1 E2");
    }
    for (int i=0; i<102; ++i) assert(WorkflowArchive::save(root, source, "printer", "test"));
    size_t count=0; for(auto& item : fs::directory_iterator(root)) if(item.is_directory()) ++count;
    assert(count == 100); assert(fs::exists(source));
    WorkflowArchive::set_enabled(root, false); assert(!WorkflowArchive::save(root, source, "printer", "test"));
    std::cout << "Archive and slice-summary checks passed\n";
}

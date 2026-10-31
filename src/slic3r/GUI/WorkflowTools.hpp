#pragma once
// OrcaStudio revision 003. Original, contained workflow tools inspired by EdgeSlicer.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <wx/filedlg.h>
#include <wx/datetime.h>
#include <wx/textdlg.h>
#include <wx/choicdlg.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>
#include <wx/filename.h>
#include "GUI_App.hpp"
#include "Tab.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "../Utils/WorkflowArchive.hpp"
#include "../Utils/SliceSummary.hpp"

namespace Slic3r::GUI::WorkflowTools {
namespace fs = std::filesystem;
inline fs::path archive_root() { return fs::u8path(data_dir()) / "print_archive"; }
inline wxString wxpath(const fs::path& p) { return wxString::FromUTF8(p.u8string()); }
inline bool support_key(const std::string& key) {
    // Filament slots belong to the current plate; do not transfer stale slot numbers.
    if (key == "support_filament" || key == "support_interface_filament") return false;
    return key.rfind("support_", 0) == 0 || key.rfind("tree_support_", 0) == 0 || key.rfind("raft_", 0) == 0
        || key == "enable_support" || key == "independent_support_layer_height"
        || key == "bridge_no_support" || key == "max_bridge_length";
}
inline void report(wxWindow* parent, const wxString& title, const std::string& text) {
    wxDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxSize(820, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* view = new wxTextCtrl(&dialog, wxID_ANY, wxString::FromUTF8(text), wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
    box->Add(view, 1, wxEXPAND | wxALL, 10);
    box->Add(dialog.CreateButtonSizer(wxOK), 0, wxALIGN_RIGHT | wxALL, 10);
    dialog.SetSizer(box); dialog.ShowModal();
}
inline void recipe(wxWindow* parent, bool save) {
    try {
        auto* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab || !tab->get_config()) return;
        auto folder = fs::u8path(data_dir()) / "support_recipes";
        fs::create_directories(folder);
        wxFileDialog picker(parent, save ? _L("Save support recipe") : _L("Apply support recipe"), wxpath(folder), "",
            "Support recipe (*.json)|*.json", save ? wxFD_SAVE | wxFD_OVERWRITE_PROMPT : wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (picker.ShowModal() != wxID_OK) return;
        auto path = fs::u8path(picker.GetPath().ToUTF8().data());
        if (save) {
            nlohmann::json options = nlohmann::json::object();
            for (const auto& key : tab->get_config()->keys())
                if (support_key(key)) options[key] = tab->get_config()->option(key)->serialize();
            nlohmann::json doc = {{"format", "OrcaStudio.support-recipe"}, {"version", 1}, {"options", options}};
            auto tmp = path; tmp += ".tmp";
            std::ofstream out(tmp); out << doc.dump(2); out.close();
            if (!out) throw std::runtime_error("Could not save recipe");
            // wxRenameFile supports replacing existing files on all supported platforms.
            if (!wxRenameFile(wxpath(tmp), wxpath(path), true)) throw std::runtime_error("Could not replace recipe");
        } else {
            if (fs::file_size(path) > 1024 * 1024) throw std::runtime_error("Recipe exceeds 1 MB");
            std::ifstream in(path); auto doc = nlohmann::json::parse(in);
            if (doc.value("format", "") != "OrcaStudio.support-recipe" || doc.value("version", 0) != 1
                || !doc.at("options").is_object()) throw std::runtime_error("Unsupported recipe format or version");
            DynamicPrintConfig candidate = *tab->get_config();
            std::string changes;
            for (auto it = doc.at("options").begin(); it != doc.at("options").end(); ++it) {
                auto* option = candidate.option(it.key());
                if (!support_key(it.key()) || !option || !it.value().is_string())
                    throw std::runtime_error("Unknown or unsupported setting: " + it.key());
                auto before = option->serialize();
                if (!option->deserialize(it.value().get<std::string>())) throw std::runtime_error("Invalid setting: " + it.key());
                if (before != option->serialize()) changes += it.key() + ": " + before + " -> " + option->serialize() + "\n";
            }
            auto full = wxGetApp().preset_bundle->full_config(); full.apply(candidate);
            auto errors = full.validate();
            if (!errors.empty()) throw std::runtime_error("Settings validation failed: " + errors.begin()->second);
            if (changes.empty()) { wxMessageBox(_L("This recipe already matches the current support settings."), _L("Support recipe"), wxOK, parent); return; }
            report(parent, _L("Support recipe changes"), changes + "\nCurrent support filament assignments will be preserved.");
            if (wxMessageBox(_L("Apply these support settings to the current process?"), _L("Support recipe"), wxYES_NO | wxNO_DEFAULT, parent) == wxYES)
                tab->load_config(candidate);
        }
    } catch (const std::exception& e) { wxMessageBox(wxString::FromUTF8(e.what()), _L("Support recipe"), wxOK | wxICON_ERROR, parent); }
}
inline std::string read_gcode(wxWindow* parent, const fs::path& path) {
    constexpr size_t limit = 256 * 1024 * 1024;
    if (path.extension() != ".3mf") {
        if (fs::file_size(path) > limit) throw std::runtime_error("Comparison supports files up to 256 MB");
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read G-code");
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    mz_zip_archive zip{};
    if (!open_zip_reader(&zip, path.u8string())) throw std::runtime_error("Cannot open 3MF");
    struct Close { mz_zip_archive* zip; ~Close() { close_zip_reader(zip); } } close{&zip};
    wxArrayString names; std::vector<mz_uint> indices;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        std::string name = stat.m_filename;
        if (name.size() >= 6 && name.substr(name.size() - 6) == ".gcode") {
            names.Add(wxString::FromUTF8(name)); indices.push_back(i);
        }
    }
    if (indices.empty()) throw std::runtime_error("This 3MF has no sliced G-code. Export a sliced plate first.");
    int choice = 0;
    if (indices.size() > 1) {
        wxSingleChoiceDialog pick(parent, _L("Choose the plate to compare"), wxpath(path.filename()), names);
        if (pick.ShowModal() != wxID_OK) return {};
        choice = pick.GetSelection();
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, indices[choice], &stat) || stat.m_uncomp_size > limit)
        throw std::runtime_error("Comparison supports G-code up to 256 MB");
    std::string text(static_cast<size_t>(stat.m_uncomp_size), '\0');
    if (!mz_zip_reader_extract_to_mem(&zip, indices[choice], text.data(), text.size(), 0))
        throw std::runtime_error("Cannot extract plate G-code");
    return text;
}
inline void compare(wxWindow* parent) {
    try {
        std::map<std::string, std::string> summaries[2];
        std::string sources[2];
        for (int i = 0; i != 2; ++i) {
            wxFileDialog pick(parent, i == 0 ? _L("Select slice A") : _L("Select slice B"), "", "",
                "Sliced files (*.gcode;*.3mf)|*.gcode;*.3mf", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (pick.ShowModal() != wxID_OK) return;
            sources[i] = pick.GetPath().ToUTF8().data();
            auto text = read_gcode(parent, fs::u8path(sources[i]));
            if (text.empty()) return;
            std::istringstream stream(text); summaries[i] = SliceSummary::read(stream);
            if (summaries[i].empty()) throw std::runtime_error("No supported settings or estimates recorded in " + sources[i]);
        }
        report(parent, _L("Compare slice settings and estimates"), "A: " + sources[0] + "\nB: " + sources[1]
            + "\n\nComparison of recorded metadata only; this does not compare toolpath geometry.\n\n"
            + SliceSummary::compare(summaries[0], summaries[1]));
    } catch (const std::exception& e) { wxMessageBox(wxString::FromUTF8(e.what()), _L("Compare slices"), wxOK | wxICON_ERROR, parent); }
}
inline void archive_notes(wxWindow* parent) {
    try {
        fs::create_directories(archive_root());
        wxArrayString labels;
        std::vector<fs::path> records;
        for (const auto& entry : fs::directory_iterator(archive_root()))
            if (entry.is_directory() && entry.path().filename().u8string().rfind("job-", 0) == 0
                && fs::is_regular_file(entry.path() / "record.json")) records.push_back(entry.path() / "record.json");
        std::sort(records.rbegin(), records.rend());
        std::vector<fs::path> readable;
        for (const auto& path : records) {
            try {
                if (fs::file_size(path) > 1024 * 1024) continue;
                std::ifstream in(path); auto doc = nlohmann::json::parse(in);
                wxDateTime date(static_cast<time_t>(doc.value("sent_unix_seconds", int64_t(0))));
                labels.Add(date.FormatISOCombined(' ') + " | " + wxString::FromUTF8(doc.value("title", "Print"))
                    + " | " + wxString::FromUTF8(doc.value("printer", "")));
                readable.push_back(path);
            } catch (...) { /* Keep other records accessible if one was edited or damaged. */ }
        }
        if (readable.empty()) { wxMessageBox(_L("No archived sends yet. Enable archiving, then send a print."), _L("Print archive"), wxOK, parent); return; }
        wxSingleChoiceDialog pick(parent, _L("Choose a sent print to record its result"), _L("Print archive — newest first"), labels);
        if (pick.ShowModal() != wxID_OK) return;
        auto path = readable[pick.GetSelection()];
        std::ifstream in(path); auto doc = nlohmann::json::parse(in); in.close();
        wxTextEntryDialog edit(parent, _L("Result notes (sending a file does not confirm a completed print)"),
            _L("Print result"), wxString::FromUTF8(doc.value("notes", "")), wxOK | wxCANCEL | wxTE_MULTILINE);
        if (edit.ShowModal() != wxID_OK) return;
        std::lock_guard<std::mutex> guard(WorkflowArchive::archive_mutex);
        if (!fs::exists(path)) throw std::runtime_error("This record was removed by archive retention");
        doc["notes"] = std::string(edit.GetValue().ToUTF8().data());
        auto tmp = path; tmp += ".tmp";
        std::ofstream out(tmp); out << doc.dump(2); out.close();
        if (!out || !wxRenameFile(wxpath(tmp), wxpath(path), true)) throw std::runtime_error("Cannot save notes");
    } catch (const std::exception& e) { wxMessageBox(wxString::FromUTF8(e.what()), _L("Print archive"), wxOK | wxICON_ERROR, parent); }
}
inline void add_menu(wxMenu* file_menu, wxWindow* parent) {
    auto* menu = new wxMenu();
    auto add = [&](const wxString& name, auto callback) {
        auto* item = menu->Append(wxID_ANY, name);
        parent->Bind(wxEVT_MENU, callback, item->GetId());
    };
    add(_L("Save support recipe…"), [parent](wxCommandEvent&) { recipe(parent, true); });
    add(_L("Apply support recipe…"), [parent](wxCommandEvent&) { recipe(parent, false); });
    add(_L("Compare slice settings and estimates…"), [parent](wxCommandEvent&) { compare(parent); });
    menu->AppendSeparator();
    auto* enabled_item = menu->AppendCheckItem(wxID_ANY, _L("Archive successful sends (keep latest 100)"));
    std::error_code error;
    enabled_item->Check(fs::exists(archive_root() / "enabled", error));
    parent->Bind(wxEVT_MENU, [parent, enabled_item](wxCommandEvent& event) {
        try { WorkflowArchive::set_enabled(archive_root(), event.IsChecked()); }
        catch (...) { enabled_item->Check(!event.IsChecked()); wxMessageBox(_L("Cannot change archive setting."), _L("Print archive"), wxOK, parent); }
    }, enabled_item->GetId());
    add(_L("Open print archive folder"), [parent](wxCommandEvent&) {
        try { fs::create_directories(archive_root()); wxLaunchDefaultApplication(wxpath(archive_root())); }
        catch (...) { wxMessageBox(_L("Cannot open archive folder."), _L("Print archive"), wxOK, parent); }
    });
    add(_L("Add print result notes…"), [parent](wxCommandEvent&) { archive_notes(parent); });
    file_menu->AppendSubMenu(menu, _L("Workflow tools"));
}
}

#include "app/session_summary_export.h"

#include "imgui.h"
#include "util/json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace smp {
namespace {

constexpr std::string_view jsonSuffix = "_session.json";

std::string sessionIdFromPath(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    if (filename.size() <= jsonSuffix.size() || !filename.ends_with(jsonSuffix))
        return {};
    return filename.substr(0, filename.size() - jsonSuffix.size());
}

std::vector<std::filesystem::path> listSessionData(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
        return paths;
    for (std::filesystem::directory_iterator iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code entryError;
        if (!iterator->is_regular_file(entryError) || entryError)
            continue;
        const auto filename = iterator->path().filename().string();
        if (filename.ends_with(jsonSuffix))
            paths.push_back(iterator->path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string duration(double seconds) {
    const auto total = static_cast<long long>(seconds + 0.5);
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto secs = total % 60;
    std::ostringstream output;
    if (hours > 0)
        output << hours << "h ";
    if (hours > 0 || minutes > 0)
        output << minutes << "m ";
    output << secs << "s";
    return output.str();
}

std::string number(const json::Value& value, int precision = 1) {
    if (!value.isNumber())
        return "N/A";
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value.asNumber();
    return output.str();
}

std::string secondsFromMs(const json::Value& value) {
    if (!value.isNumber())
        return "N/A";
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value.asNumber() / 1000.0 << " s";
    return output.str();
}

void row(std::ostringstream& output, const std::string& label,
         const std::string& value) {
    output << std::left << std::setw(40) << label << std::right << std::setw(14)
           << value << '\n';
}

void formatStats(std::ostringstream& output, const json::Value& stats) {
    row(output, "Games", std::to_string(stats["games"].asInt()));
    row(output, "Active time", duration(stats["active_seconds"].asNumber()));

    output << "\nCAMERA NAVIGATION\n\n";
    row(output, "Transitions / min",
        number(stats["navigation"]["transitions_per_minute"]));
    row(output, "Control-group jumps",
        std::to_string(stats["navigation"]["control_group_jumps"].asInt()));
    row(output, "Location-hotkey jumps",
        std::to_string(stats["navigation"]["location_hotkey_jumps"].asInt()));
    row(output, "Minimap jumps",
        std::to_string(stats["navigation"]["minimap_jumps"].asInt()));
    row(output, "Edge pans",
        std::to_string(stats["navigation"]["edge_pans"].asInt()));

    output << "\nWORKER MACRO\n\n";
    row(output, "Cycles", std::to_string(stats["worker_macro"]["cycles"].asInt()));
    row(output, "Average duration",
        secondsFromMs(stats["worker_macro"]["average_duration_ms"]));
    row(output, "Production visits",
        std::to_string(stats["worker_macro"]["production_visits"].asInt()));

    output << "\nARMY MACRO\n\n";
    row(output, "Cycles", std::to_string(stats["army_macro"]["cycles"].asInt()));
    row(output, "Average duration",
        secondsFromMs(stats["army_macro"]["average_duration_ms"]));
    row(output, "Production visits",
        std::to_string(stats["army_macro"]["production_visits"].asInt()));

    output << "\nARMY CONTROL-GROUP MANAGEMENT\n\n";
    row(output, "Assignments",
        std::to_string(stats["army_control_groups"]["assignments"].asInt()));
    row(output, "Additions",
        std::to_string(stats["army_control_groups"]["additions"].asInt()));
    row(output, "Edits / min",
        number(stats["army_control_groups"]["edits_per_minute"]));

    output << "\nSCOUTING\n\n";
    row(output, "Confirmed scouting units",
        std::to_string(stats["scouting"]["confirmed_units"].asInt()));
}

std::string formatReadableSummary(const json::Value& root) {
    std::ostringstream output;
    const std::string sessionId = root["session_id"].asString("Unknown");
    output << "============================================================\n"
           << "STARCRAFT MECHANICS PROFILER - SESSION SUMMARY\n"
           << "============================================================\n\n"
           << "Session: " << sessionId << "\n\n";
    formatStats(output, root["overall"]);

    if (root["matchups"].isObject() && !root["matchups"].asObject().empty()) {
        output << "\n============================================================\n"
               << "MATCHUP BREAKDOWN\n"
               << "============================================================\n";
        for (const auto& [matchup, stats] : root["matchups"].asObject()) {
            output << "\n" << matchup << "\n\n";
            formatStats(output, stats);
        }
    }

    if (root["games"].isArray() && !root["games"].asArray().empty()) {
        output << "\n============================================================\n"
               << "GAMES\n"
               << "============================================================\n\n";
        for (const auto& game : root["games"].asArray()) {
            output << "Game " << game["ordinal"].asInt() << "  "
                   << game["matchup"].asString("Unknown") << '\n';
        }
    }
    return output.str();
}

std::filesystem::path exportReadableSummary(const std::filesystem::path& source) {
    const auto root = json::parseFile(source);
    const auto sessionId = root["session_id"].asString(sessionIdFromPath(source));
    if (sessionId.empty())
        throw std::runtime_error("Session history file has no session id");

    const auto exportRoot = source.parent_path().parent_path() / "exports";
    std::filesystem::create_directories(exportRoot);
    const auto destination = exportRoot / (sessionId + "_session.txt");
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Unable to create readable session export");
    const auto text = formatReadableSummary(root);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output)
        throw std::runtime_error("Unable to write readable session export");
    return destination;
}

} // namespace

void drawSessionSummaryExport(const std::filesystem::path& summariesRoot) {
    static std::filesystem::path cachedRoot;
    static std::vector<std::filesystem::path> sessions;
    static int selected = -1;
    static double refreshAt{};
    static double feedbackUntil{};
    static std::string feedback;

    const double now = ImGui::GetTime();
    if (cachedRoot != summariesRoot || now >= refreshAt) {
        cachedRoot = summariesRoot;
        sessions = listSessionData(summariesRoot);
        if (sessions.empty())
            selected = -1;
        else if (selected < 0 || selected >= static_cast<int>(sessions.size()))
            selected = static_cast<int>(sessions.size()) - 1;
        refreshAt = now + 2.0;
    }

    ImGui::TextDisabled("Export session");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(210.0f);
    const char* preview = selected >= 0 ? sessions[static_cast<std::size_t>(selected)].filename().string().c_str()
                                        : "No session data";
    std::string previewStorage;
    if (selected >= 0) {
        previewStorage = sessionIdFromPath(sessions[static_cast<std::size_t>(selected)]);
        preview = previewStorage.c_str();
    }
    if (ImGui::BeginCombo("##SessionExportChoice", preview)) {
        for (int index = static_cast<int>(sessions.size()) - 1; index >= 0; --index) {
            const auto label = sessionIdFromPath(sessions[static_cast<std::size_t>(index)]);
            if (ImGui::Selectable(label.c_str(), selected == index))
                selected = index;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const bool available = selected >= 0 && selected < static_cast<int>(sessions.size());
    if (!available)
        ImGui::BeginDisabled();
    if (ImGui::Button("Export readable summary")) {
        try {
            const auto path = exportReadableSummary(sessions[static_cast<std::size_t>(selected)]);
            feedback = "Exported: " + path.string();
        } catch (const std::exception& error) {
            feedback = std::string("Export failed: ") + error.what();
        }
        feedbackUntil = now + 5.0;
    }
    if (!available)
        ImGui::EndDisabled();
    if (now < feedbackUntil) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", feedback.c_str());
    }
}

} // namespace smp

#include "storage/nav_retention.h"

#include "storage/session.h"
#include "util/json.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>
#include <windows.h>

namespace smp {
namespace {

constexpr const char* retentionIndexFilename = "nav_retention_index.json";

struct ManagedNavEntry {
    std::string navFilename;
    std::string sessionHistoryFilename;
    std::int64_t chronology{};
};

std::filesystem::path normalizedAbsolute(
    const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::path summariesRoot(
    const std::filesystem::path& sessionsRoot) {
    const auto parent = sessionsRoot.parent_path();
    return parent.empty() ? std::filesystem::path("sessionSummaries")
                          : parent / "sessionSummaries";
}

std::filesystem::path retentionIndexPath(
    const std::filesystem::path& sessionsRoot) {
    return summariesRoot(sessionsRoot) / retentionIndexFilename;
}

bool isDirectChildOf(const std::filesystem::path& parent,
                     const std::filesystem::path& child) {
    return normalizedAbsolute(child.parent_path()) ==
           normalizedAbsolute(parent);
}

bool isPlainFilename(const std::string& value,
                     const std::filesystem::path& requiredExtension) {
    const std::filesystem::path path(value);
    return !path.empty() && path == path.filename() &&
           path.extension() == requiredExtension;
}

bool isSessionHistoryFilename(const std::string& value) {
    return isPlainFilename(value, ".json") &&
           value.ends_with("_session.json");
}

bool pairedDerivedJsonAvailable(const std::filesystem::path& navPath) {
    auto jsonPath = navPath;
    jsonPath.replace_extension(".json");
    try {
        if (!std::filesystem::is_regular_file(jsonPath))
            return false;
        return json::parseFile(jsonPath)["session"]["id"].asString() ==
               navPath.stem().string();
    } catch (...) {
        return false;
    }
}

bool sessionHistoryAvailable(const std::filesystem::path& path) {
    try {
        if (!std::filesystem::is_regular_file(path))
            return false;
        const auto history = json::parseFile(path);
        return history["overall"].isObject() &&
               !history["games"].asArray().empty();
    } catch (...) {
        return false;
    }
}

std::optional<std::vector<ManagedNavEntry>> loadIndex(
    const std::filesystem::path& path,
    std::string& warning) {
    // Never infer automatic-recording provenance from a .nav filename or a
    // neighboring JSON file. Pre-index files remain retained unless the app
    // has explicitly registered them after successful automatic finalization.
    if (!std::filesystem::exists(path))
        return std::vector<ManagedNavEntry>{};
    try {
        const auto root = json::parseFile(path);
        if (root["schema_version"].asInt() != 1 ||
            !root["sessions"].isArray()) {
            warning = "Navigation retention metadata has an unsupported format";
            return std::nullopt;
        }
        std::vector<ManagedNavEntry> entries;
        for (const auto& value : root["sessions"].asArray()) {
            ManagedNavEntry entry;
            entry.navFilename = value["nav_file"].asString();
            entry.sessionHistoryFilename =
                value["session_history_file"].asString();
            entry.chronology = static_cast<std::int64_t>(
                value["session_start_unix_ms"].asNumber());
            if (!isPlainFilename(entry.navFilename, ".nav") ||
                !isSessionHistoryFilename(entry.sessionHistoryFilename) ||
                entry.chronology <= 0) {
                warning = "Navigation retention metadata contains an invalid entry";
                return std::nullopt;
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    } catch (const std::exception& error) {
        warning = std::string("Unable to read navigation retention metadata: ") +
                  error.what();
    } catch (...) {
        warning = "Unable to read navigation retention metadata";
    }
    return std::nullopt;
}

void writeIndex(const std::filesystem::path& path,
                const std::vector<ManagedNavEntry>& entries) {
    json::Value root(json::Value::Object{});
    root["schema_version"] = 1;
    json::Value::Array sessions;
    for (const auto& entry : entries) {
        sessions.emplace_back(json::Value::Object{
            {"nav_file", entry.navFilename},
            {"session_history_file", entry.sessionHistoryFilename},
            {"session_start_unix_ms",
             static_cast<double>(entry.chronology)}});
    }
    root["sessions"] = std::move(sessions);

    std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error(
                "Unable to create navigation retention metadata");
        const auto encoded = json::stringify(root, 2) + "\n";
        output.write(encoded.data(),
                     static_cast<std::streamsize>(encoded.size()));
        output.flush();
        if (!output)
            throw std::runtime_error(
                "Unable to write navigation retention metadata");
        output.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(
                "Unable to finalize navigation retention metadata: " +
                std::system_category().message(
                    static_cast<int>(GetLastError())));
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::vector<NavRetentionCandidate> managedCandidates(
    const std::filesystem::path& sessionsRoot,
    const std::vector<ManagedNavEntry>& entries) {
    std::vector<NavRetentionCandidate> candidates;
    const auto historyRoot = summariesRoot(sessionsRoot);
    for (const auto& entry : entries) {
        const auto navPath = sessionsRoot / entry.navFilename;
        std::error_code error;
        if (!std::filesystem::is_regular_file(navPath, error) || error)
            continue;
        try {
            const auto session = readNavSession(navPath);
            if (session.sessionId != navPath.stem().string() ||
                session.sessionStartUnixMs != entry.chronology) {
                continue;
            }
        } catch (...) {
            continue;
        }
        const auto historyPath =
            historyRoot / entry.sessionHistoryFilename;
        candidates.push_back(
            {navPath, entry.chronology, true, true,
             sessionHistoryAvailable(historyPath) &&
                 pairedDerivedJsonAvailable(navPath),
             false});
    }
    return candidates;
}

ManagedNavRetentionResult runManagedRetention(
    const std::filesystem::path& sessionsRoot,
    NavRetentionPolicy policy,
    const std::optional<std::filesystem::path>& latestNavPath) {
    ManagedNavRetentionResult result;
    const auto indexPath = retentionIndexPath(sessionsRoot);
    auto entries = loadIndex(indexPath, result.warning);
    if (!entries)
        return result;
    const auto candidates = managedCandidates(sessionsRoot, *entries);
    result.cleanup = applyNavRetention(
        planNavRetention(candidates, policy, latestNavPath));
    if (!result.cleanup.removedPaths.empty() ||
        !result.cleanup.alreadyMissingPaths.empty()) {
        std::set<std::string> inactiveFiles;
        for (const auto& path : result.cleanup.removedPaths)
            inactiveFiles.insert(path.filename().string());
        for (const auto& path : result.cleanup.alreadyMissingPaths)
            inactiveFiles.insert(path.filename().string());
        std::erase_if(*entries, [&](const auto& entry) {
            return inactiveFiles.contains(entry.navFilename);
        });
        try {
            writeIndex(indexPath, *entries);
        } catch (const std::exception& error) {
            result.warning =
                std::string("Navigation cleanup completed, but its metadata "
                            "could not be compacted: ") +
                error.what();
        } catch (...) {
            result.warning =
                "Navigation cleanup completed, but its metadata could not be "
                "compacted";
        }
    }
    return result;
}

} // namespace

NavRetentionPlan planNavRetention(
    const std::vector<NavRetentionCandidate>& candidates,
    NavRetentionPolicy policy,
    const std::optional<std::filesystem::path>& latestNavPath) {
    policy = normalizedNavRetentionPolicy(policy);
    std::vector<const NavRetentionCandidate*> completed;
    for (const auto& candidate : candidates) {
        if (candidate.managedAutomatic && candidate.completed &&
            !candidate.navPath.empty()) {
            completed.push_back(&candidate);
        }
    }
    std::sort(completed.begin(), completed.end(), [](const auto* first,
                                                     const auto* second) {
        return first->chronology != second->chronology
                   ? first->chronology < second->chronology
                   : first->navPath < second->navPath;
    });

    NavRetentionPlan plan;
    std::set<std::filesystem::path> protectedPaths;
    if (latestNavPath && !latestNavPath->empty())
        protectedPaths.insert(*latestNavPath);
    const std::size_t keepCount =
        policy.mode == NavRetentionMode::KeepAll
            ? completed.size()
            : std::min(completed.size(),
                       static_cast<std::size_t>(policy.gamesToKeep));
    for (std::size_t index = completed.size() - keepCount;
         index < completed.size(); ++index) {
        protectedPaths.insert(completed[index]->navPath);
    }
    if (!completed.empty())
        protectedPaths.insert(completed.back()->navPath);

    plan.protectedNavPaths.assign(protectedPaths.begin(),
                                  protectedPaths.end());
    if (policy.mode == NavRetentionMode::KeepAll)
        return plan;
    for (const auto* candidate : completed) {
        if (!protectedPaths.contains(candidate->navPath) &&
            candidate->derivedArtifactsPersisted &&
            !candidate->currentlyOpen) {
            plan.deletionPaths.push_back(candidate->navPath);
        }
    }
    return plan;
}

NavRetentionApplyResult applyNavRetention(const NavRetentionPlan& plan) {
    return applyNavRetention(
        plan, [](const std::filesystem::path& path, std::error_code& error) {
            return std::filesystem::remove(path, error);
        });
}

NavRetentionApplyResult applyNavRetention(
    const NavRetentionPlan& plan,
    const NavRetentionRemove& remove) {
    NavRetentionApplyResult result;
    if (!remove)
        return result;
    for (const auto& path : plan.deletionPaths) {
        std::error_code error;
        const bool removed = remove(path, error);
        if (removed && !error)
            result.removedPaths.push_back(path);
        else if (!error)
            result.alreadyMissingPaths.push_back(path);
        else
            result.failedPaths.push_back(path);
    }
    return result;
}

bool canRunNavRetention(
    const NavRetentionFinalizationState& state) noexcept {
    return state.automaticRecordingCompleted &&
           state.derivedAnalysisPersisted && state.sessionHistoryPersisted;
}

ManagedNavRetentionResult recordFinalizedAutomaticNavAndApplyRetention(
    const std::filesystem::path& sessionsRoot,
    const std::filesystem::path& navPath,
    const std::filesystem::path& derivedJsonPath,
    const std::filesystem::path& sessionHistoryPath,
    NavRetentionPolicy policy) noexcept {
    ManagedNavRetentionResult result;
    try {
        const auto historyRoot = summariesRoot(sessionsRoot);
        if (!isDirectChildOf(sessionsRoot, navPath) ||
            !isDirectChildOf(sessionsRoot, derivedJsonPath) ||
            !isDirectChildOf(historyRoot, sessionHistoryPath) ||
            navPath.extension() != ".nav" ||
            derivedJsonPath.extension() != ".json" ||
            navPath.stem() != derivedJsonPath.stem() ||
            !std::filesystem::is_regular_file(navPath) ||
            !std::filesystem::is_regular_file(derivedJsonPath) ||
            !sessionHistoryAvailable(sessionHistoryPath) ||
            !pairedDerivedJsonAvailable(navPath)) {
            result.warning =
                "Automatic navigation retention skipped a game because its "
                "required persisted artifacts could not be verified";
            return result;
        }
        const auto session = readNavSession(navPath);
        auto indexEntries = loadIndex(retentionIndexPath(sessionsRoot),
                                      result.warning);
        if (!indexEntries)
            return result;
        ManagedNavEntry entry{navPath.filename().string(),
                              sessionHistoryPath.filename().string(),
                              session.sessionStartUnixMs};
        const auto existing = std::find_if(
            indexEntries->begin(), indexEntries->end(),
            [&](const auto& candidate) {
                return candidate.navFilename == entry.navFilename;
            });
        if (existing == indexEntries->end())
            indexEntries->push_back(std::move(entry));
        else
            *existing = std::move(entry);
        writeIndex(retentionIndexPath(sessionsRoot), *indexEntries);
        result.registrationPersisted = true;

        auto cleanup = runManagedRetention(sessionsRoot, policy, navPath);
        result.cleanup = std::move(cleanup.cleanup);
        if (!cleanup.warning.empty())
            result.warning = std::move(cleanup.warning);
    } catch (const std::exception& error) {
        result.warning =
            std::string("Automatic navigation retention was skipped: ") +
            error.what();
    } catch (...) {
        result.warning = "Automatic navigation retention was skipped";
    }
    return result;
}

ManagedNavRetentionResult applyManagedNavRetention(
    const std::filesystem::path& sessionsRoot,
    NavRetentionPolicy policy) noexcept {
    try {
        return runManagedRetention(sessionsRoot, policy, std::nullopt);
    } catch (const std::exception& error) {
        ManagedNavRetentionResult result;
        result.warning =
            std::string("Automatic navigation retention was skipped: ") +
            error.what();
        return result;
    } catch (...) {
        ManagedNavRetentionResult result;
        result.warning = "Automatic navigation retention was skipped";
        return result;
    }
}

} // namespace smp

#include "test_framework.h"
#include "cli/replay_snapshot.h"

#include <fstream>
#include <type_traits>
#include <system_error>
#include <utility>
#include <windows.h>

namespace {

using Clock = smp::ReplayReadinessHooks::Clock;

struct ReplayFiles {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("smp-pinned-replay-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::path source = root / "LastReplay.rep";
    std::filesystem::path snapshot = root / "game1.finalizing.rep";
    ReplayFiles() { std::filesystem::create_directories(root); }
    ~ReplayFiles() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }

    static void write(const std::filesystem::path& path, const std::string& bytes,
                      std::ios::openmode mode = std::ios::trunc) {
        std::ofstream output(path, std::ios::binary | mode);
        output << bytes;
        output.close();
        REQUIRE(output.good());
    }
    static std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.is_open());
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    void replace(const std::string& bytes) {
        const auto next = root / "next.rep";
        write(next, bytes);
        if (!ReplaceFileW(source.c_str(), next.c_str(), nullptr, 0, nullptr, nullptr))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Replace pinned pathname");
    }
};

struct ReadinessTest {
    Clock::time_point now{};
    int waits{};
    int parses{};
    smp::PinnedReplayReadinessHooks hooks;
    smp::ReplayReadinessPolicy policy;
    ReadinessTest() {
        hooks.now = [&]() { return now; };
        hooks.wait = [&](std::chrono::milliseconds duration) { now += duration; ++waits; };
        policy.timeout = std::chrono::milliseconds(600);
    }
    void expectBytes(const ReplayFiles& files, std::string bytes) {
        hooks.parse = [&, bytes = std::move(bytes)](const auto& path, std::chrono::milliseconds timeout) {
            ++parses;
            REQUIRE(path == files.snapshot);
            REQUIRE(timeout > std::chrono::milliseconds::zero());
            REQUIRE(ReplayFiles::read(path) == bytes);
            smp::ReplayExtractionResult result;
            result.available = true;
            return result;
        };
    }
};

static_assert(!std::is_copy_constructible_v<smp::PinnedReplaySource>);
static_assert(!std::is_copy_assignable_v<smp::PinnedReplaySource>);
static_assert(std::is_nothrow_move_constructible_v<smp::PinnedReplaySource>);
static_assert(std::is_nothrow_move_assignable_v<smp::PinnedReplaySource>);

} // namespace

TEST_CASE("same generation replay grows after notification and is accepted after settling") {
    ReplayFiles files;
    ReplayFiles::write(files.source, std::string(100, 'a'));
    const auto observed = smp::readReplayMetadata(files.source);
    REQUIRE(observed.size == 100);
    auto pinned = smp::PinnedReplaySource::open(files.source);
    REQUIRE(pinned);
    ReadinessTest test;
    test.expectBytes(files, std::string(100, 'a') + std::string(20, 'b'));
    test.hooks.wait = [&](std::chrono::milliseconds duration) {
        test.now += duration;
        if (++test.waits <= 2)
            ReplayFiles::write(files.source, std::string(10, 'b'), std::ios::app);
    };
    const auto result = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(result.available);
    REQUIRE(result.unavailableReason.empty());
    REQUIRE(test.waits == 3); // 100 -> 110 -> 120 -> stable 120.
    REQUIRE(test.parses == 1);
    REQUIRE(pinned.metadata().size == 120);
    REQUIRE(!(pinned.metadata() == observed)); // Changed metadata is not a new generation.
    REQUIRE(!std::filesystem::exists(files.snapshot));
}

TEST_CASE("pinned replay survives pathname replacement and deletion without reading next game") {
    ReplayFiles files;
    ReplayFiles::write(files.source, "game one");
    auto pinned = smp::PinnedReplaySource::open(files.source);
    REQUIRE(pinned);
    files.replace("game two"); // Real Windows replacement, not rewriting the same file object.
    REQUIRE(ReplayFiles::read(files.source) == "game two");
    ReadinessTest test;
    test.expectBytes(files, "game one");
    const auto result = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(result.available);
    REQUIRE(test.parses == 1);
    REQUIRE(DeleteFileW(files.source.c_str()));
    REQUIRE(!std::filesystem::exists(files.source));
    const auto afterDelete = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(afterDelete.available);
    REQUIRE(test.parses == 2);
}

TEST_CASE("a write during snapshot copy rejects partial bytes and retries a consistent snapshot") {
    ReplayFiles files;
    const std::string original(131072, 'a'); // More than one copy chunk.
    const std::string updated(131073, 'b');
    ReplayFiles::write(files.source, original);
    auto pinned = smp::PinnedReplaySource::open(files.source);
    ReadinessTest test;
    bool changed = false;
    bool discarded = false;
    int chunks = 0;
    test.expectBytes(files, updated);
    test.hooks.copiedChunk = [&]() {
        ++chunks;
        if (!changed) {
            changed = true;
            // Writers remain allowed while the handle and copy are active.
            ReplayFiles::write(files.source, updated);
        }
    };
    test.hooks.wait = [&](std::chrono::milliseconds duration) {
        test.now += duration;
        ++test.waits;
        if (changed) {
            REQUIRE(!std::filesystem::exists(files.snapshot));
            discarded = true;
        }
    };
    const auto result = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(result.available);
    REQUIRE(changed);
    REQUIRE(discarded);
    REQUIRE(chunks == 5); // Rejected two-chunk attempt, then a complete three-chunk copy.
    REQUIRE(test.parses == 1); // Mixed bytes never reach the parser.
    REQUIRE(test.waits == 3); // New metadata must settle before the retry.
    REQUIRE(!std::filesystem::exists(files.snapshot));
}

TEST_CASE("pinned snapshot parser retries keep immutable bytes after pathname replacement") {
    ReplayFiles files;
    ReplayFiles::write(files.source, "game one");
    auto pinned = smp::PinnedReplaySource::open(files.source);
    ReadinessTest test;
    int copies = 0;
    test.hooks.copiedChunk = [&]() { ++copies; };
    test.hooks.parse = [&](const auto& path, std::chrono::milliseconds) {
        REQUIRE(path == files.snapshot);
        REQUIRE(ReplayFiles::read(path) == "game one");
        smp::ReplayExtractionResult result;
        if (++test.parses == 1) {
            files.replace("game two");
            result.unavailableReason = "temporary parser failure";
        } else result.available = true;
        return result;
    };
    const auto result = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(result.available);
    REQUIRE(test.parses == 2);
    REQUIRE(copies == 1);
}

TEST_CASE("pinned replay handle moves ownership and never blocks a writer or deletion") {
    ReplayFiles files;
    ReplayFiles::write(files.source, "game one");
    auto first = smp::PinnedReplaySource::open(files.source);
    smp::PinnedReplaySource second(std::move(first));
    REQUIRE(!first);
    REQUIRE(second);
    smp::PinnedReplaySource third;
    third = std::move(second);
    REQUIRE(!second);
    ReplayFiles::write(files.source, " grows", std::ios::app);
    REQUIRE(third.metadata().size == 14);
    REQUIRE(DeleteFileW(files.source.c_str()));
    REQUIRE(third.metadata().exists);
    third = {};
    REQUIRE(!third);
}

TEST_CASE("unobtainable or never stable pinned replay is unavailable within readiness deadline") {
    ReplayFiles files;
    auto missing = smp::PinnedReplaySource::open(files.source);
    ReadinessTest test;
    test.expectBytes(files, "should not parse");
    auto result = smp::waitForPinnedReplayReadiness(missing, files.snapshot, test.hooks, test.policy);
    REQUIRE(!result.available);
    REQUIRE(test.parses == 0);
    REQUIRE(test.waits == 0);
    REQUIRE(result.unavailableReason.find("pin") != std::string::npos);
    ReplayFiles::write(files.source, "game one");
    auto pinned = smp::PinnedReplaySource::open(files.source);
    test.hooks.wait = [&](std::chrono::milliseconds duration) {
        test.now += duration;
        ++test.waits;
        ReplayFiles::write(files.source, "+", std::ios::app);
    };
    result = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(!result.available);
    REQUIRE(test.parses == 0);
    REQUIRE(test.now.time_since_epoch() == test.policy.timeout);
    REQUIRE(!std::filesystem::exists(files.snapshot));
}

TEST_CASE("pinned replay continuously changes during copies and expires without parsing partial bytes") {
    ReplayFiles files;
    ReplayFiles::write(files.source, "game one");
    auto pinned = smp::PinnedReplaySource::open(files.source);
    ReadinessTest test;
    int attempts = 0;
    test.expectBytes(files, "should not parse");
    test.hooks.copiedChunk = [&]() {
        ++attempts;
        ReplayFiles::write(files.source, "+", std::ios::app);
    };
    const auto result = smp::waitForPinnedReplayReadiness(pinned, files.snapshot, test.hooks, test.policy);
    REQUIRE(!result.available);
    REQUIRE(attempts > 1);
    REQUIRE(test.parses == 0);
    REQUIRE(test.now.time_since_epoch() == test.policy.timeout);
    REQUIRE(result.unavailableReason.find("changed during snapshot copy") != std::string::npos);
    REQUIRE(!std::filesystem::exists(files.snapshot));
}

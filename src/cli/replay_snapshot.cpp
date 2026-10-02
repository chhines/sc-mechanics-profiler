#include "cli/replay_snapshot.h"

#include <array>
#include <fstream>
#include <windows.h>

namespace smp {

std::string snapshotGenerationReplay(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     const ReplayMetadata& expected) {
    const auto handle = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return "Replay snapshot unavailable: source is missing, busy or still being written";
    struct CloseHandleOnExit {
        HANDLE handle;
        ~CloseHandleOnExit() { CloseHandle(handle); }
    } close{handle};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        return "Replay snapshot unavailable: unable to verify source metadata";
    const ReplayMetadata actual{
        true, (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                  info.ftLastWriteTime.dwLowDateTime,
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
    if (!(actual == expected))
        return "Replay generation mismatch: LastReplay.rep advanced before snapshot capture";
    if (!expected.exists || expected.size == 0 || source == destination)
        return "Replay snapshot unavailable: invalid source or destination";

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) return "Replay snapshot unavailable: unable to create temporary snapshot";
    std::array<char, 65536> bytes{};
    std::uint64_t copied = 0;
    for (;;) {
        DWORD count{};
        if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
            return "Replay snapshot unavailable: source read failed";
        if (count == 0) break;
        output.write(bytes.data(), count);
        if (!output) return "Replay snapshot unavailable: snapshot write failed";
        copied += count;
    }
    output.close();
    if (!output || copied != expected.size)
        return "Replay snapshot unavailable: incomplete snapshot";
    return {};
}

} // namespace smp

#pragma once

#include "platform/automatic_lifecycle.h"

namespace smp {

// Holds a Windows read handle that denies writes/deletes only during the copy.
// Empty return means success. A changed source is never copied for an old game.
[[nodiscard]] std::string snapshotGenerationReplay(
    const std::filesystem::path& source, const std::filesystem::path& destination,
    const ReplayMetadata& expected);

} // namespace smp

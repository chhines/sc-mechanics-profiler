#pragma once

#include <string_view>
#include <windows.h>

namespace smp {

inline constexpr wchar_t guiInstanceMutexName[] =
    L"Local\\StarcraftMechanicsProfiler.Gui.SingleInstance.v1";
inline constexpr wchar_t showExistingGuiInstanceMessageName[] =
    L"StarcraftMechanicsProfiler.ShowExistingGuiInstance.v1";
inline constexpr wchar_t guiMainWindowClassName[] =
    L"StarcraftMechanicsProfilerMainWindow";

class GuiInstanceClaim {
  public:
    ~GuiInstanceClaim();
    GuiInstanceClaim(const GuiInstanceClaim&) = delete;
    GuiInstanceClaim& operator=(const GuiInstanceClaim&) = delete;
    GuiInstanceClaim(GuiInstanceClaim&& other) noexcept;
    GuiInstanceClaim& operator=(GuiInstanceClaim&& other) noexcept;

    [[nodiscard]] static GuiInstanceClaim acquire(std::wstring_view mutexName);
    [[nodiscard]] bool ownsInstance() const noexcept { return ownsInstance_; }

  private:
    GuiInstanceClaim(HANDLE handle, bool ownsInstance) noexcept;
    void close() noexcept;

    HANDLE handle_{};
    bool ownsInstance_{};
};

[[nodiscard]] UINT showExistingGuiInstanceMessage() noexcept;
[[nodiscard]] bool signalExistingGuiInstance(DWORD maximumWaitMs = 2000) noexcept;

} // namespace smp

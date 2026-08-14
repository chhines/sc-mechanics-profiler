#include "app/windows_application.h"

#include "app/analysis_window.h"
#include "app/application_controller.h"
#include "app/game_analysis_visualization_model.h"
#include "app/gui_preferences.h"
#include "app/gui_single_instance.h"
#include "app/page_container.h"
#include "app/results_view_model.h"
#include "cli/automatic_session_files.h"
#include "config/config.h"
#include "platform/foreground.h"
#include "platform/resource_ids.h"
#include "storage/session.h"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <shellapi.h>
#include <windowsx.h>
#include <string>
#include <string_view>
#include <vector>

namespace smp {
namespace {

constexpr wchar_t resultsCanvasClass[] = L"StarcraftMechanicsProfilerResultsCanvas";
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT controllerChangedMessage = WM_APP + 2;
constexpr UINT uiTimer = 1;
constexpr UINT trayIconId = 1;

enum ControlId : int {
    IdTabs = 100,
    IdStatusMode,
    IdStatusActivity,
    IdStatusStarCraft,
    IdStatusSession,
    IdStatusDetail,
    IdAutomaticToggle,
    IdDebugToggle,
    IdCalibrate,
    IdOpenData,
    IdShowResults,
    IdExit,
    IdDebugLog,
    IdResultsSource,
    IdResultsOpenFile,
    IdResultsOpenSession,
    IdResultsExport,
    IdResultsAnalysis,
    IdResultsCanvas,
    IdSettingCamera,
    IdSettingWorker,
    IdSettingArmy,
    IdSettingStyles,
    IdSettingArmyGroups,
    IdSettingScouting,
    IdSettingMinimize,
    IdSettingSelectAll,
    IdSettingSave,
    IdSettingOpenConfig,
    IdSettingCalibrate,
    IdAboutText,
    IdStatusDataFolder = 200,
    IdTrayOpen = 1000,
    IdTrayStatus,
    IdTrayToggle,
    IdTrayLatest,
    IdTrayExit,
};

std::wstring wide(std::string_view value) {
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

void setWindowText(HWND control, const std::string& value) {
    const auto converted = wide(value);
    SetWindowTextW(control, converted.c_str());
}

void setControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND createControl(HWND parent, const wchar_t* className, const wchar_t* text,
                   DWORD style, int id, DWORD extendedStyle = 0) {
    const HWND control = CreateWindowExW(
        extendedStyle, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    setControlFont(control, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    return control;
}

void move(HWND control, int x, int y, int width, int height) {
    MoveWindow(control, x, y, std::max(0, width), std::max(0, height), TRUE);
}

void setChecked(HWND control, bool checked) {
    Button_SetCheck(control, checked ? BST_CHECKED : BST_UNCHECKED);
}

bool checked(HWND control) {
    return Button_GetCheck(control) == BST_CHECKED;
}

struct ResultsCanvasData {
    const ResultsViewModel* model{};
    int scroll{};
    int contentHeight{};
    HFONT normal{};
    HFONT heading{};
};

void updateCanvasScroll(HWND window, ResultsCanvasData& data) {
    RECT client{};
    GetClientRect(window, &client);
    const int page = std::max(1, static_cast<int>(client.bottom - client.top));
    data.scroll = std::clamp(data.scroll, 0, std::max(0, data.contentHeight - page));
    SCROLLINFO info{sizeof(info), SIF_PAGE | SIF_POS | SIF_RANGE};
    info.nMin = 0;
    info.nMax = std::max(0, data.contentHeight - 1);
    info.nPage = static_cast<UINT>(page);
    info.nPos = data.scroll;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
}

void paintResultsCanvas(HWND window, ResultsCanvasData& data) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(32, 39, 48));
    const int left = 20;
    const int right = std::max(left + 100, static_cast<int>(client.right) - 24);
    int y = 18 - data.scroll;
    int logicalY = 18;

    const auto drawText = [&](const std::wstring& text, RECT rect, UINT format,
                              HFONT font, COLORREF color) {
        const auto oldFont = SelectObject(dc, font);
        const auto oldColor = SetTextColor(dc, color);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
        SetTextColor(dc, oldColor);
        SelectObject(dc, oldFont);
    };

    if (!data.model) {
        RECT empty{left, y, right, y + 30};
        drawText(L"No results are available yet.", empty, DT_LEFT | DT_VCENTER,
                 data.normal, RGB(90, 98, 108));
        logicalY += 50;
    } else {
        RECT title{left, y, right, y + 30};
        drawText(wide(data.model->title), title, DT_LEFT | DT_VCENTER, data.heading,
                 RGB(24, 76, 128));
        y += 30;
        logicalY += 30;
        if (!data.model->subtitle.empty()) {
            RECT subtitle{left, y, right, y + 22};
            drawText(wide(data.model->subtitle), subtitle, DT_LEFT | DT_VCENTER,
                     data.normal, RGB(92, 100, 110));
            y += 30;
            logicalY += 30;
        }
        for (const auto& section : data.model->sections) {
            y += 8;
            logicalY += 8;
            RECT header{left, y, right, y + 27};
            drawText(wide(section.title), header, DT_LEFT | DT_VCENTER, data.heading,
                     RGB(32, 72, 112));
            HPEN linePen = CreatePen(PS_SOLID, 1, RGB(207, 216, 225));
            const auto oldPen = SelectObject(dc, linePen);
            MoveToEx(dc, left, y + 27, nullptr);
            LineTo(dc, right, y + 27);
            SelectObject(dc, oldPen);
            DeleteObject(linePen);
            y += 35;
            logicalY += 35;
            for (const auto& metric : section.metrics) {
                RECT label{left, y, left + (right - left) * 58 / 100, y + 22};
                RECT value{label.right + 8, y, right, y + 22};
                drawText(wide(metric.label), label, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS,
                         data.normal, RGB(45, 52, 60));
                drawText(wide(metric.value), value, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS,
                         data.normal, RGB(45, 52, 60));
                y += 24;
                logicalY += 24;
            }
            y += 8;
            logicalY += 8;
        }
    }
    data.contentHeight = logicalY + 20;
    updateCanvasScroll(window, data);
    EndPaint(window, &paint);
}

LRESULT CALLBACK resultsCanvasProcedure(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam) {
    auto* data = reinterpret_cast<ResultsCanvasData*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        data = static_cast<ResultsCanvasData*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (!data)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_PAINT:
        paintResultsCanvas(window, *data);
        return 0;
    case WM_SIZE:
        updateCanvasScroll(window, *data);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_MOUSEWHEEL: {
        data->scroll -= GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 60;
        updateCanvasScroll(window, *data);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window, SB_VERT, &info);
        int position = data->scroll;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: position -= 30; break;
        case SB_LINEDOWN: position += 30; break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: position = info.nTrackPos; break;
        default: break;
        }
        data->scroll = position;
        updateCanvasScroll(window, *data);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

class ApplicationWindow {
  public:
    ApplicationWindow(HINSTANCE instance, GuiApplicationPaths paths)
        : instance_(instance), paths_(std::move(paths)),
          config_(Config::loadOrCreate(paths_.config)),
          preferences_(GuiPreferences::load(paths_.preferences)),
          controller_(paths_.dataRoot) {
        controller_.setReportVisibility(preferences_.reports);
        normalFont_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        headingFont_ = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        resultsCanvasData_.normal = normalFont_;
        resultsCanvasData_.heading = headingFont_;
    }

    ~ApplicationWindow() {
        analysisWindow_.close();
        controller_.shutdown();
        if (headingFont_)
            DeleteObject(headingFont_);
    }

    bool create(int showCommand) {
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        int width = 820;
        int height = 700;
        if (preferences_.window) {
            x = preferences_.window->x;
            y = preferences_.window->y;
            width = preferences_.window->width;
            height = preferences_.window->height;
        }
        window_ = CreateWindowExW(
            0, guiMainWindowClassName, L"Starcraft Mechanics Profiler",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, nullptr,
            nullptr, instance_, this);
        if (!window_)
            return false;
        controller_.setStateChanged([window = window_]() {
            if (IsWindow(window))
                PostMessageW(window, controllerChangedMessage, 0, 0);
        });
        addTrayIcon();
        ShowWindow(window_, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
        UpdateWindow(window_);
        SetTimer(window_, uiTimer, 1000, nullptr);
        refreshState();
        return true;
    }

    int run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<int>(message.wParam);
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam,
                                            LPARAM lParam) {
        ApplicationWindow* self = reinterpret_cast<ApplicationWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ApplicationWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handleMessage(message, wParam, lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
    }

  private:
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (showExistingInstanceMessage_ != 0 &&
            message == showExistingInstanceMessage_) {
            restoreWindow();
            return 0;
        }
        if (message == taskbarCreatedMessage_) {
            addTrayIcon();
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            if (!createPages())
                return -1;
            loadSettingsControls();
            updateResults();
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                if (minimizeAction(preferences_.minimizeToTray) ==
                    MainWindowAction::HideToTray)
                    ShowWindow(window_, SW_HIDE);
                return 0;
            }
            layoutPages(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {700, 580};
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == IdTabs && header->code == TCN_SELCHANGE)
                showSelectedPage();
            return 0;
        }
        case WM_COMMAND:
            handleCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_CLOSE:
            if (!exiting_ && closeAction(preferences_.minimizeToTray) ==
                                 MainWindowAction::HideToTray) {
                ShowWindow(window_, SW_HIDE);
                return 0;
            }
            beginExit();
            return 0;
        case WM_TIMER:
            if (wParam == uiTimer) {
                controller_.reapFinished();
                refreshStarCraftStatus();
            }
            return 0;
        case controllerChangedMessage:
            refreshState();
            return 0;
        case trayMessage:
            handleTrayMessage(lParam);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam)
                beginExit();
            return 0;
        case WM_DESTROY:
            KillTimer(window_, uiTimer);
            removeTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    bool createPages() {
        tabs_ = createControl(window_, WC_TABCONTROLW, L"", WS_TABSTOP, IdTabs);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        std::array<wchar_t*, 4> names{
            const_cast<wchar_t*>(L"Main"), const_cast<wchar_t*>(L"Results"),
            const_cast<wchar_t*>(L"Settings"), const_cast<wchar_t*>(L"About")};
        for (int index = 0; index < static_cast<int>(names.size()); ++index) {
            item.pszText = names[static_cast<std::size_t>(index)];
            TabCtrl_InsertItem(tabs_, index, &item);
        }
        if (!createPageContainers(pages_, [this]() {
                return createPageContainer(window_, instance_);
            }))
            return false;
        createMainPage();
        createResultsPage();
        createSettingsPage();
        createAboutPage();
        showSelectedPage();
        return true;
    }

    void createMainPage() {
        const HWND page = pages_[0];
        auto title = createControl(page, L"STATIC", L"Profiler status", SS_LEFT, 0);
        setControlFont(title, headingFont_);
        mainTitle_ = title;
        statusMode_ = createControl(page, L"STATIC", L"Idle", SS_LEFT, IdStatusMode);
        statusActivity_ = createControl(page, L"STATIC", L"Idle", SS_LEFT, IdStatusActivity);
        statusStarCraft_ = createControl(page, L"STATIC", L"Not foreground", SS_LEFT, IdStatusStarCraft);
        statusSession_ = createControl(page, L"STATIC", L"No completed games", SS_LEFT, IdStatusSession);
        statusDetail_ = createControl(page, L"STATIC", L"Profiler is idle", SS_LEFT, IdStatusDetail);
        statusDataFolder_ = createControl(page, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS,
                                          IdStatusDataFolder);
        SetWindowTextW(statusDataFolder_, paths_.dataRoot.c_str());
        automaticButton_ = createControl(page, L"BUTTON", L"Turn automatic detector on",
                                         BS_PUSHBUTTON | WS_TABSTOP, IdAutomaticToggle);
        debugButton_ = createControl(page, L"BUTTON", L"Test live detection",
                                     BS_PUSHBUTTON | WS_TABSTOP, IdDebugToggle);
        calibrateButton_ = createControl(page, L"BUTTON", L"Calibrate minimap",
                                         BS_PUSHBUTTON | WS_TABSTOP, IdCalibrate);
        openDataButton_ = createControl(page, L"BUTTON", L"Open data folder",
                                        BS_PUSHBUTTON | WS_TABSTOP, IdOpenData);
        showResultsButton_ = createControl(page, L"BUTTON", L"View latest results",
                                           BS_PUSHBUTTON | WS_TABSTOP, IdShowResults);
        exitButton_ = createControl(page, L"BUTTON", L"Exit",
                                    BS_PUSHBUTTON | WS_TABSTOP, IdExit);
        debugLog_ = createControl(page, L"EDIT", L"Live detection events appear here.",
                                  ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                      WS_VSCROLL | WS_BORDER,
                                  IdDebugLog, WS_EX_CLIENTEDGE);
    }

    void createResultsPage() {
        const HWND page = pages_[1];
        resultsSource_ = createControl(page, WC_COMBOBOXW, L"",
                                       CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                       IdResultsSource);
        ComboBox_AddString(resultsSource_, L"Latest game");
        ComboBox_AddString(resultsSource_, L"Current session");
        ComboBox_SetCurSel(resultsSource_, 0);
        resultsOpenFile_ = createControl(page, L"BUTTON", L"Open result file",
                                         BS_PUSHBUTTON | WS_TABSTOP, IdResultsOpenFile);
        resultsOpenSession_ = createControl(page, L"BUTTON", L"Open latest session summary",
                                            BS_PUSHBUTTON | WS_TABSTOP, IdResultsOpenSession);
        resultsExport_ = createControl(page, L"BUTTON", L"Export latest CSV",
                                       BS_PUSHBUTTON | WS_TABSTOP, IdResultsExport);
        resultsAnalysis_ = createControl(page, L"BUTTON", L"Open Analysis / Timeline",
                                         BS_PUSHBUTTON | WS_TABSTOP, IdResultsAnalysis);
        resultsCanvas_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, resultsCanvasClass, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER, 0, 0, 10, 10, page,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdResultsCanvas)), instance_,
            &resultsCanvasData_);
    }

    HWND settingLabel(const wchar_t* text) {
        return createControl(pages_[2], L"STATIC", text, SS_LEFT, 0);
    }

    HWND settingCheck(const wchar_t* text, int id) {
        return createControl(pages_[2], L"BUTTON", text,
                             BS_AUTOCHECKBOX | WS_TABSTOP, id);
    }

    void createSettingsPage() {
        settingsReportTitle_ = settingLabel(L"Reported statistics");
        setControlFont(settingsReportTitle_, headingFont_);
        settingsCamera_ = settingCheck(L"Camera navigation", IdSettingCamera);
        settingsWorker_ = settingCheck(L"Worker macro cycles", IdSettingWorker);
        settingsArmy_ = settingCheck(L"Army macro cycles", IdSettingArmy);
        settingsStyles_ = settingCheck(L"Macro access styles", IdSettingStyles);
        settingsArmyGroups_ = settingCheck(L"Army control-group management", IdSettingArmyGroups);
        settingsScouting_ = settingCheck(L"Scouting-unit activity", IdSettingScouting);
        settingsSelectAll_ = createControl(pages_[2], L"BUTTON", L"Select all",
                                           BS_PUSHBUTTON | WS_TABSTOP, IdSettingSelectAll);

        settingsApplicationTitle_ = settingLabel(L"Application");
        setControlFont(settingsApplicationTitle_, headingFont_);
        settingsMinimize_ = settingCheck(L"Minimize to tray", IdSettingMinimize);

        settingsAdvancedTitle_ = settingLabel(L"Advanced");
        setControlFont(settingsAdvancedTitle_, headingFont_);
        settingsCalibrate_ = createControl(pages_[2], L"BUTTON", L"Calibrate minimap",
                                           BS_PUSHBUTTON | WS_TABSTOP, IdSettingCalibrate);
        settingsOpenConfig_ = createControl(pages_[2], L"BUTTON", L"Open config.json",
                                            BS_PUSHBUTTON | WS_TABSTOP, IdSettingOpenConfig);
        settingsSave_ = createControl(pages_[2], L"BUTTON", L"Save settings",
                                      BS_DEFPUSHBUTTON | WS_TABSTOP, IdSettingSave);
    }

    void createAboutPage() {
        const std::wstring about =
            L"Starcraft Mechanics Profiler " L"" STARCRAFT_MECHANICS_PROFILER_VERSION L"\r\n\r\n"
            L"A lightweight mechanical profiler for StarCraft: Remastered. It combines physical "
            L"Raw Input telemetry with replay-derived context. QPC physical-input timestamps are "
            L"authoritative for mechanical timing; replay frames identify semantic context.\r\n\r\n"
            L"WHAT THE STATISTICS MEAN\r\n\r\n"
            L"Camera navigation\r\nDistribution of detected control-group jumps, location-hotkey "
            L"jumps, minimap jumps, and edge pans. These are physical-input and screen-region "
            L"detections; they do not judge navigation quality.\r\n\r\n"
            L"Worker / Army macro-cycle duration\r\nTime from beginning access to a production "
            L"context through the first production attempt in the final context of the cycle. "
            L"Lower values describe faster observed execution, not better strategy. Product type "
            L"and context identity use replay correlation; timing uses physical QPC.\r\n\r\n"
            L"Production response latency\r\nPhysical time from establishing a production context to "
            L"the first production-key attempt. Replay data validates production meaning where "
            L"available.\r\n\r\n"
            L"Macro access style\r\nHow production contexts were reached: control-group-only, "
            L"location-hotkey plus click, control-group camera center plus click, mixed, or other. "
            L"This is descriptive and does not score the chosen method.\r\n\r\n"
            L"Army control-group management\r\nHow replay-confirmed non-production groups were assigned "
            L"or incrementally expanded, including physical selection formation such as box select "
            L"or Ctrl-click type selection. Ambiguous and excluded groups are not headline army edits.\r\n\r\n"
            L"Scouting-unit activity\r\nObserved duration from a heuristic early scouting-group "
            L"assignment to its final attributed physical right-click command. This is NOT literal "
            L"unit survival or death time. Left-click selection changes and later group overwrites "
            L"end attribution.\r\n\r\n"
            L"COMMAND-LINE REFERENCE\r\n"
            L"record [--debug-navigation] [--debug-regions] [--show-raw-events] [--save-raw] "
            L"[--verbose] [--quiet]\r\n"
            L"auto [same options as record]\r\n"
            L"debug | calibrate | config\r\n"
            L"summary <latest|session-id>\r\n"
            L"compare <session-id> <session-id> | compare last <N>\r\n"
            L"export <latest|session-id> --csv\r\n\r\n"
            L"The profiler does not read game memory, inject input, or provide strategic-quality "
            L"judgments.";
        aboutText_ = createControl(pages_[3], L"EDIT", about.c_str(),
                                   ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                                       WS_VSCROLL | WS_BORDER,
                                   IdAboutText, WS_EX_CLIENTEDGE);
    }

    void layoutPages(int width, int height) {
        if (!tabs_)
            return;
        move(tabs_, 8, 8, width - 16, height - 16);
        RECT pageRect{0, 0, width - 16, height - 16};
        TabCtrl_AdjustRect(tabs_, FALSE, &pageRect);
        MapWindowPoints(tabs_, window_, reinterpret_cast<POINT*>(&pageRect), 2);
        for (const auto page : pages_)
            move(page, pageRect.left, pageRect.top, pageRect.right - pageRect.left,
                 pageRect.bottom - pageRect.top);
        layoutMain(pageRect.right - pageRect.left, pageRect.bottom - pageRect.top);
        layoutResults(pageRect.right - pageRect.left, pageRect.bottom - pageRect.top);
        layoutSettings(pageRect.right - pageRect.left, pageRect.bottom - pageRect.top);
        move(aboutText_, 18, 16, pageRect.right - pageRect.left - 36,
             pageRect.bottom - pageRect.top - 32);
    }

    void layoutMain(int width, int height) {
        const int x = 22;
        const int contentWidth = width - 44;
        move(mainTitle_, x, 16, contentWidth, 28);
        constexpr int labelWidth = 180;
        auto statusRow = [&](const wchar_t* label, HWND value, int y) {
            HDC dc = GetDC(pages_[0]);
            (void)dc;
            RECT rect{x, y, x + labelWidth, y + 22};
            DrawTextW(dc, label, -1, &rect, DT_CALCRECT);
            ReleaseDC(pages_[0], dc);
            move(value, x + labelWidth, y, contentWidth - labelWidth, 22);
        };
        // Static row captions are painted by dedicated controls created lazily once.
        if (!mainStatusLabels_[0]) {
            const std::array<const wchar_t*, 6> labels{
                L"Mode", L"Activity", L"StarCraft", L"Current session", L"Detail",
                L"Data folder"};
            for (std::size_t index = 0; index < labels.size(); ++index)
                mainStatusLabels_[index] = createControl(pages_[0], L"STATIC", labels[index], SS_LEFT, 0);
        }
        const std::array<HWND, 6> values{statusMode_, statusActivity_, statusStarCraft_,
                                         statusSession_, statusDetail_, statusDataFolder_};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const int rowY = 52 + static_cast<int>(index) * 25;
            move(mainStatusLabels_[index], x, rowY, labelWidth - 8, 22);
            statusRow(L"", values[index], rowY);
        }
        const int buttonY = 212;
        const int gap = 8;
        const int buttonWidth = (contentWidth - gap * 2) / 3;
        move(automaticButton_, x, buttonY, buttonWidth, 30);
        move(debugButton_, x + buttonWidth + gap, buttonY, buttonWidth, 30);
        move(calibrateButton_, x + (buttonWidth + gap) * 2, buttonY, buttonWidth, 30);
        move(openDataButton_, x, buttonY + 38, buttonWidth, 30);
        move(showResultsButton_, x + buttonWidth + gap, buttonY + 38, buttonWidth, 30);
        move(exitButton_, x + (buttonWidth + gap) * 2, buttonY + 38, buttonWidth, 30);
        move(debugLog_, x, buttonY + 82, contentWidth,
             std::max(80, height - (buttonY + 102)));
    }

    void layoutResults(int width, int height) {
        const int x = 18;
        move(resultsSource_, x, 14, 170, 200);
        move(resultsOpenFile_, 198, 14, 135, 28);
        move(resultsOpenSession_, 341, 14, 190, 28);
        move(resultsExport_, 539, 14, std::max(120, width - 557), 28);
        move(resultsAnalysis_, x, 50, 190, 28);
        move(resultsCanvas_, x, 86, width - 36, height - 104);
    }

    void layoutSettings(int width, int height) {
        const int left = 20;
        const int contentWidth = width - left * 2;
        move(settingsReportTitle_, left, 14, contentWidth, 27);
        const std::array<HWND, 6> checks{
            settingsCamera_, settingsWorker_, settingsArmy_, settingsStyles_,
            settingsArmyGroups_, settingsScouting_};
        for (std::size_t index = 0; index < checks.size(); ++index)
            move(checks[index], left, 50 + static_cast<int>(index) * 32,
                 contentWidth, 24);
        move(settingsSelectAll_, left, 250, 130, 29);

        move(settingsApplicationTitle_, left, 304, contentWidth, 27);
        move(settingsMinimize_, left, 338, contentWidth, 28);

        move(settingsAdvancedTitle_, left, 390, contentWidth, 27);
        move(settingsCalibrate_, left, 426, 160, 29);
        move(settingsOpenConfig_, left + 168, 426, 160, 29);
        move(settingsSave_, left, std::min(486, height - 52), 220, 32);
    }

    void showSelectedPage() {
        const int selected = std::max(0, TabCtrl_GetCurSel(tabs_));
        for (std::size_t index = 0; index < pages_.size(); ++index)
            ShowWindow(pages_[index], static_cast<int>(index) == selected ? SW_SHOW : SW_HIDE);
        if (selected == 1)
            updateResults();
    }

    void selectPage(int index) {
        TabCtrl_SetCurSel(tabs_, index);
        showSelectedPage();
    }

    void handleCommand(int id, int notification) {
        if (id == IdResultsSource && notification == CBN_SELCHANGE) {
            updateResults();
            return;
        }
        switch (id) {
        case IdAutomaticToggle: toggleAutomatic(); break;
        case IdDebugToggle: toggleDebug(); break;
        case IdCalibrate:
        case IdSettingCalibrate: startCalibration(); break;
        case IdOpenData: openPath(paths_.dataRoot); break;
        case IdShowResults: selectPage(1); break;
        case IdExit:
        case IdTrayExit: beginExit(); break;
        case IdResultsOpenFile: openLatestResult(); break;
        case IdResultsOpenSession: openLatestSessionSummary(); break;
        case IdResultsExport: exportLatestCsv(); break;
        case IdResultsAnalysis: openAnalysis(); break;
        case IdSettingSelectAll: setAllReportChecks(true); break;
        case IdSettingSave: saveSettings(); break;
        case IdSettingOpenConfig: openPath(paths_.config); break;
        case IdTrayOpen: restoreWindow(); break;
        case IdTrayToggle: toggleAutomatic(); break;
        case IdTrayLatest:
            restoreWindow();
            selectPage(1);
            break;
        default: break;
        }
    }

    void toggleAutomatic() {
        const auto state = controller_.snapshot();
        if (state.workerRunning) {
            if (state.mode == ApplicationMode::Automatic)
                controller_.stopCurrent();
            return;
        }
        try {
            config_ = Config::loadOrCreate(paths_.config);
            if (!config_.calibratedMinimap) {
                MessageBoxW(window_, L"Calibrate the minimap before enabling automatic detection.",
                            L"Calibration required", MB_OK | MB_ICONINFORMATION);
                return;
            }
            controller_.setReportVisibility(preferences_.reports);
            (void)controller_.startAutomatic(config_);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void toggleDebug() {
        const auto state = controller_.snapshot();
        if (state.workerRunning) {
            if (state.mode == ApplicationMode::Debug)
                controller_.stopCurrent();
            return;
        }
        try {
            config_ = Config::loadOrCreate(paths_.config);
            (void)controller_.startDebug(config_);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void startCalibration() {
        if (controller_.snapshot().workerRunning)
            return;
        const int answer = MessageBoxW(
            window_,
            L"After starting, switch to StarCraft. Move to the minimap top-left and press the "
            L"configured capture key, then repeat at the bottom-right.\n\nStart calibration?",
            L"Minimap calibration", MB_YESNO | MB_ICONINFORMATION);
        if (answer != IDYES)
            return;
        try {
            config_ = Config::loadOrCreate(paths_.config);
            (void)controller_.startCalibration(config_, paths_.config);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void refreshState() {
        const auto state = controller_.snapshot();
        setWindowText(statusMode_, applicationModeName(state.mode));
        setWindowText(statusActivity_, profilerActivityName(state.activity));
        setWindowText(statusDetail_, state.detail);
        setWindowText(statusSession_,
                      std::to_string(state.currentSession.games) + " completed game(s)");
        const bool automatic = state.workerRunning && state.mode == ApplicationMode::Automatic;
        const bool debug = state.workerRunning && state.mode == ApplicationMode::Debug;
        SetWindowTextW(automaticButton_, automatic ? L"Turn automatic detector off"
                                                   : L"Turn automatic detector on");
        SetWindowTextW(debugButton_, debug ? L"Stop live detection"
                                           : L"Test live detection");
        EnableWindow(automaticButton_, !state.workerRunning || automatic);
        EnableWindow(debugButton_, !state.workerRunning || debug);
        EnableWindow(calibrateButton_, !state.workerRunning);
        EnableWindow(settingsCalibrate_, !state.workerRunning);
        EnableWindow(showResultsButton_, state.latestGame.has_value());
        EnableWindow(resultsOpenFile_, state.latestGame.has_value());
        EnableWindow(resultsAnalysis_, state.latestGame.has_value());
        updateDiagnosticLog(state.diagnostics);
        updateResults();
        updateTrayTooltip(state);
        if (lastMode_ == ApplicationMode::Calibration && state.mode == ApplicationMode::None) {
            try {
                config_ = Config::loadOrCreate(paths_.config);
                loadSettingsControls();
            } catch (...) {
            }
        }
        lastMode_ = state.mode;
    }

    void refreshStarCraftStatus() {
        try {
            ForegroundMatcher foreground(config_.starcraftProcess);
            const bool active = foreground.matches(GetForegroundWindow());
            SetWindowTextW(statusStarCraft_, active ? L"Foreground" : L"Not foreground");
        } catch (...) {
            SetWindowTextW(statusStarCraft_, L"Unavailable");
        }
    }

    void updateDiagnosticLog(const std::vector<std::string>& diagnostics) {
        if (diagnostics.size() == displayedDiagnosticCount_)
            return;
        std::wstring text;
        for (const auto& line : diagnostics) {
            text += wide(line);
            text += L"\r\n";
        }
        if (text.empty())
            text = L"Live detection events appear here.";
        SetWindowTextW(debugLog_, text.c_str());
        SendMessageW(debugLog_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
        SendMessageW(debugLog_, EM_SCROLLCARET, 0, 0);
        displayedDiagnosticCount_ = diagnostics.size();
    }

    void updateResults() {
        if (!resultsCanvas_)
            return;
        const auto state = controller_.snapshot();
        const int source = std::max(0, static_cast<int>(ComboBox_GetCurSel(resultsSource_)));
        if (source == 0) {
            if (state.latestGame) {
                resultsModel_ = deriveGameResults(*state.latestGame, preferences_.reports);
            } else {
                resultsModel_ = {"Latest Game", "No completed result is available",
                                 {{"status", "Results", {{"Status", "No result available"}}}}};
            }
        } else {
            resultsModel_ = deriveSessionResults(state.currentSession, preferences_.reports);
        }
        resultsCanvasData_.model = &resultsModel_;
        resultsCanvasData_.scroll = 0;
        InvalidateRect(resultsCanvas_, nullptr, TRUE);
    }

    void loadSettingsControls() {
        setChecked(settingsCamera_, preferences_.reports.cameraNavigation);
        setChecked(settingsWorker_, preferences_.reports.workerMacroCycles);
        setChecked(settingsArmy_, preferences_.reports.armyMacroCycles);
        setChecked(settingsStyles_, preferences_.reports.macroAccessStyles);
        setChecked(settingsArmyGroups_, preferences_.reports.armyControlGroupManagement);
        setChecked(settingsScouting_, preferences_.reports.scoutingUnitActivity);
        setChecked(settingsMinimize_, preferences_.minimizeToTray);
    }

    void saveSettings() {
        try {
            preferences_.reports.cameraNavigation = checked(settingsCamera_);
            preferences_.reports.workerMacroCycles = checked(settingsWorker_);
            preferences_.reports.armyMacroCycles = checked(settingsArmy_);
            preferences_.reports.macroAccessStyles = checked(settingsStyles_);
            preferences_.reports.armyControlGroupManagement = checked(settingsArmyGroups_);
            preferences_.reports.scoutingUnitActivity = checked(settingsScouting_);
            preferences_.minimizeToTray = checked(settingsMinimize_);
            preferences_.save(paths_.preferences);
            controller_.setReportVisibility(preferences_.reports);
            updateResults();
            MessageBoxW(window_, L"Settings saved.", L"Starcraft Mechanics Profiler",
                        MB_OK | MB_ICONINFORMATION);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void setAllReportChecks(bool value) {
        setChecked(settingsCamera_, value);
        setChecked(settingsWorker_, value);
        setChecked(settingsArmy_, value);
        setChecked(settingsStyles_, value);
        setChecked(settingsArmyGroups_, value);
        setChecked(settingsScouting_, value);
    }

    void openPath(const std::filesystem::path& path) {
        if (path.empty() || !std::filesystem::exists(path)) {
            const std::wstring message = L"The requested file or folder does not exist:\n" +
                                         path.wstring();
            MessageBoxW(window_, message.c_str(),
                        L"Unable to open", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", path.c_str(), nullptr,
                                                    nullptr, SW_SHOWNORMAL)) <= 32) {
            const std::wstring message = L"Windows could not open:\n" + path.wstring();
            MessageBoxW(window_, message.c_str(),
                        L"Unable to open", MB_OK | MB_ICONERROR);
        }
    }

    void openLatestResult() {
        const auto state = controller_.snapshot();
        if (state.latestGamePath.empty())
            return;
        openPath(state.latestGamePath);
    }

    void openLatestSessionSummary() {
        const auto path = findLatestAutomaticSessionSummary(paths_.sessions);
        if (!path) {
            MessageBoxW(window_, L"No saved automatic session summary is available yet.",
                        L"Session summary", MB_OK | MB_ICONINFORMATION);
            return;
        }
        openPath(*path);
    }

    void exportLatestCsv() {
        try {
            const auto exported = exportSessionCsv(paths_.sessions, paths_.exports,
                                                   "latest");
            std::wstring message = L"Exported to:\n" + exported.wstring();
            MessageBoxW(window_, message.c_str(), L"CSV export complete",
                        MB_OK | MB_ICONINFORMATION);
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void openAnalysis() {
        const auto state = controller_.snapshot();
        if (state.latestGamePath.empty()) {
            MessageBoxW(window_, L"No completed game is available to analyze yet.",
                        L"Analysis", MB_OK | MB_ICONINFORMATION);
            return;
        }
        try {
            auto model = loadGameAnalysisVisualizationModel(state.latestGamePath);
            if (!model.navLoaded && !model.jsonLoaded) {
                std::wstring message = L"The paired .nav and .json files could not be loaded.\n\n";
                message += wide(model.navigationStatus.reason);
                message += L"\n";
                message += wide(model.workerMacroStatus.reason);
                MessageBoxW(window_, message.c_str(), L"Analysis unavailable",
                            MB_OK | MB_ICONINFORMATION);
                return;
            }
            analysisWindow_.open(window_, std::move(model));
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void restoreWindow() {
        ShowWindow(window_, SW_RESTORE);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void handleTrayMessage(LPARAM lParam) {
        const UINT event = LOWORD(lParam);
        if (event == WM_LBUTTONDBLCLK || event == NIN_SELECT || event == NIN_KEYSELECT) {
            restoreWindow();
            return;
        }
        if (event != WM_RBUTTONUP && event != WM_CONTEXTMENU)
            return;
        const auto state = controller_.snapshot();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_DEFAULT, IdTrayOpen, L"Open");
        std::wstring status = L"Status: ";
        status += wide(profilerActivityName(state.activity));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IdTrayStatus, status.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const bool automatic = state.workerRunning && state.mode == ApplicationMode::Automatic;
        AppendMenuW(menu, MF_STRING | (state.workerRunning && !automatic ? MF_GRAYED : 0),
                    IdTrayToggle, automatic ? L"Stop automatic detector"
                                            : L"Start automatic detector");
        AppendMenuW(menu, MF_STRING | (state.latestGame ? 0 : MF_GRAYED), IdTrayLatest,
                    L"Open latest result");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IdTrayExit, L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                       cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
    }

    void addTrayIcon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = trayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = trayMessage;
        data.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        wcscpy_s(data.szTip, L"Starcraft Mechanics Profiler - Idle");
        Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        trayAdded_ = true;
    }

    void updateTrayTooltip(const ApplicationSnapshot& state) {
        if (!trayAdded_)
            return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = trayIconId;
        data.uFlags = NIF_TIP;
        std::wstring tooltip = L"Starcraft Mechanics Profiler - ";
        tooltip += wide(profilerActivityName(state.activity));
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void removeTrayIcon() {
        if (!trayAdded_)
            return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = trayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayAdded_ = false;
    }

    void persistWindowPlacement() noexcept {
        try {
            if (!IsIconic(window_)) {
                RECT rect{};
                if (GetWindowRect(window_, &rect)) {
                    GuiWindowPlacement placement{rect.left, rect.top,
                                                 rect.right - rect.left,
                                                 rect.bottom - rect.top};
                    if (placement.valid())
                        preferences_.window = placement;
                }
            }
            preferences_.save(paths_.preferences);
        } catch (...) {
        }
    }

    void beginExit() {
        if (exiting_)
            return;
        exiting_ = true;
        persistWindowPlacement();
        analysisWindow_.close();
        controller_.shutdown();
        DestroyWindow(window_);
    }

    void showError(const std::string& message) {
        const auto converted = wide(message);
        MessageBoxW(window_, converted.c_str(), L"Starcraft Mechanics Profiler",
                    MB_OK | MB_ICONERROR);
    }

    HINSTANCE instance_{};
    GuiApplicationPaths paths_;
    Config config_;
    GuiPreferences preferences_;
    ApplicationController controller_;
    AnalysisWindow analysisWindow_;
    HWND window_{};
    HWND tabs_{};
    std::array<HWND, 4> pages_{};
    HFONT normalFont_{};
    HFONT headingFont_{};
    HWND mainTitle_{};
    std::array<HWND, 6> mainStatusLabels_{};
    HWND statusMode_{};
    HWND statusActivity_{};
    HWND statusStarCraft_{};
    HWND statusSession_{};
    HWND statusDetail_{};
    HWND statusDataFolder_{};
    HWND automaticButton_{};
    HWND debugButton_{};
    HWND calibrateButton_{};
    HWND openDataButton_{};
    HWND showResultsButton_{};
    HWND exitButton_{};
    HWND debugLog_{};
    HWND resultsSource_{};
    HWND resultsOpenFile_{};
    HWND resultsOpenSession_{};
    HWND resultsExport_{};
    HWND resultsAnalysis_{};
    HWND resultsCanvas_{};
    ResultsCanvasData resultsCanvasData_{};
    ResultsViewModel resultsModel_{};
    HWND settingsCalibrate_{};
    HWND settingsOpenConfig_{};
    HWND settingsReportTitle_{};
    HWND settingsApplicationTitle_{};
    HWND settingsAdvancedTitle_{};
    HWND settingsCamera_{};
    HWND settingsWorker_{};
    HWND settingsArmy_{};
    HWND settingsStyles_{};
    HWND settingsArmyGroups_{};
    HWND settingsScouting_{};
    HWND settingsMinimize_{};
    HWND settingsSelectAll_{};
    HWND settingsSave_{};
    HWND aboutText_{};
    std::size_t displayedDiagnosticCount_{};
    ApplicationMode lastMode_{ApplicationMode::None};
    bool trayAdded_{};
    bool exiting_{};
    UINT taskbarCreatedMessage_{RegisterWindowMessageW(L"TaskbarCreated")};
    UINT showExistingInstanceMessage_{showExistingGuiInstanceMessage()};
};

} // namespace

int runWindowsApplication(HINSTANCE instance,
                          const GuiApplicationPaths& paths,
                          int showCommand) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW canvasClass{};
    canvasClass.cbSize = sizeof(canvasClass);
    canvasClass.lpfnWndProc = resultsCanvasProcedure;
    canvasClass.hInstance = instance;
    canvasClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    canvasClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    canvasClass.lpszClassName = resultsCanvasClass;
    if (!RegisterClassExW(&canvasClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;
    if (!registerPageContainerClass(instance))
        return 1;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ApplicationWindow::windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = guiMainWindowClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    ApplicationWindow application(instance, paths);
    if (!application.create(showCommand))
        return 1;
    return application.run();
}

} // namespace smp

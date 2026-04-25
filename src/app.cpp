#include "app.h"

#include "delete.h"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <exception>
#include <sstream>
#include <unordered_set>

namespace {

constexpr int kIdScan = 1001;
constexpr int kIdStop = 1002;
constexpr int kIdAuto = 1003;
constexpr int kIdDelete = 1004;
constexpr int kIdScanAll = 1005;
constexpr int kIdGroups = 2001;
constexpr int kIdFiles = 2002;
constexpr UINT kMsgScanProgress = WM_APP + 1;
constexpr UINT kMsgScanDone = WM_APP + 2;
constexpr UINT_PTR kScanWatchdogTimer = 3001;

const wchar_t* kMainClass = L"DuplicateImageFinder.MainWindow";
const wchar_t* kPreviewClass = L"DuplicateImageFinder.PreviewPane";
constexpr COLORREF kWindowBg = RGB(246, 247, 249);
constexpr COLORREF kPanelBg = RGB(255, 255, 255);
constexpr COLORREF kText = RGB(31, 41, 55);
constexpr COLORREF kMutedText = RGB(95, 106, 122);

std::wstring FormatFileTime(const FILETIME& file_time) {
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&file_time, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return L"";
    }

    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond);
    return buffer;
}

std::wstring HashShort(const std::wstring& hash) {
    if (hash.size() <= 16) {
        return hash;
    }
    return hash.substr(0, 16) + L"...";
}

void InsertColumn(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void SetItemText(HWND list, int item, int sub_item, const std::wstring& text) {
    ListView_SetItemText(list, item, sub_item, const_cast<wchar_t*>(text.c_str()));
}

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

int MessageBoxQuestion(HWND hwnd, const std::wstring& text, const std::wstring& title) {
    return MessageBoxW(hwnd, text.c_str(), title.c_str(), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
}

std::wstring WidenErrorText(const char* text) {
    if (!text || *text == '\0') {
        return L"未知错误";
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    UINT code_page = CP_UTF8;
    if (length <= 0) {
        code_page = CP_ACP;
        length = MultiByteToWideChar(code_page, 0, text, -1, nullptr, 0);
    }
    if (length <= 0) {
        return L"未知错误";
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(code_page, 0, text, -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {}

App::~App() {
    StopScan();
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    if (gdiplus_token_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
    }
    if (ui_font_) {
        DeleteObject(ui_font_);
    }
    if (title_font_) {
        DeleteObject(title_font_);
    }
    if (small_font_) {
        DeleteObject(small_font_);
    }
    if (window_brush_) {
        DeleteObject(window_brush_);
    }
    if (preview_brush_) {
        DeleteObject(preview_brush_);
    }
}

bool App::Initialize(int) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiplus_input{};
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
    }

    RegisterWindowClasses();

    hwnd_ = CreateWindowExW(0, kMainClass, L"重复图片查找器",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1120, 740,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }

    ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd_);
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::RegisterWindowClasses() {
    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.hInstance = instance_;
    main_class.lpszClassName = kMainClass;
    main_class.lpfnWndProc = App::WindowProc;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    main_class.hIconSm = reinterpret_cast<HICON>(
        LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&main_class);

    WNDCLASSEXW preview_class{};
    preview_class.cbSize = sizeof(preview_class);
    preview_class.hInstance = instance_;
    preview_class.lpszClassName = kPreviewClass;
    preview_class.lpfnWndProc = App::PreviewProc;
    preview_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    preview_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&preview_class);
}

void App::CreateUiResources() {
    if (!ui_font_) {
        ui_font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    }
    if (!title_font_) {
        title_font_ = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    }
    if (!small_font_) {
        small_font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    }
    if (!window_brush_) {
        window_brush_ = CreateSolidBrush(kWindowBg);
    }
    if (!preview_brush_) {
        preview_brush_ = CreateSolidBrush(kPanelBg);
    }
}

void App::ApplyControlFonts() {
    const HWND controls[] = {
        title_label_, subtitle_label_, scan_button_, stop_button_, auto_button_, delete_button_,
        scan_all_button_, status_label_, group_list_, file_list_
    };
    for (HWND control : controls) {
        if (control) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        }
    }
    SendMessageW(title_label_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    SendMessageW(subtitle_label_, WM_SETFONT, reinterpret_cast<WPARAM>(small_font_), TRUE);
    SendMessageW(status_label_, WM_SETFONT, reinterpret_cast<WPARAM>(small_font_), TRUE);
}

void App::CreateControls() {
    CreateUiResources();

    title_label_ = CreateWindowExW(0, WC_STATICW, L"重复图片查找器",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 100, 28, hwnd_, nullptr, instance_, nullptr);
    subtitle_label_ = CreateWindowExW(0, WC_STATICW, L"默认扫描图片、桌面、下载等常用目录；需要彻底查找时可扫描全部硬盘。",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 100, 20, hwnd_, nullptr, instance_, nullptr);

    scan_button_ = CreateWindowExW(0, WC_BUTTONW, L"扫描常用目录",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 120, 34, hwnd_, ControlId(kIdScan), instance_, nullptr);
    scan_all_button_ = CreateWindowExW(0, WC_BUTTONW, L"扫描全部硬盘",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 0, 120, 34, hwnd_, ControlId(kIdScanAll), instance_, nullptr);
    stop_button_ = CreateWindowExW(0, WC_BUTTONW, L"停止",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 80, 30, hwnd_, ControlId(kIdStop), instance_, nullptr);
    auto_button_ = CreateWindowExW(0, WC_BUTTONW, L"自动勾选",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 100, 30, hwnd_, ControlId(kIdAuto), instance_, nullptr);
    delete_button_ = CreateWindowExW(0, WC_BUTTONW, L"删除所选",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 100, 30, hwnd_, ControlId(kIdDelete), instance_, nullptr);

    progress_bar_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                    WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                                    0, 0, 100, 18, hwnd_, nullptr, instance_, nullptr);
    status_label_ = CreateWindowExW(0, WC_STATICW, L"点击“扫描常用目录”快速查找重复图片；全盘扫描会慢很多。",
                                    WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                    0, 0, 100, 22, hwnd_, nullptr, instance_, nullptr);

    group_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                  0, 0, 100, 100, hwnd_, ControlId(kIdGroups), instance_, nullptr);
    file_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                 0, 0, 100, 100, hwnd_, ControlId(kIdFiles), instance_, nullptr);
    preview_ = CreateWindowExW(WS_EX_CLIENTEDGE, kPreviewClass, nullptr,
                               WS_CHILD | WS_VISIBLE,
                               0, 0, 100, 100, hwnd_, nullptr, instance_, this);

    SetWindowTheme(group_list_, L"Explorer", nullptr);
    SetWindowTheme(file_list_, L"Explorer", nullptr);
    ListView_SetBkColor(group_list_, kPanelBg);
    ListView_SetBkColor(file_list_, kPanelBg);
    ListView_SetTextBkColor(group_list_, kPanelBg);
    ListView_SetTextBkColor(file_list_, kPanelBg);
    ListView_SetTextColor(group_list_, kText);
    ListView_SetTextColor(file_list_, kText);
    ListView_SetExtendedListViewStyle(group_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetExtendedListViewStyle(file_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

    InsertColumn(group_list_, 0, 70, L"组");
    InsertColumn(group_list_, 1, 70, L"数量");
    InsertColumn(group_list_, 2, 110, L"单文件大小");
    InsertColumn(group_list_, 3, 120, L"可释放");
    InsertColumn(group_list_, 4, 160, L"哈希");

    InsertColumn(file_list_, 0, 470, L"文件路径");
    InsertColumn(file_list_, 1, 100, L"大小");
    InsertColumn(file_list_, 2, 150, L"创建时间");

    ApplyControlFonts();
    UpdateButtons();
}

void App::LayoutControls() {
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const int margin = 16;
    const int top = 14;
    const int button_h = 34;
    int x = margin;

    MoveWindow(title_label_, margin, top, 260, 32, TRUE);
    MoveWindow(subtitle_label_, margin, top + 34, rect.right - rect.left - margin * 2, 22, TRUE);

    const int toolbar_top = top + 68;
    MoveWindow(scan_button_, x, toolbar_top, 142, button_h, TRUE);
    x += 154;
    MoveWindow(scan_all_button_, x, toolbar_top, 142, button_h, TRUE);
    x += 154;
    MoveWindow(stop_button_, x, toolbar_top, 86, button_h, TRUE);
    x += 98;
    MoveWindow(auto_button_, x, toolbar_top, 110, button_h, TRUE);
    x += 122;
    MoveWindow(delete_button_, x, toolbar_top, 110, button_h, TRUE);

    MoveWindow(progress_bar_, margin, toolbar_top + 44, rect.right - rect.left - margin * 2, 16, TRUE);
    MoveWindow(status_label_, margin, toolbar_top + 68, rect.right - rect.left - margin * 2, 22, TRUE);

    const int content_top = toolbar_top + 104;
    const int content_h = rect.bottom - content_top - margin;
    const int content_w = rect.right - rect.left - margin * 2;
    const int group_w = std::min(520, std::max(430, content_w * 44 / 100));
    const int right_x = margin + group_w + margin;
    const int right_w = std::max(200, content_w - group_w - margin);
    const int file_h = std::max(180, content_h * 55 / 100);
    const int preview_y = content_top + file_h + margin;
    const int preview_h = std::max(120, content_h - file_h - margin);

    MoveWindow(group_list_, margin, content_top, group_w, content_h, TRUE);
    MoveWindow(file_list_, right_x, content_top, right_w, file_h, TRUE);
    MoveWindow(preview_, right_x, preview_y, right_w, preview_h, TRUE);
}

void App::StartScan(bool all_drives) {
    if (scanning_) {
        return;
    }
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }

    SaveCurrentFileChecks();
    groups_.clear();
    current_group_ = -1;
    preview_path_.clear();
    ListView_DeleteAllItems(group_list_);
    ListView_DeleteAllItems(file_list_);
    InvalidateRect(preview_, nullptr, TRUE);

    scanning_ = true;
    exit_when_scan_finishes_ = false;
    stop_requested_ = false;
    cancel_scan_.store(false);
    scan_started_at_ = std::chrono::steady_clock::now();
    last_progress_at_ = scan_started_at_;
    stop_requested_at_ = {};
    SendMessageW(progress_bar_, PBM_SETMARQUEE, TRUE, 30);
    UpdateStatus(all_drives ? L"正在扫描全部本地硬盘，请稍候..." : L"正在扫描常用目录，请稍候...");
    UpdateButtons();
    SetTimer(hwnd_, kScanWatchdogTimer, 1000, nullptr);

    scan_thread_ = std::thread([this, all_drives]() {
        ScanResult result;
        try {
            Scanner scanner;
            auto last_progress = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            auto post_progress = [this, &last_progress](const ScanProgress& progress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_progress < std::chrono::milliseconds(250) && !cancel_scan_.load()) {
                    return;
                }
                last_progress = now;
                auto* posted = new ScanProgress(progress);
                if (!PostMessageW(hwnd_, kMsgScanProgress, 0, reinterpret_cast<LPARAM>(posted))) {
                    delete posted;
                }
            };
            result = all_drives
                ? scanner.ScanAllFixedDrives(cancel_scan_, post_progress)
                : scanner.ScanCommonFolders(cancel_scan_, post_progress);
        } catch (const std::bad_alloc&) {
            result.cancelled = true;
            result.error_message = L"扫描停止：内存不足。";
        } catch (const std::exception& ex) {
            result.cancelled = true;
            result.error_message = L"扫描停止：" + WidenErrorText(ex.what());
        } catch (...) {
            result.cancelled = true;
            result.error_message = L"扫描停止：发生未知错误。";
        }

        if (!result.error_message.empty() && result.progress.current_path.empty()) {
            result.progress.current_path = L"";
        }
        auto* posted = new ScanResult(std::move(result));
        if (!PostMessageW(hwnd_, kMsgScanDone, 0, reinterpret_cast<LPARAM>(posted))) {
            delete posted;
        }
    });
}

void App::StopScan() {
    cancel_scan_.store(true);
}

void App::RequestStop(bool exit_after_stop) {
    if (!scanning_) {
        if (exit_after_stop) {
            DestroyWindow(hwnd_);
        }
        return;
    }

    exit_when_scan_finishes_ = exit_when_scan_finishes_ || exit_after_stop;
    if (!stop_requested_) {
        stop_requested_ = true;
        stop_requested_at_ = std::chrono::steady_clock::now();
    }
    StopScan();
    SetTimer(hwnd_, kScanWatchdogTimer, 1000, nullptr);
}

void App::CheckScanWatchdog() {
    if (!scanning_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (stop_requested_) {
        const auto stop_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stop_requested_at_);
        if (stop_elapsed.count() >= 15) {
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return;
    }

    const auto idle_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_progress_at_);
    if (idle_elapsed.count() >= 90) {
        RequestStop(false);
        UpdateStatus(L"扫描 90 秒没有进展，已自动请求停止。如果系统调用无响应，程序会在 15 秒后强制结束。");
    }
}

void App::OnScanProgress(std::unique_ptr<ScanProgress> progress) {
    last_progress_at_ = std::chrono::steady_clock::now();
    std::wstring path = progress->current_path;
    constexpr std::size_t kMaxPathShown = 140;
    if (path.size() > kMaxPathShown) {
        path = L"..." + path.substr(path.size() - kMaxPathShown);
    }

    std::wostringstream out;
    const double elapsed = std::max(
        1.0,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - scan_started_at_).count());
    const std::uint64_t files_per_second = static_cast<std::uint64_t>(progress->files_seen / elapsed);
    out << (progress->phase.empty() ? L"正在扫描" : progress->phase)
        << L"；已查看 " << progress->files_seen << L" 个文件（约 " << files_per_second
        << L" 个/秒），图片 " << progress->image_files
        << L" 个，候选 " << progress->candidate_files
        << L" 个，已哈希 " << progress->files_hashed << L" 个，跳过目录 " << progress->skipped_dirs
        << L" 个。当前：" << path;
    UpdateStatus(out.str());
}

void App::OnScanDone(std::unique_ptr<ScanResult> result) {
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
    scanning_ = false;
    stop_requested_ = false;
    KillTimer(hwnd_, kScanWatchdogTimer);
    SendMessageW(progress_bar_, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(progress_bar_, PBM_SETPOS, 0, 0);

    groups_.clear();
    for (auto& group : result->groups) {
        UiGroup ui_group;
        ui_group.hash = std::move(group.hash);
        ui_group.size = group.size;
        for (auto& file : group.files) {
            UiFile ui_file;
            ui_file.file = std::move(file);
            ui_group.files.push_back(std::move(ui_file));
        }
        groups_.push_back(std::move(ui_group));
    }

    PopulateGroups();
    if (!groups_.empty()) {
        ListView_SetItemState(group_list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        PopulateFiles(0);
    } else {
        ListView_DeleteAllItems(file_list_);
        UpdatePreviewPath(L"");
    }

    std::wostringstream out;
    if (!result->error_message.empty()) {
        out << result->error_message << L" ";
    } else {
        out << (result->cancelled ? L"扫描已停止。" : L"扫描完成。");
    }
    out << L"找到 " << groups_.size() << L" 组重复图片；图片 " << result->progress.image_files
        << L" 个；跳过目录 " << result->progress.skipped_dirs << L" 个。";
    UpdateStatus(out.str());
    UpdateButtons();

    if (exit_when_scan_finishes_) {
        DestroyWindow(hwnd_);
    }
}

void App::PopulateGroups() {
    ListView_DeleteAllItems(group_list_);
    for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
        const UiGroup& group = groups_[i];
        std::wstring index_text = std::to_wstring(i + 1);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(index_text.c_str());
        item.lParam = i;
        ListView_InsertItem(group_list_, &item);

        SetItemText(group_list_, i, 1, std::to_wstring(group.files.size()));
        SetItemText(group_list_, i, 2, FormatBytes(group.size));
        const std::uint64_t reclaim = group.files.size() > 1 ? group.size * (group.files.size() - 1) : 0;
        SetItemText(group_list_, i, 3, FormatBytes(reclaim));
        SetItemText(group_list_, i, 4, HashShort(group.hash));
    }
}

void App::PopulateFiles(int group_index) {
    current_group_ = group_index;
    ListView_DeleteAllItems(file_list_);

    if (group_index < 0 || group_index >= static_cast<int>(groups_.size())) {
        UpdatePreviewPath(L"");
        return;
    }

    const UiGroup& group = groups_[group_index];
    for (int i = 0; i < static_cast<int>(group.files.size()); ++i) {
        const UiFile& ui_file = group.files[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(ui_file.file.path.c_str());
        item.lParam = i;
        ListView_InsertItem(file_list_, &item);

        SetItemText(file_list_, i, 1, FormatBytes(ui_file.file.size));
        SetItemText(file_list_, i, 2, FormatFileTime(ui_file.file.creation_time));
        ListView_SetCheckState(file_list_, i, ui_file.checked ? TRUE : FALSE);
    }

    if (!group.files.empty()) {
        ListView_SetItemState(file_list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        UpdatePreviewPath(group.files[0].file.path);
    } else {
        UpdatePreviewPath(L"");
    }
}

void App::SaveCurrentFileChecks() {
    if (current_group_ < 0 || current_group_ >= static_cast<int>(groups_.size()) || !file_list_) {
        return;
    }
    UiGroup& group = groups_[current_group_];
    for (int i = 0; i < static_cast<int>(group.files.size()); ++i) {
        group.files[i].checked = ListView_GetCheckState(file_list_, i) != FALSE;
    }
}

void App::AutoSelect() {
    SaveCurrentFileChecks();
    for (auto& group : groups_) {
        if (group.files.empty()) {
            continue;
        }
        int keep = 0;
        for (int i = 1; i < static_cast<int>(group.files.size()); ++i) {
            const FILETIME& candidate = group.files[i].file.creation_time;
            const FILETIME& current = group.files[keep].file.creation_time;
            if (IsEarlierFileTime(candidate, current) ||
                (CompareFileTime(&candidate, &current) == 0 && group.files[i].file.path < group.files[keep].file.path)) {
                keep = i;
            }
        }
        for (int i = 0; i < static_cast<int>(group.files.size()); ++i) {
            group.files[i].checked = i != keep;
        }
    }
    PopulateFiles(current_group_);
    UpdateStatus(L"已自动勾选：每组保留创建时间最早的文件，其余文件标记为删除。");
}

std::vector<std::wstring> App::CheckedPaths() const {
    std::vector<std::wstring> paths;
    for (const auto& group : groups_) {
        for (const auto& file : group.files) {
            if (file.checked) {
                paths.push_back(file.file.path);
            }
        }
    }
    return paths;
}

void App::DeleteChecked() {
    SaveCurrentFileChecks();
    const std::vector<std::wstring> paths = CheckedPaths();
    if (paths.empty()) {
        MessageBoxW(hwnd_, L"没有勾选要删除的文件。", L"删除所选", MB_ICONINFORMATION);
        return;
    }

    std::wostringstream confirm;
    confirm << L"确定要将 " << paths.size() << L" 个文件放入回收站吗？";
    if (MessageBoxQuestion(hwnd_, confirm.str(), L"确认删除") != IDYES) {
        return;
    }

    DeleteResult result = MoveFilesToRecycleBin(paths);
    RemoveDeletedFiles(result.deleted);
    PopulateGroups();
    if (!groups_.empty()) {
        const int index = std::clamp(current_group_, 0, static_cast<int>(groups_.size()) - 1);
        ListView_SetItemState(group_list_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        PopulateFiles(index);
    } else {
        current_group_ = -1;
        ListView_DeleteAllItems(file_list_);
        UpdatePreviewPath(L"");
    }

    std::wostringstream status;
    status << L"已放入回收站 " << result.deleted.size() << L" 个文件";
    if (!result.failed.empty()) {
        status << L"，失败 " << result.failed.size() << L" 个。首个失败：" << result.failed.front();
    } else {
        status << L"。";
    }
    UpdateStatus(status.str());
}

void App::RemoveDeletedFiles(const std::vector<std::wstring>& deleted) {
    std::unordered_set<std::wstring> deleted_set(deleted.begin(), deleted.end());
    for (auto& group : groups_) {
        group.files.erase(std::remove_if(group.files.begin(), group.files.end(), [&](const UiFile& file) {
            return deleted_set.contains(file.file.path);
        }), group.files.end());
    }
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const UiGroup& group) {
        return group.files.size() < 2;
    }), groups_.end());
}

void App::UpdateButtons() {
    EnableWindow(scan_button_, scanning_ ? FALSE : TRUE);
    EnableWindow(scan_all_button_, scanning_ ? FALSE : TRUE);
    EnableWindow(stop_button_, scanning_ ? TRUE : FALSE);
    EnableWindow(auto_button_, (!scanning_ && !groups_.empty()) ? TRUE : FALSE);
    EnableWindow(delete_button_, (!scanning_ && !groups_.empty()) ? TRUE : FALSE);
}

void App::UpdateStatus(const std::wstring& text) {
    SetWindowTextW(status_label_, text.c_str());
}

void App::UpdatePreviewPath(const std::wstring& path) {
    preview_path_ = path;
    InvalidateRect(preview_, nullptr, TRUE);
}

std::wstring App::SelectedPreviewPath() const {
    const int selected = ListView_GetNextItem(file_list_, -1, LVNI_SELECTED);
    if (current_group_ < 0 || current_group_ >= static_cast<int>(groups_.size()) || selected < 0) {
        return L"";
    }
    const UiGroup& group = groups_[current_group_];
    if (selected >= static_cast<int>(group.files.size())) {
        return L"";
    }
    return group.files[selected].file.path;
}

void App::DrawPreview(HWND hwnd, HDC dc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    FillRect(dc, &rect, preview_brush_ ? preview_brush_ : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    HPEN border = CreatePen(PS_SOLID, 1, RGB(225, 229, 235));
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kMutedText);
    HFONT old_font = nullptr;
    if (ui_font_) {
        old_font = reinterpret_cast<HFONT>(SelectObject(dc, ui_font_));
    }

    if (preview_path_.empty()) {
        DrawTextW(dc, L"选择文件后显示预览", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old_font) {
            SelectObject(dc, old_font);
        }
        return;
    }

    if (gdiplus_token_ == 0) {
        DrawTextW(dc, L"预览组件不可用", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old_font) {
            SelectObject(dc, old_font);
        }
        return;
    }

    Gdiplus::Image image(preview_path_.c_str());
    if (image.GetLastStatus() != Gdiplus::Ok || image.GetWidth() == 0 || image.GetHeight() == 0) {
        DrawTextW(dc, L"此格式无法预览，但仍可按内容重复删除", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old_font) {
            SelectObject(dc, old_font);
        }
        return;
    }

    const int padding = 12;
    const int available_w = std::max(1, static_cast<int>(rect.right - rect.left) - padding * 2);
    const int available_h = std::max(1, static_cast<int>(rect.bottom - rect.top) - padding * 2);
    const double scale = std::min(static_cast<double>(available_w) / image.GetWidth(),
                                  static_cast<double>(available_h) / image.GetHeight());
    const int draw_w = std::max(1, static_cast<int>(image.GetWidth() * scale));
    const int draw_h = std::max(1, static_cast<int>(image.GetHeight() * scale));
    const int draw_x = rect.left + (rect.right - rect.left - draw_w) / 2;
    const int draw_y = rect.top + (rect.bottom - rect.top - draw_h) / 2;

    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(&image, draw_x, draw_y, draw_w, draw_h);
    if (old_font) {
        SelectObject(dc, old_font);
    }
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = reinterpret_cast<App*>(create->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) {
        return app->HandleMessage(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK App::PreviewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = reinterpret_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (message == WM_PAINT && app) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        app->DrawPreview(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        CreateControls();
        LayoutControls();
        return 0;
    case WM_SIZE:
        LayoutControls();
        return 0;
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect,
                 window_brush_ ? window_brush_ : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, control == title_label_ ? kText : kMutedText);
        return reinterpret_cast<LRESULT>(window_brush_ ? window_brush_ : GetStockObject(WHITE_BRUSH));
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kIdScan:
            StartScan(false);
            return 0;
        case kIdScanAll:
            StartScan(true);
            return 0;
        case kIdStop:
            RequestStop(false);
            UpdateStatus(L"正在请求停止扫描。如果系统调用无响应，程序会在 15 秒后强制结束。");
            return 0;
        case kIdAuto:
            AutoSelect();
            return 0;
        case kIdDelete:
            DeleteChecked();
            return 0;
        default:
            break;
        }
        break;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header->hwndFrom == group_list_ && header->code == LVN_ITEMCHANGED) {
            auto* item = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((item->uChanged & LVIF_STATE) != 0 &&
                (item->uNewState & LVIS_SELECTED) != 0 &&
                (item->uOldState & LVIS_SELECTED) == 0) {
                SaveCurrentFileChecks();
                PopulateFiles(item->iItem);
            }
        } else if (header->hwndFrom == file_list_ && header->code == LVN_ITEMCHANGED) {
            auto* item = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((item->uChanged & LVIF_STATE) != 0 && (item->uNewState & LVIS_SELECTED) != 0) {
                UpdatePreviewPath(SelectedPreviewPath());
            }
        }
        return 0;
    }
    case kMsgScanProgress:
        OnScanProgress(std::unique_ptr<ScanProgress>(reinterpret_cast<ScanProgress*>(lparam)));
        return 0;
    case kMsgScanDone:
        OnScanDone(std::unique_ptr<ScanResult>(reinterpret_cast<ScanResult*>(lparam)));
        return 0;
    case WM_TIMER:
        if (wparam == kScanWatchdogTimer) {
            CheckScanWatchdog();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (scanning_) {
            if (!exit_when_scan_finishes_ &&
                MessageBoxQuestion(hwnd_, L"扫描仍在进行。要停止扫描并退出吗？", L"退出") != IDYES) {
                return 0;
            }
            exit_when_scan_finishes_ = true;
            close_requested_at_ = std::chrono::steady_clock::now();
            RequestStop(true);
            EnableWindow(scan_button_, FALSE);
            EnableWindow(scan_all_button_, FALSE);
            EnableWindow(stop_button_, FALSE);
            EnableWindow(auto_button_, FALSE);
            EnableWindow(delete_button_, FALSE);
            UpdateStatus(L"正在停止扫描并退出。如果磁盘枚举无响应，程序会在 15 秒后强制结束。");
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kScanWatchdogTimer);
        StopScan();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

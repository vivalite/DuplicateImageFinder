#pragma once

#include "scanner.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct UiFile {
    ImageFile file;
    bool checked = false;
};

struct UiGroup {
    std::wstring hash;
    std::uint64_t size = 0;
    std::vector<UiFile> files;
};

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();

    bool Initialize(int show_command);
    int Run();
    void DrawPreview(HWND hwnd, HDC dc);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void RegisterWindowClasses();
    void CreateControls();
    void CreateUiResources();
    void ApplyControlFonts();
    void LayoutControls();
    void StartScan(bool all_drives);
    void StopScan();
    void RequestStop(bool exit_after_stop);
    void CheckScanWatchdog();
    void OnScanProgress(std::unique_ptr<ScanProgress> progress);
    void OnScanDone(std::unique_ptr<ScanResult> result);
    void PopulateGroups();
    void PopulateFiles(int group_index);
    void SaveCurrentFileChecks();
    void AutoSelect();
    void DeleteChecked();
    void RemoveDeletedFiles(const std::vector<std::wstring>& deleted);
    void UpdateButtons();
    void UpdateStatus(const std::wstring& text);
    void UpdatePreviewPath(const std::wstring& path);
    std::vector<std::wstring> CheckedPaths() const;
    std::wstring SelectedPreviewPath() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND title_label_ = nullptr;
    HWND subtitle_label_ = nullptr;
    HWND scan_button_ = nullptr;
    HWND scan_all_button_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND auto_button_ = nullptr;
    HWND delete_button_ = nullptr;
    HWND progress_bar_ = nullptr;
    HWND status_label_ = nullptr;
    HWND group_list_ = nullptr;
    HWND file_list_ = nullptr;
    HWND preview_ = nullptr;

    HFONT ui_font_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HBRUSH window_brush_ = nullptr;
    HBRUSH preview_brush_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    std::thread scan_thread_;
    std::atomic_bool cancel_scan_ = false;
    std::chrono::steady_clock::time_point scan_started_at_{};
    std::chrono::steady_clock::time_point last_progress_at_{};
    std::chrono::steady_clock::time_point stop_requested_at_{};
    std::chrono::steady_clock::time_point close_requested_at_{};
    bool scanning_ = false;
    bool exit_when_scan_finishes_ = false;
    bool stop_requested_ = false;
    int current_group_ = -1;
    std::wstring preview_path_;
    std::vector<UiGroup> groups_;
};

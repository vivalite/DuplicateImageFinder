#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImageFile {
    std::wstring path;
    std::uint64_t size = 0;
    FILETIME creation_time{};
    FILETIME write_time{};
    std::wstring hash;
};

struct DuplicateGroup {
    std::wstring hash;
    std::uint64_t size = 0;
    std::vector<ImageFile> files;
};

struct ScanProgress {
    std::uint64_t files_seen = 0;
    std::uint64_t image_files = 0;
    std::uint64_t candidate_files = 0;
    std::uint64_t bytes_hashed = 0;
    std::uint64_t files_hashed = 0;
    std::uint64_t skipped_dirs = 0;
    std::wstring phase;
    std::wstring current_path;
};

struct ScanResult {
    std::vector<DuplicateGroup> groups;
    ScanProgress progress;
    bool cancelled = false;
    std::wstring error_message;
};

using ProgressCallback = std::function<void(const ScanProgress&)>;

class Scanner {
public:
    ScanResult ScanCommonFolders(const std::atomic_bool& cancel, const ProgressCallback& progress);
    ScanResult ScanAllFixedDrives(const std::atomic_bool& cancel, const ProgressCallback& progress);
};

bool IsEarlierFileTime(const FILETIME& left, const FILETIME& right);
std::wstring FormatBytes(std::uint64_t bytes);

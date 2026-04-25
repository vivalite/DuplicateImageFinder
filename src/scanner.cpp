#include "scanner.h"

#include "hash.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace {

bool IsImagePath(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) {
        return false;
    }
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp" ||
           ext == L".gif" || ext == L".webp" || ext == L".tif" || ext == L".tiff" ||
           ext == L".heic" || ext == L".ico";
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return text;
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ShouldSkipDirectory(const WIN32_FIND_DATAW& data) {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }

    const std::wstring name = Lower(data.cFileName);
    static const std::vector<std::wstring> kSkipNames = {
        L"$recycle.bin",
        L"system volume information",
        L"windows",
        L"program files",
        L"program files (x86)",
        L"programdata",
        L"appdata",
        L".git",
        L".svn",
        L".hg",
        L".vs",
        L".cache",
        L"__pycache__",
        L"node_modules",
        L"packages",
        L"vcpkg_installed",
        L"bin",
        L"obj",
        L"debug",
        L"release",
        L"x64",
        L"x86",
        L"arm",
        L"arm64",
        L"build",
        L"dist",
        L"target",
        L"vendor",
        L"plugins",
        L"library",
        L"intermediate",
        L"binaries",
        L"deriveddatacache",
        L"saved",
        L"temp"
    };

    if (std::find(kSkipNames.begin(), kSkipNames.end(), name) != kSkipNames.end()) {
        return true;
    }
    return StartsWith(name, L"sdk") || StartsWith(name, L"windows kits");
}

std::uint64_t FileSizeFromFindData(const WIN32_FIND_DATAW& data) {
    ULARGE_INTEGER value{};
    value.LowPart = data.nFileSizeLow;
    value.HighPart = data.nFileSizeHigh;
    return value.QuadPart;
}

void MaybeReport(const ProgressCallback& callback, const ScanProgress& progress, std::uint64_t cadence = 128) {
    if (callback && ((progress.files_seen % cadence) == 0)) {
        callback(progress);
    }
}

template <typename FileHandler>
void EnumerateDirectory(const std::wstring& root,
                        const std::atomic_bool& cancel,
                        const ProgressCallback& callback,
                        ScanProgress& progress,
                        bool count_totals,
                        FileHandler handle_file) {
    std::vector<std::wstring> pending;
    pending.push_back(root);

    while (!pending.empty() && !cancel.load()) {
        std::wstring directory = std::move(pending.back());
        pending.pop_back();

        std::wstring pattern = directory;
        if (!pattern.empty() && pattern.back() != L'\\') {
            pattern.push_back(L'\\');
        }
        pattern.append(L"*");

        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            ++progress.skipped_dirs;
            if (callback) {
                callback(progress);
            }
            continue;
        }

        do {
            if (cancel.load()) {
                break;
            }
            const wchar_t* name = data.cFileName;
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) {
                continue;
            }

            std::wstring path = directory;
            if (!path.empty() && path.back() != L'\\') {
                path.push_back(L'\\');
            }
            path.append(name);
            progress.current_path = path;

            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (ShouldSkipDirectory(data)) {
                    ++progress.skipped_dirs;
                } else {
                    pending.push_back(std::move(path));
                }
                continue;
            }

            if (count_totals) {
                ++progress.files_seen;
            }
            if (IsImagePath(path)) {
                if (count_totals) {
                    ++progress.image_files;
                }
                handle_file(path, data);
            }
            MaybeReport(callback, progress);
        } while (FindNextFileW(find, &data));

        FindClose(find);
    }
}

std::vector<std::wstring> FixedDrives() {
    std::vector<wchar_t> buffer(512);
    DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) {
        return {};
    }
    if (length > buffer.size()) {
        buffer.resize(length + 1);
        length = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    }

    std::vector<std::wstring> drives;
    for (const wchar_t* p = buffer.data(); *p != L'\0'; p += wcslen(p) + 1) {
        if (GetDriveTypeW(p) == DRIVE_FIXED) {
            drives.emplace_back(p);
        }
    }
    return drives;
}

bool DirectoryExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void AddKnownFolder(std::vector<std::wstring>& roots, const KNOWNFOLDERID& folder_id) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
        std::wstring folder(path);
        CoTaskMemFree(path);
        if (DirectoryExists(folder) && std::find(roots.begin(), roots.end(), folder) == roots.end()) {
            roots.push_back(std::move(folder));
        }
    }
}

std::vector<std::wstring> CommonFolders() {
    std::vector<std::wstring> roots;
    AddKnownFolder(roots, FOLDERID_Pictures);
    AddKnownFolder(roots, FOLDERID_Desktop);
    AddKnownFolder(roots, FOLDERID_Downloads);

    wchar_t user_profile[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (GetEnvironmentVariableW(L"USERPROFILE", user_profile, size) > 0) {
        const std::wstring profile(user_profile);
        const std::wstring one_drive_pictures = profile + L"\\OneDrive\\Pictures";
        if (DirectoryExists(one_drive_pictures) &&
            std::find(roots.begin(), roots.end(), one_drive_pictures) == roots.end()) {
            roots.push_back(one_drive_pictures);
        }
    }
    return roots.empty() ? FixedDrives() : roots;
}

struct SizeBucket {
    std::uint32_t count = 0;
    std::optional<ImageFile> first;
    std::vector<ImageFile> candidates;
};

void AddImageBySize(std::unordered_map<std::uint64_t, SizeBucket>& buckets,
                    const std::wstring& path,
                    const WIN32_FIND_DATAW& data) {
    ImageFile file;
    file.path = path;
    file.size = FileSizeFromFindData(data);
    file.creation_time = data.ftCreationTime;
    file.write_time = data.ftLastWriteTime;

    SizeBucket& bucket = buckets[file.size];
    ++bucket.count;
    if (bucket.count == 1) {
        bucket.first = std::move(file);
    } else {
        if (bucket.count == 2 && bucket.first) {
            bucket.candidates.push_back(std::move(*bucket.first));
            bucket.first.reset();
        }
        bucket.candidates.push_back(std::move(file));
    }
}

unsigned int HashWorkerCount(std::size_t candidate_count) {
    if (candidate_count < 2) {
        return 1;
    }

    unsigned int hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        hardware = 4;
    }

    const unsigned int balanced = std::max(2u, hardware / 2);
    return std::min<unsigned int>({balanced, 8u, static_cast<unsigned int>(candidate_count)});
}

std::vector<ImageFile> HashCandidates(std::vector<ImageFile>& candidates,
                                      const std::atomic_bool& cancel,
                                      const ProgressCallback& callback,
                                      ScanProgress& progress) {
    std::vector<ImageFile> hashed_files;
    if (candidates.empty() || cancel.load()) {
        return hashed_files;
    }

    progress.phase = L"正在哈希候选图片";

    const unsigned int worker_count = HashWorkerCount(candidates.size());
    hashed_files.reserve(std::min<std::size_t>(candidates.size(), 65536));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    std::atomic_size_t next_index = 0;
    std::atomic_uint64_t bytes_hashed = progress.bytes_hashed;
    std::atomic_uint64_t files_hashed = progress.files_hashed;
    std::atomic_uint32_t active_workers = worker_count;
    std::atomic_bool stop_workers = false;

    std::mutex current_mutex;
    std::wstring current_path = progress.current_path;
    std::mutex hashed_mutex;
    std::mutex error_mutex;
    std::exception_ptr worker_error;

    for (unsigned int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker]() {
            try {
                while (!cancel.load() && !stop_workers.load()) {
                    const std::size_t index = next_index.fetch_add(1);
                    if (index >= candidates.size()) {
                        break;
                    }

                    {
                        std::lock_guard lock(current_mutex);
                        current_path = candidates[index].path;
                    }

                    auto hash = ComputeSha256(candidates[index].path, cancel, [&](std::uint64_t bytes) {
                        bytes_hashed.fetch_add(bytes, std::memory_order_relaxed);
                    });
                    files_hashed.fetch_add(1, std::memory_order_relaxed);
                    if (hash) {
                        candidates[index].hash = *hash;
                        std::lock_guard lock(hashed_mutex);
                        hashed_files.push_back(std::move(candidates[index]));
                    }
                }
            } catch (...) {
                stop_workers.store(true);
                std::lock_guard lock(error_mutex);
                if (!worker_error) {
                    worker_error = std::current_exception();
                }
            }
            active_workers.fetch_sub(1);
        });
    }

    while (active_workers.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        progress.bytes_hashed = bytes_hashed.load(std::memory_order_relaxed);
        progress.files_hashed = files_hashed.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(current_mutex);
            progress.current_path = current_path;
        }
        if (callback) {
            callback(progress);
        }
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    progress.bytes_hashed = bytes_hashed.load(std::memory_order_relaxed);
    progress.files_hashed = files_hashed.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(current_mutex);
        progress.current_path = current_path;
    }

    if (worker_error) {
        std::rethrow_exception(worker_error);
    }

    return hashed_files;
}

}  // namespace

bool IsEarlierFileTime(const FILETIME& left, const FILETIME& right) {
    return CompareFileTime(&left, &right) < 0;
}

std::wstring FormatBytes(std::uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(unit == 0 ? 0 : 2);
    out << value << L" " << units[unit];
    return out.str();
}

namespace {

ScanResult ScanRoots(const std::vector<std::wstring>& roots,
                     const std::atomic_bool& cancel,
                     const ProgressCallback& callback) {
    ScanResult result;
    std::unordered_map<std::uint64_t, SizeBucket> by_size;

    for (const auto& root : roots) {
        if (cancel.load()) {
            break;
        }
        result.progress.phase = L"正在枚举文件";
        result.progress.current_path = root;
        if (callback) {
            callback(result.progress);
        }
        EnumerateDirectory(root, cancel, callback, result.progress, true,
                           [&](const std::wstring& path, const WIN32_FIND_DATAW& data) {
            AddImageBySize(by_size, path, data);
        });
    }

    if (!cancel.load()) {
        result.progress.phase = L"正在准备候选图片";
        result.progress.current_path = L"";
        if (callback) {
            callback(result.progress);
        }

        std::size_t total_candidates = 0;
        for (const auto& [size, bucket] : by_size) {
            if (bucket.candidates.size() >= 2) {
                total_candidates += bucket.candidates.size();
            }
        }

        std::vector<ImageFile> candidates;
        candidates.reserve(total_candidates);
        std::uint64_t prepared = 0;
        for (auto& [size, bucket] : by_size) {
            if (cancel.load()) {
                break;
            }
            if (bucket.candidates.size() < 2) {
                continue;
            }
            for (auto& file : bucket.candidates) {
                if (cancel.load()) {
                    break;
                }
                result.progress.current_path = file.path;
                candidates.push_back(std::move(file));
                ++prepared;
                result.progress.candidate_files = prepared;
                if (callback && (prepared % 8192 == 0)) {
                    callback(result.progress);
                }
            }
        }
        result.progress.candidate_files = prepared;
        if (callback) {
            callback(result.progress);
        }

        std::vector<ImageFile> hashed_files = HashCandidates(candidates, cancel, callback, result.progress);
        result.progress.phase = L"正在整理结果";
        result.progress.current_path = L"";
        if (callback) {
            callback(result.progress);
        }
        std::unordered_map<std::wstring, std::vector<ImageFile>> by_hash;
        std::uint64_t grouped = 0;
        for (auto& file : hashed_files) {
            if (cancel.load()) {
                break;
            }
            std::wstring key = std::to_wstring(file.size);
            key.push_back(L'|');
            key.append(file.hash);
            by_hash[key].push_back(std::move(file));
            ++grouped;
            if (callback && (grouped % 8192 == 0)) {
                result.progress.current_path = L"";
                callback(result.progress);
            }
        }

        for (auto& [key, files] : by_hash) {
            if (files.size() < 2) {
                continue;
            }
            DuplicateGroup group;
            group.hash = files.front().hash;
            group.size = files.front().size;
            group.files = std::move(files);
            std::sort(group.files.begin(), group.files.end(), [](const ImageFile& a, const ImageFile& b) {
                if (CompareFileTime(&a.creation_time, &b.creation_time) != 0) {
                    return IsEarlierFileTime(a.creation_time, b.creation_time);
                }
                return a.path < b.path;
            });
            result.groups.push_back(std::move(group));
        }
    }

    result.cancelled = cancel.load();
    if (callback) {
        callback(result.progress);
    }
    std::sort(result.groups.begin(), result.groups.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
        return (a.size * a.files.size()) > (b.size * b.files.size());
    });
    return result;
}

}  // namespace

ScanResult Scanner::ScanCommonFolders(const std::atomic_bool& cancel, const ProgressCallback& callback) {
    return ScanRoots(CommonFolders(), cancel, callback);
}

ScanResult Scanner::ScanAllFixedDrives(const std::atomic_bool& cancel, const ProgressCallback& callback) {
    return ScanRoots(FixedDrives(), cancel, callback);
}

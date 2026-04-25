#include "scanner.h"

#include "hash.h"

#include <windows.h>

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
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
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

    const unsigned int worker_count = HashWorkerCount(candidates.size());
    std::vector<std::optional<ImageFile>> hashed(candidates.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    std::atomic_size_t next_index = 0;
    std::atomic_uint64_t bytes_hashed = progress.bytes_hashed;
    std::atomic_uint64_t files_hashed = progress.files_hashed;
    std::atomic_uint32_t active_workers = worker_count;
    std::atomic_bool stop_workers = false;

    std::mutex current_mutex;
    std::wstring current_path = progress.current_path;
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
                        hashed[index] = std::move(candidates[index]);
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

    for (auto& file : hashed) {
        if (file && !file->hash.empty()) {
            hashed_files.push_back(std::move(*file));
        }
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

ScanResult Scanner::ScanAllFixedDrives(const std::atomic_bool& cancel, const ProgressCallback& callback) {
    ScanResult result;
    std::unordered_map<std::uint64_t, std::vector<ImageFile>> by_size;
    std::vector<std::wstring> drives = FixedDrives();

    for (const auto& drive : drives) {
        if (cancel.load()) {
            break;
        }
        result.progress.current_path = drive;
        if (callback) {
            callback(result.progress);
        }
        EnumerateDirectory(drive, cancel, callback, result.progress, true,
                           [&](const std::wstring& path, const WIN32_FIND_DATAW& data) {
            ImageFile file;
            file.path = path;
            file.size = FileSizeFromFindData(data);
            file.creation_time = data.ftCreationTime;
            file.write_time = data.ftLastWriteTime;
            by_size[file.size].push_back(std::move(file));
        });
    }

    if (!cancel.load()) {
        std::vector<ImageFile> candidates;
        for (auto& [size, files] : by_size) {
            if (files.size() < 2) {
                continue;
            }
            candidates.reserve(candidates.size() + files.size());
            for (auto& file : files) {
                candidates.push_back(std::move(file));
            }
        }

        std::vector<ImageFile> hashed_files = HashCandidates(candidates, cancel, callback, result.progress);
        std::unordered_map<std::wstring, std::vector<ImageFile>> by_hash;
        for (auto& file : hashed_files) {
            std::wstring key = std::to_wstring(file.size);
            key.push_back(L'|');
            key.append(file.hash);
            by_hash[key].push_back(std::move(file));
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

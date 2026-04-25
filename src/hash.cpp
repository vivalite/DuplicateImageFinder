#include "hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <memory>
#include <sstream>
#include <vector>

namespace {

struct BCryptAlgCloser {
    void operator()(BCRYPT_ALG_HANDLE handle) const {
        if (handle) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
};

struct BCryptHashCloser {
    void operator()(BCRYPT_HASH_HANDLE handle) const {
        if (handle) {
            BCryptDestroyHash(handle);
        }
    }
};

std::wstring ToHex(const std::vector<unsigned char>& bytes) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

}  // namespace

std::optional<std::wstring> ComputeSha256(
    const std::wstring& path,
    const std::atomic_bool& cancel,
    const std::function<void(std::uint64_t)>& on_bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE raw_alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&raw_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::unique_ptr<void, BCryptAlgCloser> alg_guard(raw_alg);

    DWORD object_length = 0;
    DWORD result_length = 0;
    if (BCryptGetProperty(raw_alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
                          sizeof(object_length), &result_length, 0) < 0) {
        CloseHandle(file);
        return std::nullopt;
    }

    DWORD hash_length = 0;
    if (BCryptGetProperty(raw_alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length),
                          sizeof(hash_length), &result_length, 0) < 0) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::vector<unsigned char> object_buffer(object_length);
    BCRYPT_HASH_HANDLE raw_hash = nullptr;
    if (BCryptCreateHash(raw_alg, &raw_hash, object_buffer.data(), object_length, nullptr, 0, 0) < 0) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::unique_ptr<void, BCryptHashCloser> hash_guard(raw_hash);

    std::vector<unsigned char> buffer(256 * 1024);
    while (!cancel.load()) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            CloseHandle(file);
            return std::nullopt;
        }
        if (read == 0) {
            break;
        }
        if (BCryptHashData(raw_hash, buffer.data(), read, 0) < 0) {
            CloseHandle(file);
            return std::nullopt;
        }
        if (on_bytes) {
            on_bytes(read);
        }
    }

    CloseHandle(file);
    if (cancel.load()) {
        return std::nullopt;
    }

    std::vector<unsigned char> hash(hash_length);
    if (BCryptFinishHash(raw_hash, hash.data(), hash_length, 0) < 0) {
        return std::nullopt;
    }
    return ToHex(hash);
}

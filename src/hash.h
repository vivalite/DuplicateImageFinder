#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

std::optional<std::wstring> ComputeSha256(
    const std::wstring& path,
    const std::atomic_bool& cancel,
    const std::function<void(std::uint64_t)>& on_bytes);

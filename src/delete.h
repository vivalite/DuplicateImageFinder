#pragma once

#include <string>
#include <vector>

struct DeleteResult {
    std::vector<std::wstring> deleted;
    std::vector<std::wstring> failed;
};

DeleteResult MoveFilesToRecycleBin(const std::vector<std::wstring>& paths);

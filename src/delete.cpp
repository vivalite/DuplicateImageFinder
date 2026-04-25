#include "delete.h"

#include <windows.h>
#include <shellapi.h>

DeleteResult MoveFilesToRecycleBin(const std::vector<std::wstring>& paths) {
    DeleteResult result;
    for (const auto& path : paths) {
        std::wstring double_null = path;
        double_null.push_back(L'\0');
        double_null.push_back(L'\0');

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = double_null.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

        const int code = SHFileOperationW(&op);
        if (code == 0 && !op.fAnyOperationsAborted) {
            result.deleted.push_back(path);
        } else {
            result.failed.push_back(path);
        }
    }
    return result;
}

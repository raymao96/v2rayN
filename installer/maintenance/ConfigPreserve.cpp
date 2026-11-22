#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <objbase.h>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t PayloadSeparator = L'|';
    constexpr wchar_t BackupPrefix[] = L".v2rayN-upgrade-guiConfigs-";

    void LogMessage(MSIHANDLE install, const std::wstring& message)
    {
        PMSIHANDLE record = MsiCreateRecord(1);
        if (record)
        {
            MsiRecordSetStringW(record, 0, L"v2rayN ConfigPreserve: [1]");
            MsiRecordSetStringW(record, 1, message.c_str());
            MsiProcessMessage(install, INSTALLMESSAGE_INFO, record);
        }
    }

    std::wstring GetProperty(MSIHANDLE install, const wchar_t* name)
    {
        wchar_t empty = L'\0';
        DWORD length = 0;
        UINT status = MsiGetPropertyW(install, name, &empty, &length);
        if (status != ERROR_MORE_DATA || length == 0)
        {
            return {};
        }

        std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
        DWORD capacity = length + 1;
        status = MsiGetPropertyW(install, name, buffer.data(), &capacity);
        if (status != ERROR_SUCCESS)
        {
            return {};
        }
        return std::wstring(buffer.data(), capacity);
    }

    std::wstring NormalizePath(std::wstring path)
    {
        for (wchar_t& ch : path)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
        }
        while (path.size() > 3 && path.back() == L'\\')
        {
            path.pop_back();
        }
        return path;
    }

    bool IsDriveAbsolutePath(const std::wstring& path)
    {
        return path.size() >= 3 && std::iswalpha(path[0]) != 0 && path[1] == L':' && path[2] == L'\\';
    }

    bool HasDotSegments(const std::wstring& path)
    {
        size_t pos = 3;
        while (pos < path.size())
        {
            size_t next = path.find(L'\\', pos);
            const size_t len = (next == std::wstring::npos ? path.size() : next) - pos;
            if ((len == 1 && path[pos] == L'.') || (len == 2 && path[pos] == L'.' && path[pos + 1] == L'.'))
            {
                return true;
            }
            if (next == std::wstring::npos)
            {
                break;
            }
            pos = next + 1;
        }
        return false;
    }

    std::wstring ParentDirectory(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos)
        {
            return {};
        }
        if (slash <= 2)
        {
            return path.substr(0, slash + 1);
        }
        return path.substr(0, slash);
    }

    std::wstring LeafName(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L'\\');
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    std::wstring JoinPath(const std::wstring& base, const std::wstring& child)
    {
        std::wstring result = NormalizePath(base);
        if (!result.empty() && result.back() != L'\\')
        {
            result.push_back(L'\\');
        }
        result.append(child);
        return result;
    }

    std::wstring ExtendedPath(const std::wstring& path)
    {
        if (path.rfind(L"\\\\?\\", 0) == 0)
        {
            return path;
        }
        if (IsDriveAbsolutePath(path))
        {
            return L"\\\\?\\" + path;
        }
        return path;
    }

    bool IsNotFoundError(DWORD error)
    {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    bool MoveWithRetry(const std::wstring& from, const std::wstring& to, DWORD flags, DWORD& lastError)
    {
        const std::wstring extFrom = ExtendedPath(from);
        const std::wstring extTo = ExtendedPath(to);
        for (int attempt = 0; attempt < 15; ++attempt)
        {
            if (MoveFileExW(extFrom.c_str(), extTo.c_str(), flags))
            {
                lastError = ERROR_SUCCESS;
                return true;
            }
            lastError = GetLastError();
            if (lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION)
            {
                return false;
            }
            Sleep(200);
        }
        return false;
    }

    bool EnsureDirectoryExists(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        const std::wstring ext = ExtendedPath(path);
        const DWORD attr = GetFileAttributesW(ext.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
        {
            return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        const std::wstring parent = ParentDirectory(path);
        if (!parent.empty() && _wcsicmp(parent.c_str(), path.c_str()) != 0 && !EnsureDirectoryExists(parent))
        {
            return false;
        }
        return CreateDirectoryW(ext.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool MergeDirectoryTree(MSIHANDLE install, const std::wstring& sourceDir, const std::wstring& targetDir)
    {
        if (!EnsureDirectoryExists(targetDir))
        {
            LogMessage(install, L"Failed to ensure target directory: " + targetDir);
            return false;
        }

        const std::wstring pattern = ExtendedPath(JoinPath(sourceDir, L"*"));
        WIN32_FIND_DATAW entry = {};
        HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES)
            {
                RemoveDirectoryW(ExtendedPath(sourceDir).c_str());
                return true;
            }
            LogMessage(install, L"FindFirstFileW failed (" + std::to_wstring(err) + L") on " + sourceDir);
            return false;
        }

        bool ok = true;
        do
        {
            if (std::wcscmp(entry.cFileName, L".") == 0 || std::wcscmp(entry.cFileName, L"..") == 0)
            {
                continue;
            }

            const std::wstring srcItem = JoinPath(sourceDir, entry.cFileName);
            const std::wstring dstItem = JoinPath(targetDir, entry.cFileName);
            const bool isDir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparse = (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            if (isDir && !isReparse)
            {
                const DWORD dstAttr = GetFileAttributesW(ExtendedPath(dstItem).c_str());
                if (dstAttr == INVALID_FILE_ATTRIBUTES)
                {
                    DWORD moveError = ERROR_SUCCESS;
                    if (!MoveWithRetry(srcItem, dstItem, MOVEFILE_WRITE_THROUGH, moveError))
                    {
                        LogMessage(install, L"Failed to move directory (" + std::to_wstring(moveError) + L"): " + srcItem);
                        ok = false;
                        break;
                    }
                }
                else if ((dstAttr & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    if (!MergeDirectoryTree(install, srcItem, dstItem))
                    {
                        ok = false;
                        break;
                    }
                }
                else
                {
                    LogMessage(install, L"Destination path is not a directory: " + dstItem);
                    ok = false;
                    break;
                }
            }
            else
            {
                DWORD moveError = ERROR_SUCCESS;
                if (!MoveWithRetry(srcItem, dstItem, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, moveError))
                {
                    LogMessage(install, L"Failed to move entry (" + std::to_wstring(moveError) + L"): " + srcItem);
                    ok = false;
                    break;
                }
            }
        } while (FindNextFileW(find, &entry));

        const DWORD findError = GetLastError();
        FindClose(find);
        if (!ok)
        {
            return false;
        }
        if (findError != ERROR_NO_MORE_FILES)
        {
            LogMessage(install, L"FindNextFileW failed (" + std::to_wstring(findError) + L") on " + sourceDir);
            return false;
        }

        if (!RemoveDirectoryW(ExtendedPath(sourceDir).c_str()))
        {
            const DWORD removeError = GetLastError();
            if (!IsNotFoundError(removeError))
            {
                LogMessage(install, L"Failed to remove drained backup directory (" + std::to_wstring(removeError) + L"): " + sourceDir);
                return false;
            }
        }
        return true;
    }

    struct PreservePaths
    {
        std::wstring sourceDir;
        std::wstring targetDir;
        std::wstring backupDir;
    };

    bool ParsePayload(const std::wstring& payload, PreservePaths& paths)
    {
        const size_t first = payload.find(PayloadSeparator);
        if (first == std::wstring::npos)
        {
            return false;
        }
        const size_t second = payload.find(PayloadSeparator, first + 1);
        if (second == std::wstring::npos || payload.find(PayloadSeparator, second + 1) != std::wstring::npos)
        {
            return false;
        }

        paths.sourceDir = NormalizePath(payload.substr(0, first));
        paths.targetDir = NormalizePath(payload.substr(first + 1, second - first - 1));
        paths.backupDir = NormalizePath(payload.substr(second + 1));
        return IsDriveAbsolutePath(paths.sourceDir) && !HasDotSegments(paths.sourceDir) &&
               IsDriveAbsolutePath(paths.targetDir) && !HasDotSegments(paths.targetDir) &&
               IsDriveAbsolutePath(paths.backupDir) && !HasDotSegments(paths.backupDir);
    }

    UINT RestoreDirectory(MSIHANDLE install, const std::wstring& backupDir, const std::wstring& destDir)
    {
        const std::wstring extBackup = ExtendedPath(backupDir);
        const DWORD backupAttr = GetFileAttributesW(extBackup.c_str());
        if (backupAttr == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD err = GetLastError();
            if (IsNotFoundError(err))
            {
                return ERROR_SUCCESS;
            }
            LogMessage(install, L"Failed to inspect backup directory (" + std::to_wstring(err) + L"): " + backupDir);
            return ERROR_INSTALL_FAILURE;
        }

        if (!EnsureDirectoryExists(ParentDirectory(destDir)))
        {
            LogMessage(install, L"Failed to create parent directory for " + destDir);
            return ERROR_INSTALL_FAILURE;
        }

        const std::wstring extDest = ExtendedPath(destDir);
        DWORD destAttr = GetFileAttributesW(extDest.c_str());
        if (destAttr != INVALID_FILE_ATTRIBUTES && (destAttr & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            if (RemoveDirectoryW(extDest.c_str()))
            {
                destAttr = INVALID_FILE_ATTRIBUTES;
            }
        }

        if (destAttr == INVALID_FILE_ATTRIBUTES)
        {
            DWORD moveError = ERROR_SUCCESS;
            if (!MoveWithRetry(backupDir, destDir, MOVEFILE_WRITE_THROUGH, moveError))
            {
                LogMessage(install, L"Failed to restore backup directory (" + std::to_wstring(moveError) + L"): " + backupDir);
                return ERROR_INSTALL_FAILURE;
            }
            LogMessage(install, L"Restored guiConfigs to " + destDir);
            return ERROR_SUCCESS;
        }

        if ((backupAttr & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            LogMessage(install, L"Cannot merge reparse point backup into non-empty directory: " + destDir);
            return ERROR_INSTALL_FAILURE;
        }

        if (!MergeDirectoryTree(install, backupDir, destDir))
        {
            return ERROR_INSTALL_FAILURE;
        }
        LogMessage(install, L"Merged guiConfigs into " + destDir);
        return ERROR_SUCCESS;
    }

    std::wstring CreateUniqueSuffix()
    {
        GUID guid = {};
        if (FAILED(CoCreateGuid(&guid)))
        {
            return {};
        }
        wchar_t text[40] = {};
        swprintf_s(
            text,
            L"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
            guid.Data1,
            guid.Data2,
            guid.Data3,
            guid.Data4[0],
            guid.Data4[1],
            guid.Data4[2],
            guid.Data4[3],
            guid.Data4[4],
            guid.Data4[5],
            guid.Data4[6],
            guid.Data4[7]);
        return text;
    }
}

extern "C" UINT WINAPI PrepareConfigPreserve(MSIHANDLE install)
{
    const std::wstring previousDir = NormalizePath(GetProperty(install, L"PREVIOUS_DIR"));
    const std::wstring installDir = NormalizePath(GetProperty(install, L"INSTALLFOLDER"));
    const std::wstring cleanConfig = NormalizePath(GetProperty(install, L"CLEAN_CONFIG"));

    const std::wstring baseDir = !previousDir.empty() ? previousDir : installDir;
    if (!IsDriveAbsolutePath(baseDir) || HasDotSegments(baseDir))
    {
        LogMessage(install, L"Skipping config preserve: invalid base directory.");
        return ERROR_SUCCESS;
    }

    std::wstring sourceDir = JoinPath(baseDir, L"guiConfigs");
    if (!cleanConfig.empty() &&
        IsDriveAbsolutePath(cleanConfig) &&
        !HasDotSegments(cleanConfig) &&
        _wcsicmp(LeafName(cleanConfig).c_str(), L"guiConfigs") == 0 &&
        _wcsicmp(ParentDirectory(cleanConfig).c_str(), baseDir.c_str()) == 0)
    {
        sourceDir = cleanConfig;
    }

    const std::wstring targetBase = IsDriveAbsolutePath(installDir) && !HasDotSegments(installDir) ? installDir : baseDir;
    const std::wstring targetDir = JoinPath(targetBase, L"guiConfigs");
    const std::wstring suffix = CreateUniqueSuffix();
    if (suffix.empty())
    {
        LogMessage(install, L"Failed to generate unique backup directory suffix.");
        return ERROR_INSTALL_FAILURE;
    }

    const std::wstring backupDir = JoinPath(ParentDirectory(sourceDir), std::wstring(BackupPrefix) + suffix);
    const std::wstring payload = sourceDir + PayloadSeparator + targetDir + PayloadSeparator + backupDir;

    if (MsiSetPropertyW(install, L"BackupConfigPreserve", payload.c_str()) != ERROR_SUCCESS ||
        MsiSetPropertyW(install, L"RestoreConfigPreserve", payload.c_str()) != ERROR_SUCCESS ||
        MsiSetPropertyW(install, L"RollbackConfigPreserve", payload.c_str()) != ERROR_SUCCESS)
    {
        LogMessage(install, L"Failed to set CustomActionData properties.");
        return ERROR_INSTALL_FAILURE;
    }

    LogMessage(install, L"Prepared guiConfigs preservation from " + sourceDir + L" via " + backupDir);
    return ERROR_SUCCESS;
}

extern "C" UINT WINAPI BackupConfigPreserve(MSIHANDLE install)
{
    const std::wstring payload = GetProperty(install, L"CustomActionData");
    PreservePaths paths;
    if (!ParsePayload(payload, paths))
    {
        return ERROR_SUCCESS;
    }

    const DWORD srcAttr = GetFileAttributesW(ExtendedPath(paths.sourceDir).c_str());
    if (srcAttr == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD err = GetLastError();
        if (IsNotFoundError(err))
        {
            LogMessage(install, L"No existing guiConfigs directory to preserve.");
            return ERROR_SUCCESS;
        }
        LogMessage(install, L"Failed to inspect guiConfigs (" + std::to_wstring(err) + L"): " + paths.sourceDir);
        return ERROR_INSTALL_FAILURE;
    }
    if ((srcAttr & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        LogMessage(install, L"Skipping guiConfigs preservation: path is not a directory.");
        return ERROR_SUCCESS;
    }

    DWORD moveError = ERROR_SUCCESS;
    if (!MoveWithRetry(paths.sourceDir, paths.backupDir, MOVEFILE_WRITE_THROUGH, moveError))
    {
        LogMessage(install, L"Failed to back up guiConfigs (" + std::to_wstring(moveError) + L"): " + paths.sourceDir);
        return ERROR_INSTALL_FAILURE;
    }

    LogMessage(install, L"Backed up guiConfigs to " + paths.backupDir);
    return ERROR_SUCCESS;
}

extern "C" UINT WINAPI RestoreConfigPreserve(MSIHANDLE install)
{
    const std::wstring payload = GetProperty(install, L"CustomActionData");
    PreservePaths paths;
    if (!ParsePayload(payload, paths))
    {
        return ERROR_SUCCESS;
    }
    return RestoreDirectory(install, paths.backupDir, paths.targetDir);
}

extern "C" UINT WINAPI RollbackConfigPreserve(MSIHANDLE install)
{
    const std::wstring payload = GetProperty(install, L"CustomActionData");
    PreservePaths paths;
    if (!ParsePayload(payload, paths))
    {
        return ERROR_SUCCESS;
    }
    return RestoreDirectory(install, paths.backupDir, paths.sourceDir);
}

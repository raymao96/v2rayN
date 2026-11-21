#include <windows.h>
#include <commctrl.h>
#include <msi.h>
#include <shellapi.h>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include <BootstrapperApplication.h>
#include <BAFunctions.h>

namespace
{
    constexpr WORD RepairControl = BAFUNCTIONS_FIRST_ASSIGN_CONTROL_ID;
    constexpr WORD UninstallControl = RepairControl + 1;

    struct Maintenance
    {
        explicit Maintenance(IBootstrapperEngine* bootstrapperEngine) : engine(bootstrapperEngine) {}

        IBootstrapperEngine* engine;
        std::mutex mutex;
        std::wstring installedCode;
        std::wstring installedVersion;
    };
    std::unique_ptr<Maintenance> instance;

    std::wstring RegistrationKey(const std::wstring& code)
    {
        return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + code;
    }

    bool IsInstalled(const std::wstring& key)
    {
        DWORD installed = 0;
        DWORD size = sizeof(installed);
        return RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Installed", RRF_RT_REG_DWORD,
            nullptr, &installed, &size) == ERROR_SUCCESS && installed == 1;
    }

    std::wstring CachedInstaller(const std::wstring& key)
    {
        wchar_t path[32768];
        DWORD size = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"BundleCachePath", RRF_RT_REG_SZ,
            nullptr, path, &size) != ERROR_SUCCESS)
        {
            return {};
        }

        // Use only the executable path registered by Burn; never invoke a registry command line.
        const size_t length = std::wcslen(path);
        if (length < 7 || path[1] != L':' || path[2] != L'\\' || _wcsicmp(path + length - 4, L".exe") != 0)
        {
            return {};
        }

        DWORD attributes = GetFileAttributesW(path);
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)
            ? std::wstring(path) : std::wstring();
    }

    void DetectRelated(Maintenance& maintenance, const BA_ONDETECTRELATEDBUNDLE_ARGS& related)
    {
        // Burn reports upgrade relations from the bundle's authored UpgradeCode.
        if (related.relationType != BOOTSTRAPPER_RELATION_UPGRADE || !related.fPerMachine ||
            !IsInstalled(RegistrationKey(related.wzBundleCode)))
        {
            return;
        }

        wchar_t setupVersion[128];
        SIZE_T length = _countof(setupVersion);
        int versionComparison = 0;
        if (FAILED(maintenance.engine->GetVariableVersion(L"WixBundleVersion", setupVersion, &length)) ||
            FAILED(maintenance.engine->CompareVersions(setupVersion, related.wzVersion, &versionComparison)))
        {
            return;
        }

        std::lock_guard guard(maintenance.mutex);
        if (!maintenance.installedVersion.empty())
        {
            int candidateComparison = 0;
            if (FAILED(maintenance.engine->CompareVersions(related.wzVersion,
                maintenance.installedVersion.c_str(), &candidateComparison)) || candidateComparison <= 0)
            {
                return;
            }
        }

        maintenance.installedCode = related.wzBundleCode;
        maintenance.installedVersion = related.wzVersion;
        const bool hasCache = !related.fMissingFromCache &&
            !CachedInstaller(RegistrationKey(maintenance.installedCode)).empty();
        maintenance.engine->SetVariableString(L"RelatedInstalledVersion", related.wzVersion, FALSE);
        maintenance.engine->SetVariableNumeric(L"RelatedInstallerAvailable", hasCache);
        maintenance.engine->SetVariableNumeric(L"RelatedUpgradeAvailable", versionComparison > 0);
        maintenance.engine->SetVariableNumeric(L"RelatedInstallDetected", TRUE);
        maintenance.engine->Log(BOOTSTRAPPER_LOG_LEVEL_STANDARD,
            hasCache ? L"v2rayN maintenance: registered installer available." : L"v2rayN maintenance: registered installer cache missing.");
        if (versionComparison > 0)
        {
            maintenance.engine->Log(BOOTSTRAPPER_LOG_LEVEL_STANDARD, L"v2rayN maintenance: upgrade available.");
        }
    }

    HRESULT WINAPI OnMessage(BA_FUNCTIONS_MESSAGE message, LPVOID args, LPVOID results, LPVOID context)
    {
        auto& maintenance = *static_cast<Maintenance*>(context);
        switch (message)
        {
        case BA_FUNCTIONS_MESSAGE_ONDETECTBEGIN:
        {
            std::lock_guard guard(maintenance.mutex);
            maintenance.installedCode.clear();
            maintenance.installedVersion.clear();
            maintenance.engine->SetVariableNumeric(L"RelatedInstallDetected", FALSE);
            maintenance.engine->SetVariableNumeric(L"RelatedInstallerAvailable", FALSE);
            maintenance.engine->SetVariableNumeric(L"RelatedUpgradeAvailable", FALSE);
            return S_OK;
        }
        case BA_FUNCTIONS_MESSAGE_ONDETECTRELATEDBUNDLE:
            DetectRelated(maintenance, *static_cast<const BA_ONDETECTRELATEDBUNDLE_ARGS*>(args));
            return S_OK;
        case BA_FUNCTIONS_MESSAGE_ONTHEMECONTROLLOADING:
        {
            const auto& control = *static_cast<const BA_FUNCTIONS_ONTHEMECONTROLLOADING_ARGS*>(args);
            auto& assigned = *static_cast<BA_FUNCTIONS_ONTHEMECONTROLLOADING_RESULTS*>(results);
            if (!control.wzName) { return E_NOTIMPL; }
            if (std::wcscmp(control.wzName, L"RelatedRepairButton") == 0) { assigned.wId = RepairControl; }
            else if (std::wcscmp(control.wzName, L"RelatedUninstallButton") == 0) { assigned.wId = UninstallControl; }
            else { return E_NOTIMPL; }
            assigned.fProcessed = TRUE;
            return S_OK;
        }
        case BA_FUNCTIONS_MESSAGE_ONTHEMECONTROLWMCOMMAND:
        {
            const auto& control = *static_cast<const BA_FUNCTIONS_ONTHEMECONTROLWMCOMMAND_ARGS*>(args);
            if (HIWORD(control.wParam) != BN_CLICKED || (control.wId != RepairControl && control.wId != UninstallControl))
            {
                return E_NOTIMPL;
            }

            auto& handled = *static_cast<BA_FUNCTIONS_ONTHEMECONTROLWMCOMMAND_RESULTS*>(results);
            handled.fProcessed = TRUE;
            handled.lResult = 0;
            std::wstring code;
            {
                std::lock_guard guard(maintenance.mutex);
                code = maintenance.installedCode;
            }
            const std::wstring key = RegistrationKey(code);
            const std::wstring setup = !code.empty() && IsInstalled(key) ? CachedInstaller(key) : std::wstring();
            SHELLEXECUTEINFOW launch = {};
            launch.cbSize = sizeof(launch);
            launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            launch.hwnd = GetAncestor(control.hWnd, GA_ROOT);
            launch.lpFile = setup.c_str();
            launch.lpParameters = control.wId == RepairControl ? L"/repair /passive /norestart" : L"/uninstall /passive /norestart";
            launch.nShow = SW_SHOWNORMAL;
            if (!setup.empty() && ShellExecuteExW(&launch))
            {
                PostMessageW(launch.hwnd, WM_CLOSE, 0, 0);
            }
            else
            {
                const DWORD error = setup.empty() ? ERROR_FILE_NOT_FOUND : GetLastError();
                wchar_t detail[512] = {};
                if (error != ERROR_CANCELLED && FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    nullptr, error, 0, detail, _countof(detail), nullptr))
                {
                    MessageBoxW(launch.hwnd, detail, L"v2rayN", MB_OK | MB_ICONERROR);
                }
            }
            return S_OK;
        }
        default:
            return E_NOTIMPL;
        }
    }
}

extern "C" HRESULT WINAPI BAFunctionsCreate(const BA_FUNCTIONS_CREATE_ARGS* args,
    BA_FUNCTIONS_CREATE_RESULTS* results)
{
    instance.reset(new (std::nothrow) Maintenance(args->pEngine));
    if (!instance) { return E_OUTOFMEMORY; }
    results->pfnBAFunctionsProc = OnMessage;
    results->pvBAFunctionsProcContext = instance.get();
    return S_OK;
}

extern "C" void WINAPI BAFunctionsDestroy(const BA_FUNCTIONS_DESTROY_ARGS*, BA_FUNCTIONS_DESTROY_RESULTS*)
{
    instance.reset();
}

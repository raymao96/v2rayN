[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PreviousSetup,
    [Parameter(Mandatory)][string]$CurrentSetup,
    [ValidateSet('wpf', 'desktop')][string]$Flavor = 'wpf',
    [switch]$VerifyUpgrade
)

$ErrorActionPreference = 'Stop'

$previousVersion = [version](Get-Item -LiteralPath $PreviousSetup).VersionInfo.FileVersion
$currentVersion = [version](Get-Item -LiteralPath $CurrentSetup).VersionInfo.FileVersion
if ($currentVersion -lt $previousVersion -or ($VerifyUpgrade -and $currentVersion -eq $previousVersion)) {
    throw "Invalid maintenance test versions: previous $previousVersion, current $currentVersion."
}
$testUpgrade = $currentVersion -gt $previousVersion
$testReplacement = $testUpgrade -or (Get-FileHash -LiteralPath $PreviousSetup).Hash -ne (Get-FileHash -LiteralPath $CurrentSetup).Hash

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class MaintenanceWindow
{
    private delegate bool WindowCallback(IntPtr window, IntPtr data);
    private static IntPtr operationWindow;

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll")]
    private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    public static bool ClickInstallAction(bool freshInstall, bool sameBundle)
    {
        var clicked = false;
        EnumWindows((window, data) =>
        {
            if (!IsWindowVisible(window) || !IsWindowEnabled(window)) return true;
            var reinstall = GetDlgItem(window, sameBundle ? 0x8004 : 0x4000);
            var uninstall = GetDlgItem(window, sameBundle ? 0x8005 : 0x4001);
            var install = GetDlgItem(window, 0x8000);
            var fresh = GetDlgItem(window, 0x4002);
            if (install == IntPtr.Zero || fresh == IntPtr.Zero || reinstall == IntPtr.Zero ||
                uninstall == IntPtr.Zero || IsWindowVisible(install)) return true;
            if (IsWindowVisible(fresh) != freshInstall || IsWindowVisible(reinstall) == freshInstall ||
                IsWindowVisible(uninstall) == freshInstall) return true;

            var action = freshInstall ? fresh : reinstall;
            clicked = IsWindowEnabled(action) && (freshInstall || IsWindowEnabled(uninstall)) &&
                PostMessage(action, 0x00F5, IntPtr.Zero, IntPtr.Zero);
            if (clicked) operationWindow = window;
            return !clicked;
        }, IntPtr.Zero);
        return clicked;
    }

    public static bool CloseOperationWindow()
    {
        return operationWindow != IntPtr.Zero && IsWindow(operationWindow) &&
            PostMessage(operationWindow, 0x0010, IntPtr.Zero, IntPtr.Zero);
    }
}
'@

function Wait-Condition([scriptblock]$Condition, [string]$Failure, [int]$TimeoutSeconds = 60) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Failure
}

function Invoke-InteractiveInstallAction([string]$Log, [bool]$FreshInstall, [bool]$SameBundle) {
    $action = if ($FreshInstall) { 'fresh install' } else { 'reinstall' }
    $process = Start-Process -FilePath $CurrentSetup -ArgumentList "/log `"$Log`"" -PassThru
    Wait-Condition { [MaintenanceWindow]::ClickInstallAction($FreshInstall, $SameBundle) } "The $Flavor window did not show the expected buttons for $action."
    Wait-Condition {
        (Test-Path -LiteralPath $Log -PathType Leaf) -and
        [bool](Select-String -LiteralPath $Log -Pattern 'Apply complete, result: 0x0' -Quiet)
    } "The $Flavor $action did not complete successfully. See $Log." 180
    Wait-Condition {
        if ($process.HasExited) { return $true }
        [void][MaintenanceWindow]::CloseOperationWindow()
        return $process.WaitForExit(500)
    } "The $Flavor $action window did not close after completion." 30
    if ($process.ExitCode -ne 0) { throw "The $Flavor $action exited with $($process.ExitCode)." }
}

function Assert-SentinelConfig([string]$ConfigDir, [string]$ExpectedJson, [string]$ExpectedNested) {
    $jsonPath = Join-Path $ConfigDir 'guiNConfig.json'
    $nestedPath = Join-Path $ConfigDir 'custom/sentinel.txt'
    if (!(Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "Missing preserved config file: $jsonPath"
    }
    if (!(Test-Path -LiteralPath $nestedPath -PathType Leaf)) {
        throw "Missing preserved nested config file: $nestedPath"
    }
    if ((Get-Content -LiteralPath $jsonPath -Raw) -ne $ExpectedJson) {
        throw "Preserved config content mismatch: $jsonPath"
    }
    if ((Get-Content -LiteralPath $nestedPath -Raw) -ne $ExpectedNested) {
        throw "Preserved nested config content mismatch: $nestedPath"
    }
}

function Assert-NoBackupLeftover([string]$InstallDir) {
    $leftovers = @(Get-ChildItem -LiteralPath $InstallDir -Directory -Force -Filter '.v2rayN-upgrade-guiConfigs-*' -ErrorAction SilentlyContinue)
    if ($leftovers.Count -gt 0) {
        throw "Leftover upgrade backup directory found: $($leftovers.FullName -join ', ')"
    }
}

function Assert-PreserveLog([string]$BurnLog, [string[]]$Patterns) {
    $msiLogs = @(Get-ChildItem -LiteralPath (Split-Path -Parent $BurnLog) -Filter ("$([System.IO.Path]::GetFileNameWithoutExtension($BurnLog))_*.log") -File -ErrorAction SilentlyContinue)
    if ($msiLogs.Count -eq 0) {
        throw "No MSI log found beside $BurnLog"
    }
    foreach ($pattern in $Patterns) {
        if (-not (Select-String -Path $msiLogs.FullName -Pattern $pattern -SimpleMatch -Quiet)) {
            throw "Expected MSI log entry '$pattern' not found for $BurnLog"
        }
    }
}

$folderName = if ($Flavor -eq 'wpf') { 'v2rayN' } else { 'v2rayN Desktop' }
$installDir = Join-Path $env:ProgramFiles $folderName
$app = Join-Path $installDir 'v2rayN.exe'
$configDir = Join-Path $installDir 'guiConfigs'
$logPrefix = "v2rayN-$Flavor"

$freshLog = Join-Path $env:RUNNER_TEMP "$logPrefix-fresh-install.log"
Invoke-InteractiveInstallAction $freshLog $true $false
if (!(Test-Path -LiteralPath $app -PathType Leaf)) {
    throw "Fresh $Flavor installation did not install v2rayN.exe."
}
$freshUninstall = Start-Process -FilePath $CurrentSetup -ArgumentList '/uninstall', '/quiet', '/norestart' -Wait -PassThru
if ($freshUninstall.ExitCode -ne 0 -or (Test-Path -LiteralPath $app)) {
    throw "Removing the fresh $Flavor installation failed: $($freshUninstall.ExitCode)"
}

$install = Start-Process -FilePath $PreviousSetup -ArgumentList '/quiet', '/norestart' -Wait -PassThru
if ($install.ExitCode -ne 0 -or !(Test-Path -LiteralPath $app -PathType Leaf)) {
    throw "Installing the previous $Flavor bundle failed: $($install.ExitCode)"
}

$logs = [System.Collections.Generic.List[string]]::new()
$licenseBlocker = Join-Path $installDir 'LICENSE.txt'
try {
    $sentinelJson = '{"sentinel":"preserve-upgrade-config","flavor":"' + $Flavor + '"}'
    $sentinelNested = "nested-config-data-$Flavor"
    New-Item -ItemType Directory -Force -Path (Join-Path $configDir 'custom') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $configDir 'guiNConfig.json'), $sentinelJson, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $configDir 'custom/sentinel.txt'), $sentinelNested, [System.Text.UTF8Encoding]::new($false))

    if ($testUpgrade) {
        if (Test-Path -LiteralPath $licenseBlocker -PathType Leaf) {
            Remove-Item -LiteralPath $licenseBlocker -Force
        }
        New-Item -ItemType Directory -Force -Path $licenseBlocker | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $licenseBlocker 'block.txt'), 'force-rollback', [System.Text.UTF8Encoding]::new($false))

        $rollbackLog = Join-Path $env:RUNNER_TEMP "$logPrefix-upgrade-rollback.log"
        $logs.Add($rollbackLog)
        $failedUpgrade = Start-Process -FilePath $CurrentSetup -ArgumentList '/quiet', '/norestart', "/log `"$rollbackLog`"" -Wait -PassThru
        if ($failedUpgrade.ExitCode -eq 0) {
            throw "Expected $Flavor upgrade with blocked LICENSE.txt to fail and roll back."
        }
        Assert-SentinelConfig $configDir $sentinelJson $sentinelNested
        Assert-NoBackupLeftover $installDir
        Assert-PreserveLog $rollbackLog @('v2rayN ConfigPreserve: Backed up guiConfigs', 'v2rayN ConfigPreserve: Restored guiConfigs')
        Remove-Item -LiteralPath $licenseBlocker -Recurse -Force

        $reinstall = Start-Process -FilePath $PreviousSetup -ArgumentList '/repair', '/quiet', '/norestart' -Wait -PassThru
        if ($reinstall.ExitCode -ne 0 -or !(Test-Path -LiteralPath $app -PathType Leaf)) {
            throw "Restoring the previous $Flavor baseline failed: $($reinstall.ExitCode)"
        }
        Assert-SentinelConfig $configDir $sentinelJson $sentinelNested
    }

    if ($testReplacement) {
        $upgradeLog = Join-Path $env:RUNNER_TEMP "$logPrefix-upgrade.log"
        $logs.Add($upgradeLog)
        Invoke-InteractiveInstallAction $upgradeLog $false $false
        Assert-SentinelConfig $configDir $sentinelJson $sentinelNested
        Assert-NoBackupLeftover $installDir
        Assert-PreserveLog $upgradeLog @('v2rayN ConfigPreserve: Backed up guiConfigs', 'v2rayN ConfigPreserve: Restored guiConfigs')
    }

    $licenseHash = (Get-FileHash -LiteralPath $licenseBlocker).Hash
    Add-Content -LiteralPath $licenseBlocker -Value 'same-version reinstall sentinel'
    $reinstallLog = Join-Path $env:RUNNER_TEMP "$logPrefix-self-reinstall.log"
    $logs.Add($reinstallLog)
    Invoke-InteractiveInstallAction $reinstallLog $false $true
    if (!(Test-Path -LiteralPath $app -PathType Leaf) -or (Get-FileHash -LiteralPath $licenseBlocker).Hash -ne $licenseHash) {
        throw "Same-version $Flavor reinstall did not restore the installed files."
    }
    Assert-SentinelConfig $configDir $sentinelJson $sentinelNested

    $uninstallLog = Join-Path $env:RUNNER_TEMP "$logPrefix-self-uninstall.log"
    $logs.Add($uninstallLog)
    $uninstall = Start-Process -FilePath $CurrentSetup -ArgumentList '/uninstall', '/quiet', '/norestart', "/log `"$uninstallLog`"" -Wait -PassThru
    if ($uninstall.ExitCode -ne 0 -or (Test-Path -LiteralPath $app)) {
        throw "Same-version $Flavor uninstall failed: $($uninstall.ExitCode)"
    }
    Write-Host "Verified $Flavor interactive reinstall, guiConfigs preservation, and uninstall."
} finally {
    if (Test-Path -LiteralPath $licenseBlocker -PathType Container) {
        Remove-Item -LiteralPath $licenseBlocker -Recurse -Force -ErrorAction SilentlyContinue
    }
    foreach ($log in $logs) {
        if ($log -and (Test-Path -LiteralPath $log)) {
            Select-String -LiteralPath $log -Pattern 'v2rayN maintenance:|Detected related bundle|Apply complete'
            $msiLogs = @(Get-ChildItem -LiteralPath (Split-Path -Parent $log) -Filter ("$([System.IO.Path]::GetFileNameWithoutExtension($log))_*.log") -File -ErrorAction SilentlyContinue)
            foreach ($msiLog in $msiLogs) {
                Select-String -LiteralPath $msiLog.FullName -Pattern 'v2rayN ConfigPreserve:'
            }
        }
    }
    if (Test-Path -LiteralPath $app) {
        Start-Process -FilePath $CurrentSetup -ArgumentList '/uninstall', '/quiet', '/norestart' -Wait -ErrorAction SilentlyContinue
        Start-Process -FilePath $PreviousSetup -ArgumentList '/uninstall', '/quiet', '/norestart' -Wait
    }
}

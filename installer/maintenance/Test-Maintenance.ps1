[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PreviousSetup,
    [Parameter(Mandatory)][string]$CurrentSetup
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class MaintenanceWindow
{
    private delegate bool WindowCallback(IntPtr window, IntPtr data);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll")]
    private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    public static bool Click(int id)
    {
        var clicked = false;
        EnumWindows((window, data) =>
        {
            if (!IsWindowVisible(window) || !IsWindowEnabled(window)) return true;
            var repair = GetDlgItem(window, 0x4000);
            var uninstall = GetDlgItem(window, 0x4001);
            if (repair == IntPtr.Zero || uninstall == IntPtr.Zero ||
                !IsWindowVisible(repair) || !IsWindowVisible(uninstall)) return true;

            var button = id == 0x4000 ? repair : uninstall;
            clicked = IsWindowEnabled(button) && PostMessage(button, 0x00F5, IntPtr.Zero, IntPtr.Zero);
            return !clicked;
        }, IntPtr.Zero);
        return clicked;
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

$app = Join-Path $env:ProgramFiles 'v2rayN/v2rayN.exe'
$install = Start-Process -FilePath $PreviousSetup -ArgumentList '/quiet', '/norestart' -Wait -PassThru
if ($install.ExitCode -ne 0 -or !(Test-Path -LiteralPath $app -PathType Leaf)) {
    throw "Installing the previous WPF bundle failed: $($install.ExitCode)"
}
try {
    Move-Item -LiteralPath $app -Destination (Join-Path $env:RUNNER_TEMP 'v2rayN.before-repair.exe')
    $repairLog = Join-Path $env:RUNNER_TEMP 'v2rayN-related-repair.log'
    $repair = Start-Process -FilePath $CurrentSetup -ArgumentList "/log `"$repairLog`"" -PassThru
    Wait-Condition { [MaintenanceWindow]::Click(0x4000) } 'The related install did not show enabled maintenance buttons.'
    Wait-Condition { Test-Path -LiteralPath $app -PathType Leaf } 'Repair did not restore v2rayN.exe.' 120
    if (!$repair.WaitForExit(30000)) { throw 'The maintenance window did not close after dispatching repair.' }

    $uninstallLog = Join-Path $env:RUNNER_TEMP 'v2rayN-related-uninstall.log'
    $uninstall = Start-Process -FilePath $CurrentSetup -ArgumentList "/log `"$uninstallLog`"" -PassThru
    Wait-Condition { [MaintenanceWindow]::Click(0x4001) } 'The maintenance window did not offer uninstall after repair.'
    Wait-Condition { !(Test-Path -LiteralPath $app) } 'Uninstall did not remove v2rayN.exe.' 120
    if (!$uninstall.WaitForExit(30000)) { throw 'The maintenance window did not close after dispatching uninstall.' }
    Write-Host 'The current setup detected, repaired and uninstalled the previous WPF bundle.'
} finally {
    foreach ($log in @($repairLog, $uninstallLog)) {
        if ($log -and (Test-Path -LiteralPath $log)) {
            Select-String -LiteralPath $log -Pattern 'v2rayN maintenance:|Detected related bundle'
        }
    }
    if (Test-Path -LiteralPath $app) {
        Start-Process -FilePath $PreviousSetup -ArgumentList '/uninstall', '/quiet', '/norestart' -Wait
    }
}

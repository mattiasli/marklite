<#
Installer smoke test. Installs MarkLite silently, checks what Setup left behind, then uninstalls
and checks that it is all gone again. CI runs it for both install scopes.

  pwsh tests/installer-smoke.ps1 -Setup MarkLite-Setup.exe -AppExe MarkLite.exe [-AllUsers] [-SkipLaunch]

-AllUsers needs an elevated shell. -SkipLaunch skips starting the installed MarkLite (it would read
and write the current user's MarkLite settings); CI starts it to test the "MarkLite is open" check.
#>
param(
    [Parameter(Mandatory)] [string] $Setup,
    [string] $AppExe = "",
    [switch] $AllUsers,
    [switch] $SkipLaunch
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$failures = 0
function Check([bool] $condition, [string] $what) {
    if ($condition) { Write-Host "  ok    $what" }
    else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures++ }
}

function Run([string] $exe, [string[]] $arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode
}

$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'VERSION') -Raw).Trim()
$setupPath = (Resolve-Path $Setup).Path
$hive = if ($AllUsers) { 'HKLM:' } else { 'HKCU:' }
$uninstallKey = "$hive\Software\Microsoft\Windows\CurrentVersion\Uninstall\MarkLite"
$progId = "$hive\Software\Classes\MarkLite.Markdown"
$startMenu = Join-Path ([Environment]::GetFolderPath($(if ($AllUsers) { 'CommonPrograms' } else { 'Programs' }))) 'MarkLite.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath($(if ($AllUsers) { 'CommonDesktopDirectory' } else { 'DesktopDirectory' }))) 'MarkLite.lnk'
# A folder name with spaces checks the quoting of every path Setup writes.
$dir = Join-Path ([IO.Path]::GetTempPath()) ("MarkLite smoke " + [guid]::NewGuid().ToString('N').Substring(0, 8))
$exe = Join-Path $dir 'MarkLite.exe'
$uninstaller = Join-Path $dir 'uninstall.exe'
$scopeArg = if ($AllUsers) { '--all-users' } else { '--just-me' }

if (Test-Path $uninstallKey) { throw "MarkLite is already installed in this scope; uninstall it before running the smoke test." }

Write-Host "Setup $version, $(if ($AllUsers) { 'all users' } else { 'current user' }), into '$dir'"

# "Open with" may already know another MarkLite.exe, such as a portable copy. Setup must leave that
# entry alone. If there is none, a stand-in is created for the test and removed at the end.
$appKey = "$hive\Software\Classes\Applications\MarkLite.exe"
$foreignCommand = (Get-ItemProperty "$appKey\shell\open\command" -ErrorAction SilentlyContinue).'(default)'
$madeForeign = $false
if (-not $foreignCommand) {
    $foreignCommand = '"C:\Portable\MarkLite.exe" "%1"'
    New-Item "$appKey\shell\open\command" -Force | Out-Null
    Set-ItemProperty "$appKey\shell\open\command" -Name '(default)' -Value $foreignCommand
    $madeForeign = $true
}

Write-Host "install"
$code = Run $setupPath @('--silent', $scopeArg, "`"--dir=$dir`"")
Check ($code -eq 0) "silent install exits with 0 (got $code)"
Check (Test-Path $exe) "MarkLite.exe is installed"
Check (Test-Path $uninstaller) "uninstall.exe is installed"
if ($AppExe) {
    Check ((Get-FileHash $exe).Hash -eq (Get-FileHash $AppExe).Hash) "installed MarkLite.exe is identical to the build"
}
Check ((Get-Item $exe).VersionInfo.ProductVersion -eq $version) "MarkLite.exe reports version $version"

$entry = Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue
Check ($null -ne $entry) "Apps & features entry exists"
if ($entry) {
    Check ($entry.DisplayName -eq 'MarkLite') "DisplayName is MarkLite"
    Check ($entry.DisplayVersion -eq $version) "DisplayVersion is $version"
    Check ($entry.Publisher -eq 'Mattias Li') "Publisher is Mattias Li"
    Check ($entry.InstallLocation -eq $dir) "InstallLocation is the install folder"
    Check ($entry.UninstallString -eq "`"$uninstaller`"") "UninstallString points at uninstall.exe"
    Check ($entry.QuietUninstallString -eq "`"$uninstaller`" --silent") "QuietUninstallString is set"
    Check ($entry.EstimatedSize -gt 500) "EstimatedSize is set"
}

$shell = New-Object -ComObject WScript.Shell
Check (Test-Path $startMenu) "Start menu shortcut exists"
if (Test-Path $startMenu) { Check ($shell.CreateShortcut($startMenu).TargetPath -eq $exe) "Start menu shortcut points at MarkLite.exe" }
Check (Test-Path $desktop) "desktop shortcut exists"

$command = (Get-ItemProperty "$progId\shell\open\command" -ErrorAction SilentlyContinue).'(default)'
Check ($command -eq "`"$exe`" `"%1`"") "MarkLite.Markdown open command is registered"
foreach ($ext in '.md', '.markdown') {
    $owp = Get-ItemProperty "$hive\Software\Classes\$ext\OpenWithProgids" -ErrorAction SilentlyContinue
    Check ($null -ne $owp -and $owp.PSObject.Properties.Name -contains 'MarkLite.Markdown') "$ext lists MarkLite under Open with"
}
$apps = Get-ItemProperty "$hive\Software\RegisteredApplications" -ErrorAction SilentlyContinue
Check ($null -ne $apps -and $apps.MarkLite -eq 'Software\MarkLite\Capabilities') "MarkLite is listed in Default apps"

Write-Host "reinstall over the existing installation, without the desktop shortcut"
$code = Run $setupPath @('--silent', $scopeArg, "`"--dir=$dir`"", '--no-desktop-shortcut')
Check ($code -eq 0) "second install exits with 0 (got $code)"
Check (-not (Test-Path $desktop)) "desktop shortcut was removed when not wanted"
Check ((Get-ItemProperty $uninstallKey).MarkLiteDesktopShortcut -eq 0) "the choice is remembered"

if (-not $SkipLaunch) {
    Write-Host "uninstall while MarkLite is running"
    $app = Start-Process -FilePath $exe -ArgumentList '--new' -PassThru
    Start-Sleep -Seconds 3
    Check (-not $app.HasExited) "installed MarkLite starts"
    $code = Run $uninstaller @('--silent')
    Check ($code -eq 0x20000001) "uninstall refuses while MarkLite is open (got $code)"
    Check (Test-Path $exe) "MarkLite.exe is still there"
    Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
    $app.WaitForExit(10000) | Out-Null
}

Write-Host "uninstall"
$code = Run $uninstaller @('--silent')
Check ($code -eq 0) "silent uninstall exits with 0 (got $code)"
# uninstall.exe deletes itself (and the folder) right after it exits.
for ($i = 0; $i -lt 30 -and (Test-Path $dir); $i++) { Start-Sleep -Seconds 1 }
Check (-not (Test-Path $dir)) "install folder is gone"
Check (-not (Test-Path $uninstallKey)) "Apps & features entry is gone"
Check (-not (Test-Path $startMenu)) "Start menu shortcut is gone"
Check (-not (Test-Path $progId)) "MarkLite.Markdown is unregistered"
$owp = Get-ItemProperty "$hive\Software\Classes\.md\OpenWithProgids" -ErrorAction SilentlyContinue
Check ($null -eq $owp -or -not ($owp.PSObject.Properties.Name -contains 'MarkLite.Markdown')) ".md no longer lists MarkLite"
$apps = Get-ItemProperty "$hive\Software\RegisteredApplications" -ErrorAction SilentlyContinue
Check ($null -eq $apps -or -not ($apps.PSObject.Properties.Name -contains 'MarkLite')) "Default apps entry is gone"
$after = (Get-ItemProperty "$appKey\shell\open\command" -ErrorAction SilentlyContinue).'(default)'
Check ($after -eq $foreignCommand) "another MarkLite.exe under Open with is left alone"
if ($madeForeign) { Remove-Item $appKey -Recurse -Force }

Write-Host "the window opens and closes"
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SmokeWin {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$ui = Start-Process -FilePath $setupPath -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $hwnd -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $hwnd = [SmokeWin]::FindWindowW('MarkLiteSetup', [IntPtr]::Zero) }
Check ($hwnd -ne [IntPtr]::Zero) "Setup window appears"
if ($hwnd -ne [IntPtr]::Zero) { [SmokeWin]::PostMessageW($hwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
$closed = $ui.WaitForExit(10000)
Check $closed "Setup window closes"
if ($closed) { Check ($ui.ExitCode -eq 1223) "closing Setup without installing reports 'cancelled' (got $($ui.ExitCode))" }
else { Stop-Process -Id $ui.Id -Force }

if ($failures) {
    Write-Host "$failures check(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host "all checks passed"

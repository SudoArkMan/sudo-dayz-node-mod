# Installs from an unpacked package, checks what it created, uninstalls, and
# checks that all of it is gone.
#
#   pwsh -File tools/verify-install.ps1 -PackageDir <the unpacked folder>
#
# The install goes into a scratch folder rather than the default location, so a
# verification run does not become an install nobody asked for. The Start menu
# shortcut and the Add or remove programs entry have fixed locations and cannot
# be redirected, so this refuses to run at all when either already exists: an
# existing install belongs to the user and is not something a check may take
# apart and put back.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PackageDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ProductName = 'SUDO DayZ Node Mod'
$ExeName     = 'DAYZSUDONodeMod.exe'
$RegistryKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SUDODayZNodeMod'
$StartLink   = Join-Path ([Environment]::GetFolderPath('Programs')) "$ProductName.lnk"
$DesktopLink = Join-Path ([Environment]::GetFolderPath('Desktop'))  "$ProductName.lnk"

$problems = @()
function Check {
    param([string] $What, [bool] $Ok)
    if ($Ok) { Write-Host ("  ok    " + $What) }
    else     { Write-Host ("  FAIL  " + $What); $script:problems += $What }
}

Write-Host ''
Write-Host 'Install and uninstall round trip'
Write-Host '--------------------------------'

if (Test-Path -LiteralPath $RegistryKey) {
    Write-Host "  Skipped: $ProductName is already listed in Add or remove programs."
    Write-Host '  Uninstall it first if you want this check to run.'
    exit 0
}
if (Test-Path -LiteralPath $StartLink) {
    Write-Host "  Skipped: a Start menu shortcut already exists at $StartLink."
    exit 0
}

$script = Join-Path $PackageDir 'install.ps1'
if (-not (Test-Path -LiteralPath $script)) {
    Write-Host "  FAIL  install.ps1 is not in the package"
    exit 1
}

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("nodemod-install-" + [guid]::NewGuid().ToString('N').Substring(0, 8))

$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
& $powershell -NoProfile -ExecutionPolicy Bypass -File $script -Silent -InstallDir $scratch
if ($LASTEXITCODE -ne 0) {
    Write-Host '  FAIL  the install returned a failure'
    exit 1
}

Write-Host ''
Write-Host '  After installing:'
Check "the executable is at $scratch"      (Test-Path -LiteralPath (Join-Path $scratch $ExeName))
Check 'Qt6Core.dll went with it'           (Test-Path -LiteralPath (Join-Path $scratch 'Qt6Core.dll'))
Check 'resources went with it'             (Test-Path -LiteralPath (Join-Path $scratch 'resources/catalog.json'))
Check 'the Start menu shortcut exists'     (Test-Path -LiteralPath $StartLink)
Check 'the Add or remove programs entry exists' (Test-Path -LiteralPath $RegistryKey)

if (Test-Path -LiteralPath $RegistryKey) {
    $entry = Get-ItemProperty -LiteralPath $RegistryKey
    Check 'it names the product'    ($entry.DisplayName -eq $ProductName)
    Check 'it carries a version'    ([bool]$entry.DisplayVersion -and $entry.DisplayVersion -ne 'unknown')
    Check 'it points at the folder' ($entry.InstallLocation -eq $scratch)
    Check 'it can uninstall itself' ([bool]$entry.UninstallString)
    Write-Host ("        version " + $entry.DisplayVersion)
}

& $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scratch 'install.ps1') `
    -Uninstall -InstallDir $scratch
if ($LASTEXITCODE -ne 0) {
    Write-Host '  FAIL  the uninstall returned a failure'
    $problems += 'uninstall returned a failure'
}

# The uninstaller hands the last of the work to a copy of itself outside the
# folder and waits for it, so the folder should be gone the moment the command
# above returned. Given a few seconds anyway, because a virus scanner holding a
# handle open is a real thing and a fixed answer here would be a flaky check.
$deadline = (Get-Date).AddSeconds(20)
while ((Test-Path -LiteralPath $scratch) -and ((Get-Date) -lt $deadline)) {
    Start-Sleep -Milliseconds 500
}

Write-Host ''
Write-Host '  After uninstalling:'
Check 'the install folder is gone'              (-not (Test-Path -LiteralPath $scratch))
Check 'the Start menu shortcut is gone'         (-not (Test-Path -LiteralPath $StartLink))
Check 'the desktop shortcut is gone'            (-not (Test-Path -LiteralPath $DesktopLink))
Check 'the Add or remove programs entry is gone' (-not (Test-Path -LiteralPath $RegistryKey))

# A last sweep for anything left in the scratch folder, since a partial delete
# is the failure that looks like a pass when only the top level is checked.
if (Test-Path -LiteralPath $scratch) {
    Write-Host '  Left behind:'
    Get-ChildItem -LiteralPath $scratch -Recurse -Force |
        Select-Object -First 20 |
        ForEach-Object { Write-Host ("    " + $_.FullName) }
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($problems.Count -gt 0) {
    Write-Host ("  " + $problems.Count + " checks failed.")
    exit 1
}
Write-Host '  Installed, listed, shortcut made, then removed with nothing left behind.'
exit 0

# Installs SUDO DayZ Node Mod for the current user, and uninstalls it again.
#
# Per user on purpose. Everything it writes lives under the current account, so
# it never asks for elevation and a machine with a locked down administrator
# account can still run it:
#
#   %LOCALAPPDATA%\Programs\SUDO DayZ Node Mod    the application
#   Start Menu\Programs\SUDO DayZ Node Mod.lnk    the shortcut
#   HKCU\...\CurrentVersion\Uninstall\...         the Add or remove programs entry
#
# Those three are the whole of it, and -Uninstall removes all three. There is no
# service, no scheduled task, no file association and nothing under Program
# Files or HKLM.
#
# The version is read off the executable rather than written here, so this file
# does not need editing when the version changes.
#
# Run it through Install.cmd, or directly:
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 -Uninstall

[CmdletBinding()]
param(
    # Where to install. The default is under the current user's local app data.
    [string] $InstallDir,

    # Remove an install made by this script.
    [switch] $Uninstall,

    # No questions. Accepts the licence terms on the command line rather than at
    # a prompt, which is what an unattended install has to do.
    [switch] $Silent,

    # Skip the Start menu shortcut.
    [switch] $NoShortcut,

    # Add a desktop shortcut as well as the Start menu one.
    [switch] $DesktopShortcut,

    # Uninstall only. Also removes the settings the app keeps for this user:
    # the recent projects list, the window layout and the update preferences.
    # Left alone by default so reinstalling keeps them.
    [switch] $RemoveSettings
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ProductName  = 'SUDO DayZ Node Mod'
$ExeName      = 'DAYZSUDONodeMod.exe'
$Publisher    = 'Dillan Stephenson'
$RegistryKey  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SUDODayZNodeMod'

# Where the app keeps what it remembers, from the organisation and application
# names main.cpp sets. This one key and this one folder, not the SUDO key above
# them: that one is shared with anything else published under the same name and
# is not this application's to delete.
$SettingsKey  = 'HKCU:\Software\SUDO\SUDO DayZ Node Mod'
$AppDataDir   = Join-Path $env:APPDATA 'SUDO\SUDO DayZ Node Mod'

function Write-Step  { param([string] $Text) Write-Host "  $Text" }
function Write-Title { param([string] $Text) Write-Host ''; Write-Host $Text; Write-Host ('-' * $Text.Length) }

function Get-DefaultInstallDir {
    Join-Path (Join-Path $env:LOCALAPPDATA 'Programs') $ProductName
}

function Get-ProductVersion {
    param([string] $ExePath)
    if (-not (Test-Path -LiteralPath $ExePath)) { return 'unknown' }
    $info = (Get-Item -LiteralPath $ExePath).VersionInfo
    if ($info.ProductVersion) { return $info.ProductVersion.Trim() }
    return 'unknown'
}

# A copy over a running executable fails part way and leaves a folder that is
# neither the old install nor the new one. Asked first, and never resolved by
# closing the application on the user's behalf: the answer to a running editor
# is the person using it, not this script.
function Assert-NotRunning {
    param([string] $Dir)
    $exe = Join-Path $Dir $ExeName
    if (-not (Test-Path -LiteralPath $exe)) { return }
    # Matched on the full path, not on the name, so another copy of the editor
    # running from somewhere else is not mistaken for this one.
    $running = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ExeName)) `
                    -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and ($_.Path -ieq $exe) })
    if ($running.Count -gt 0) {
        throw ("$ProductName is running from $Dir. Close it and run this again. " +
               "Process id: " + (($running | ForEach-Object { $_.Id }) -join ', '))
    }
}

function New-Shortcut {
    param([string] $LinkPath, [string] $TargetPath, [string] $WorkingDirectory)
    $shell = New-Object -ComObject WScript.Shell
    try {
        $link = $shell.CreateShortcut($LinkPath)
        $link.TargetPath       = $TargetPath
        $link.WorkingDirectory = $WorkingDirectory
        $link.IconLocation     = "$TargetPath,0"
        $link.Description      = 'Visual scripting for DayZ Enforce Script'
        $link.Save()
    } finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell)
    }
}

function Get-ShortcutPaths {
    @(
        (Join-Path ([Environment]::GetFolderPath('Programs')) "$ProductName.lnk"),
        (Join-Path ([Environment]::GetFolderPath('Desktop'))  "$ProductName.lnk")
    )
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------

function Invoke-Uninstall {
    param([string] $Dir)

    if (-not $Dir) {
        if (Test-Path -LiteralPath $RegistryKey) {
            $recorded = Get-ItemProperty -LiteralPath $RegistryKey
            if ($recorded.PSObject.Properties.Name -contains 'InstallLocation' -and
                $recorded.InstallLocation) {
                $Dir = $recorded.InstallLocation
            }
        }
    }
    if (-not $Dir) { $Dir = Get-DefaultInstallDir }

    Assert-NotRunning -Dir $Dir

    # A script cannot delete the folder it is running out of, and that folder is
    # exactly what an uninstall has to delete. So when this copy is inside the
    # install, it copies itself to a temporary file, hands the job to that copy
    # and waits for it.
    #
    # Waiting is the point. The obvious alternative is to delete what can be
    # deleted and leave a detached command to sweep up the rest, and that both
    # makes the result arrive after the caller has stopped looking and puts the
    # paths through another round of shell quoting, which every path here has a
    # space in. Handing over and waiting has neither problem: when this returns,
    # the folder is gone rather than scheduled to go.
    $self = $PSCommandPath
    $prefix = [System.IO.Path]::GetFullPath($Dir).TrimEnd('\', '/') + '\'
    if ($self -and (Test-Path -LiteralPath $Dir) -and
        $self.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {

        $relay = Join-Path ([System.IO.Path]::GetTempPath()) `
            ('nodemod-uninstall-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.ps1')
        Copy-Item -LiteralPath $self -Destination $relay -Force

        $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $relayArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $relay,
                       '-Uninstall', '-InstallDir', $Dir)
        if ($Silent)         { $relayArgs += '-Silent' }
        if ($RemoveSettings) { $relayArgs += '-RemoveSettings' }

        try {
            $relayed = Start-Process -FilePath $powershell -ArgumentList $relayArgs `
                           -Wait -PassThru -NoNewWindow
            return $relayed.ExitCode
        } finally {
            Remove-Item -LiteralPath $relay -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Title "Uninstalling $ProductName"

    foreach ($link in Get-ShortcutPaths) {
        if (Test-Path -LiteralPath $link) {
            Remove-Item -LiteralPath $link -Force
            Write-Step "Removed $link"
        }
    }

    if (Test-Path -LiteralPath $RegistryKey) {
        Remove-Item -LiteralPath $RegistryKey -Recurse -Force
        Write-Step "Removed the Add or remove programs entry"
    }

    if ($RemoveSettings) {
        if (Test-Path -LiteralPath $SettingsKey) {
            Remove-Item -LiteralPath $SettingsKey -Recurse -Force
            Write-Step 'Removed the saved settings'
        }
        if (Test-Path -LiteralPath $AppDataDir) {
            # The recent projects list and the mod library cache.
            Remove-Item -LiteralPath $AppDataDir -Recurse -Force
            Write-Step "Removed $AppDataDir"
        }
    }

    if (Test-Path -LiteralPath $Dir) {
        Remove-Item -LiteralPath $Dir -Recurse -Force
        Write-Step "Removed $Dir"
    } else {
        Write-Step "Nothing installed at $Dir"
    }

    Write-Host ''
    Write-Host "$ProductName has been removed."
    if (-not $RemoveSettings) {
        Write-Host 'Your settings and recent projects were kept. Add -RemoveSettings to clear them too.'
    }
    return 0
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

function Show-Licences {
    param([string] $Source)

    Write-Title 'Licences'
    Write-Host "  $ProductName is released under the MIT licence. See LICENSE."
    Write-Host '  It ships the Qt libraries under the LGPL version 3, and the MinGW'
    Write-Host '  runtime libraries under the GCC runtime library exception.'
    Write-Host '  The full texts are in the licenses folder beside this script:'
    $dir = Join-Path $Source 'licenses'
    if (Test-Path -LiteralPath $dir) {
        Get-ChildItem -LiteralPath $dir -File | ForEach-Object {
            Write-Host ("    " + $_.Name)
        }
    }
    foreach ($doc in @('LICENSE', 'THIRD-PARTY-NOTICES.md')) {
        if (Test-Path -LiteralPath (Join-Path $Source $doc)) { Write-Host "    $doc" }
    }
}

function Invoke-Install {
    param([string] $Source, [string] $Dir)

    $exe = Join-Path $Source $ExeName
    if (-not (Test-Path -LiteralPath $exe)) {
        throw ("$ExeName is not next to this script. Run install.ps1 from the " +
               "folder the ZIP unpacked into, not from a copy of the script on its own.")
    }
    $version = Get-ProductVersion -ExePath $exe

    Write-Title "$ProductName $version"
    Write-Step "From: $Source"
    Write-Step "To:   $Dir"

    Show-Licences -Source $Source

    if (-not $Silent) {
        Write-Host ''
        $answer = Read-Host 'Install, and accept those terms? [y/N]'
        if ($answer -notmatch '^(y|yes)$') {
            Write-Host 'Nothing was installed.'
            return 1
        }
    }

    Write-Title 'Installing'
    Assert-NotRunning -Dir $Dir

    # A replace rather than a merge. Leftovers from an older version are how a
    # Qt plugin from one release ends up loaded by the next one.
    if (Test-Path -LiteralPath $Dir) {
        Remove-Item -LiteralPath $Dir -Recurse -Force
        Write-Step 'Removed the previous install'
    }
    New-Item -ItemType Directory -Path $Dir -Force | Out-Null
    Copy-Item -Path (Join-Path $Source '*') -Destination $Dir -Recurse -Force
    Write-Step "Copied the application"

    $installedExe = Join-Path $Dir $ExeName

    if (-not $NoShortcut) {
        $start = Join-Path ([Environment]::GetFolderPath('Programs')) "$ProductName.lnk"
        New-Shortcut -LinkPath $start -TargetPath $installedExe -WorkingDirectory $Dir
        Write-Step "Start menu shortcut"
    }
    if ($DesktopShortcut) {
        $desktop = Join-Path ([Environment]::GetFolderPath('Desktop')) "$ProductName.lnk"
        New-Shortcut -LinkPath $desktop -TargetPath $installedExe -WorkingDirectory $Dir
        Write-Step "Desktop shortcut"
    }

    $sizeKb = [int](((Get-ChildItem -LiteralPath $Dir -Recurse -File |
        Measure-Object -Property Length -Sum).Sum) / 1024)
    $uninstallScript = Join-Path $Dir 'install.ps1'
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $uninstallCommand =
        "`"$powershell`" -NoProfile -ExecutionPolicy Bypass -File `"$uninstallScript`" -Uninstall"

    New-Item -Path $RegistryKey -Force | Out-Null
    $values = @{
        DisplayName          = $ProductName
        DisplayVersion       = $version
        Publisher            = $Publisher
        InstallLocation      = $Dir
        DisplayIcon          = $installedExe
        UninstallString      = $uninstallCommand
        QuietUninstallString = "$uninstallCommand -Silent"
        InstallDate          = (Get-Date -Format 'yyyyMMdd')
    }
    foreach ($name in $values.Keys) {
        New-ItemProperty -Path $RegistryKey -Name $name -Value $values[$name] `
            -PropertyType String -Force | Out-Null
    }
    foreach ($name in @('NoModify', 'NoRepair')) {
        New-ItemProperty -Path $RegistryKey -Name $name -Value 1 `
            -PropertyType DWord -Force | Out-Null
    }
    New-ItemProperty -Path $RegistryKey -Name 'EstimatedSize' -Value $sizeKb `
        -PropertyType DWord -Force | Out-Null
    Write-Step 'Listed in Add or remove programs'

    Write-Host ''
    Write-Host "$ProductName $version is installed."
    Write-Host "  Run it from the Start menu, or from $installedExe"
    Write-Host "  Remove it from Add or remove programs, or with:"
    Write-Host "    powershell -NoProfile -ExecutionPolicy Bypass -File `"$uninstallScript`" -Uninstall"
    return 0
}

# ---------------------------------------------------------------------------

try {
    $source = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
    $target = if ($InstallDir) { $InstallDir } else { Get-DefaultInstallDir }

    if ($Uninstall) {
        exit (Invoke-Uninstall -Dir $(if ($InstallDir) { $InstallDir } else { '' }))
    }
    exit (Invoke-Install -Source $source -Dir $target)
} catch {
    Write-Host ''
    Write-Host ("Failed: " + $_.Exception.Message)
    exit 1
}

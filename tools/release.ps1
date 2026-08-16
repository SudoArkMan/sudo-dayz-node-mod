# Builds the release artifacts from a clean tree, and refuses to if the tests
# do not pass.
#
#   pwsh -File tools/release.ps1
#
# What it does, in order:
#
#   1. configures a build tree of its own, from scratch, so nothing left over
#      from a development build can end up in a package
#   2. builds the application and all the test targets
#   3. runs every test suite and stops here if any of them fails
#   4. checks the changelog has a section for the version being packaged
#   5. packs the ZIP
#   6. unpacks that ZIP into a folder that has never had Qt near it and starts
#      the executable there with Qt taken out of PATH
#
# Step six is the one that matters. A package missing a DLL runs perfectly on
# the machine that built it, because Qt is on PATH there, and fails on the first
# machine it is sent to. The only way to see that before a stranger does is to
# take the build machine's advantages away and try again.
#
# Nothing here publishes anything. It writes files into the build tree and stops.

[CmdletBinding()]
param(
    # Build tree to use. Wiped and reconfigured unless -KeepBuildDir is given.
    [string] $BuildDir = 'build/release',

    # Reuse an existing build tree. Faster while working on the packaging, and
    # not what a real release should do.
    [switch] $KeepBuildDir,

    # A missing changelog section is a warning rather than a refusal. For a
    # dry run before the changelog has been written.
    [switch] $Draft,

    # Skip the unpack and run check. Only useful with no interactive session.
    [switch] $SkipVerify,

    # Also install from the packaged ZIP into a scratch folder, check what it
    # created, uninstall, and check that all of it is gone again.
    [switch] $VerifyInstall,

    [string] $QtPrefix = 'C:/Qt/6.11.0/mingw_64',
    [string] $Compiler = 'C:/Qt/Tools/mingw1310_64/bin/g++.exe',
    [string] $CMakeExe = 'C:/Qt/Tools/CMake_64/bin/cmake.exe',
    [string] $NinjaExe = 'C:/Qt/Tools/Ninja/ninja.exe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ExeName  = 'DAYZSUDONodeMod.exe'

function Write-Head {
    param([string] $Text)
    Write-Host ''
    Write-Host $Text
    Write-Host ('=' * $Text.Length)
}

function Fail {
    param([string] $Text)
    Write-Host ''
    Write-Host "Refusing to package: $Text"
    exit 1
}

function Resolve-Tool {
    param([string] $Preferred, [string] $OnPath)
    if ($Preferred -and (Test-Path -LiteralPath $Preferred)) { return $Preferred }
    $found = Get-Command $OnPath -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    Fail "$OnPath was not found, and neither was $Preferred."
}

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

Write-Head 'Tools'

$CMakeExe = Resolve-Tool -Preferred $CMakeExe -OnPath 'cmake'
$NinjaExe = Resolve-Tool -Preferred $NinjaExe -OnPath 'ninja'
if (-not (Test-Path -LiteralPath $Compiler)) { Fail "The compiler was not found at $Compiler." }
if (-not (Test-Path -LiteralPath $QtPrefix)) { Fail "Qt was not found at $QtPrefix." }
$CPackExe = Join-Path (Split-Path -Parent $CMakeExe) 'cpack.exe'
if (-not (Test-Path -LiteralPath $CPackExe)) { Fail "cpack was not found beside $CMakeExe." }

Write-Host "  cmake    $CMakeExe"
Write-Host "  ninja    $NinjaExe"
Write-Host "  compiler $Compiler"
Write-Host "  qt       $QtPrefix"

# The compiler's own folder has to be reachable for the build itself. The
# verification step later takes it away again on purpose.
$CompilerBin = Split-Path -Parent $Compiler
$QtBin       = Join-Path $QtPrefix 'bin'
$OriginalPath = $env:PATH
$env:PATH = "$CompilerBin;$QtBin;$OriginalPath"

# ---------------------------------------------------------------------------
# Configure and build
# ---------------------------------------------------------------------------

if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $RepoRoot $BuildDir
}

Write-Head 'Build'

if ((Test-Path -LiteralPath $BuildDir) -and -not $KeepBuildDir) {
    Write-Host "  Clearing $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

& $CMakeExe -S $RepoRoot -B $BuildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
    "-DCMAKE_PREFIX_PATH=$QtPrefix" `
    "-DCMAKE_CXX_COMPILER=$Compiler"
if ($LASTEXITCODE -ne 0) { Fail 'configure failed.' }

& $CMakeExe --build $BuildDir
if ($LASTEXITCODE -ne 0) { Fail 'the build failed.' }

# The version the build settled on, read back rather than assumed. CMakeCache
# is where the joined version ends up, suffix and all.
$cacheLine = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') `
    -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$' -ErrorAction SilentlyContinue
$Version = if ($cacheLine) { $cacheLine.Matches[0].Groups[1].Value } else { '' }
$suffixLine = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') `
    -Pattern '^NODEMOD_VERSION_SUFFIX:STRING=(.*)$' -ErrorAction SilentlyContinue
if ($suffixLine) { $Version += $suffixLine.Matches[0].Groups[1].Value }
if (-not $Version) { Fail 'the version could not be read back out of the build tree.' }
Write-Host ''
Write-Host "  Version $Version"

# ---------------------------------------------------------------------------
# Tests. The gate.
# ---------------------------------------------------------------------------

Write-Head 'Tests'

$resources = Join-Path $RepoRoot 'resources'
$suites = @(Get-ChildItem -LiteralPath (Join-Path $BuildDir 'tests') -Filter '*.exe' |
            Sort-Object Name)
if ($suites.Count -eq 0) { Fail 'no test suites were built, so nothing has been proven.' }

$failed = @()
foreach ($suite in $suites) {
    $name = $suite.BaseName
    $output = & $suite.FullName $resources 2>&1
    if ($LASTEXITCODE -ne 0) {
        $failed += $name
        Write-Host ("  {0,-20} failed" -f $name)
        $output | Select-Object -Last 15 | ForEach-Object { Write-Host "      $_" }
    } else {
        $last = ($output | Select-Object -Last 1)
        Write-Host ("  {0,-20} ok    {1}" -f $name, $last)
    }
}

Write-Host ''
Write-Host "  $($suites.Count) suites, $($failed.Count) failed"
if ($failed.Count -gt 0) {
    Fail ("these suites did not pass: " + ($failed -join ', '))
}

# ---------------------------------------------------------------------------
# Changelog
# ---------------------------------------------------------------------------

Write-Head 'Changelog'

$changelog = Join-Path $RepoRoot 'CHANGELOG.md'
if (-not (Test-Path -LiteralPath $changelog)) {
    if ($Draft) {
        Write-Host '  CHANGELOG.md is not in the tree yet. Packaging anyway because -Draft was given.'
    } else {
        Fail ('CHANGELOG.md is not in the tree. The What is new panel reads it from ' +
              'beside the executable, so a package without one ships a blank panel. ' +
              'Add it, or pass -Draft.')
    }
} else {
    # The heading the version is written under, in either of the two spellings a
    # Keep a Changelog file uses: [0.2.0] or [v0.2.0].
    $pattern = '^\s*##\s*\[v?' + [regex]::Escape($Version) + '\]'
    if (Select-String -LiteralPath $changelog -Pattern $pattern -Quiet) {
        Write-Host "  CHANGELOG.md has a section for $Version"
    } elseif ($Draft) {
        Write-Host "  CHANGELOG.md has no section for $Version. Packaging anyway because -Draft was given."
    } else {
        Fail ("CHANGELOG.md has no '## [$Version]' section. The update panel would " +
              'show release notes for a version that is not this one.')
    }
}

# ---------------------------------------------------------------------------
# Pack
# ---------------------------------------------------------------------------

Write-Head 'Pack'

& $CPackExe -G ZIP --config (Join-Path $BuildDir 'CPackConfig.cmake')
if ($LASTEXITCODE -ne 0) { Fail 'cpack failed.' }

$artifactDir = Join-Path $BuildDir 'artifacts'
$zip = Get-ChildItem -LiteralPath $artifactDir -Filter '*.zip' |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $zip) { Fail "cpack wrote no ZIP into $artifactDir." }
Write-Host ("  {0}  ({1:N1} MB)" -f $zip.FullName, ($zip.Length / 1MB))

# ---------------------------------------------------------------------------
# Verify: unpack somewhere clean and run with Qt taken away
# ---------------------------------------------------------------------------

if (-not $SkipVerify) {
    Write-Head 'Verify'

    $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("nodemod-verify-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    try {
        Expand-Archive -LiteralPath $zip.FullName -DestinationPath $scratch -Force
        $unpacked = Get-ChildItem -LiteralPath $scratch -Directory | Select-Object -First 1
        if (-not $unpacked) { Fail 'the ZIP unpacked to no folder.' }
        $exe = Join-Path $unpacked.FullName $ExeName
        if (-not (Test-Path -LiteralPath $exe)) { Fail "$ExeName is not in the package." }

        # Named rather than counted, because a missing platform plugin is the
        # one that fails with no message at all.
        $required = @(
            'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
            'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
            'platforms/qwindows.dll',
            'resources/catalog.json', 'resources/mod-template',
            'licenses/Qt-LICENSE.txt'
        )
        $missing = @($required | Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $unpacked.FullName $_)) })
        if ($missing.Count -gt 0) {
            Fail ('the package is missing: ' + ($missing -join ', '))
        }
        Write-Host "  Everything expected is in the folder"

        # PATH cut back to what a fresh Windows install has. This is the whole
        # point of the step: the executable has to find Qt beside itself,
        # because on the machine it is going to there is nothing else.
        $strippedPath = @(
            (Join-Path $env:SystemRoot 'System32'),
            $env:SystemRoot,
            (Join-Path $env:SystemRoot 'System32\Wbem'),
            (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0')
        ) -join ';'

        # --screenshot builds the window, paints it once and exits by itself.
        # Nothing is typed and nothing is clicked.
        #
        # The environment is set on the child rather than on this shell, so the
        # stripping is exact and this script's own PATH is left alone. Started
        # through Diagnostics.Process because the exit code has to be readable
        # afterwards, and that is what separates "it ran" from "it started and
        # fell over".
        $shot = Join-Path $scratch 'window.png'
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName         = $exe
        $psi.WorkingDirectory = $unpacked.FullName
        $psi.UseShellExecute  = $false
        [void]$psi.ArgumentList.Add('--screenshot')
        [void]$psi.ArgumentList.Add($shot)
        $psi.Environment['PATH'] = $strippedPath
        [void]$psi.Environment.Remove('QT_PLUGIN_PATH')
        [void]$psi.Environment.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')
        [void]$psi.Environment.Remove('QT_QPA_PLATFORM')

        $proc = [System.Diagnostics.Process]::Start($psi)
        if (-not $proc.WaitForExit(120000)) {
            # Only ever the process this script started, held by handle. Another
            # copy of the editor is a different process and is never touched.
            $id = $proc.Id
            $proc.Kill()
            Fail "the packaged executable did not exit within two minutes (process $id)."
        }
        if ($proc.ExitCode -ne 0) {
            Fail "the packaged executable exited with $($proc.ExitCode) when Qt was off PATH."
        }

        if (-not (Test-Path -LiteralPath $shot)) {
            Fail 'the packaged executable ran but painted no window.'
        }
        $shotSize = (Get-Item -LiteralPath $shot).Length
        if ($shotSize -lt 10000) {
            Fail "the window it painted was $shotSize bytes, which is not a window."
        }
        Write-Host ("  Started with Qt off PATH and painted a window ({0:N0} bytes)" -f $shotSize)

        if ($VerifyInstall) {
            & (Join-Path $PSScriptRoot 'verify-install.ps1') -PackageDir $unpacked.FullName
            if ($LASTEXITCODE -ne 0) { Fail 'the install and uninstall round trip failed.' }
        }
    } finally {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$env:PATH = $OriginalPath

Write-Head 'Done'
Write-Host "  $($zip.FullName)"
Write-Host ''
Write-Host "  Nothing has been published. The next steps are by hand: tag v$Version"
Write-Host '  and attach that ZIP to the release.'
exit 0

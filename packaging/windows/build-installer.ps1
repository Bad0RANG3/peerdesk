#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$DistDir = "",
    [string]$QtBin = "",
    [string]$MinGwBin = "",
    [string]$IsccPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "build\cpp-release"
}
if (-not $DistDir) {
    $DistDir = Join-Path $RepoRoot "dist"
}
if (-not [IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $RepoRoot $BuildDir
}
if (-not [IO.Path]::IsPathRooted($DistDir)) {
    $DistDir = Join-Path $RepoRoot $DistDir
}

function Get-WorkspacePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $RepoRoot.TrimEnd("\") + "\"
    if (-not $full.StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must stay inside the repository: $full"
    }
    return $full
}

function Remove-SafeDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $full = Get-WorkspacePath -Path $Path -Label $Label
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

function Require-File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -eq $cmakeCommand) {
    throw "cmake.exe was not found on PATH."
}

$BuildDir = Get-WorkspacePath -Path $BuildDir -Label "BuildDir"
$DistDir = Get-WorkspacePath -Path $DistDir -Label "DistDir"
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) {
    throw "Release build directory not found: $BuildDir"
}

$cmakeText = Get-Content -LiteralPath (
    Join-Path $RepoRoot "CMakeLists.txt") -Raw
$versionMatch = [regex]::Match(
    $cmakeText,
    "project\s*\(\s*PeerDesk\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
    [Text.RegularExpressions.RegexOptions]::Singleline)
if (-not $versionMatch.Success) {
    throw "Unable to read the PeerDesk version from CMakeLists.txt."
}
$Version = $versionMatch.Groups[1].Value

if (-not $QtBin) {
    if ($env:QT_ROOT_DIR) {
        $candidate = Join-Path $env:QT_ROOT_DIR "bin"
        if (Test-Path -LiteralPath (
                Join-Path $candidate "windeployqt.exe")) {
            $QtBin = $candidate
        }
    }
    if (-not $QtBin) {
        $windeployCommand =
            Get-Command windeployqt.exe -ErrorAction SilentlyContinue
        if ($null -ne $windeployCommand) {
            $QtBin = Split-Path -Parent $windeployCommand.Source
        }
    }
}
if (-not $QtBin) {
    throw "windeployqt.exe was not found. Set QtBin or QT_ROOT_DIR."
}
$WindeployQt = Join-Path $QtBin "windeployqt.exe"
Require-File -Path $WindeployQt -Label "windeployqt.exe"

if (-not $MinGwBin) {
    if ($env:IQTA_TOOLS) {
        $candidate = Join-Path $env:IQTA_TOOLS "mingw1310_64\bin"
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            $MinGwBin = $candidate
        }
    }
    if (-not $MinGwBin) {
        $compilerCommand = Get-Command g++.exe -ErrorAction SilentlyContinue
        if ($null -ne $compilerCommand) {
            $MinGwBin = Split-Path -Parent $compilerCommand.Source
        }
    }
}

if (-not $IsccPath) {
    $candidates = @()
    if ($env:LOCALAPPDATA) {
        $candidates += Join-Path $env:LOCALAPPDATA (
            "Programs\Inno Setup 6\ISCC.exe")
    }
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles (
            "Inno Setup 6\ISCC.exe")
    }
    if (${env:ProgramFiles(x86)}) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} (
            "Inno Setup 6\ISCC.exe")
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $IsccPath = $candidate
            break
        }
    }
    if (-not $IsccPath) {
        $isccCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($null -ne $isccCommand) {
            $IsccPath = $isccCommand.Source
        }
    }
}
if (-not $IsccPath) {
    throw "Inno Setup 6 compiler (ISCC.exe) was not found."
}
Require-File -Path $IsccPath -Label "ISCC.exe"

$StageDir = Join-Path $DistDir "stage"
$InstallerName = "PeerDesk-$Version-setup.exe"
$InstallerPath = Join-Path $DistDir $InstallerName
$HashPath = Join-Path $DistDir "SHA256SUMS.txt"
$IssSource = Join-Path $PSScriptRoot "PeerDesk.iss"
$IssBuild = Join-Path $DistDir "PeerDesk.generated.iss"

Require-File -Path $IssSource -Label "Installer script"
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
Remove-SafeDirectory -Path $StageDir -Label "StageDir"
foreach ($path in @($InstallerPath, $HashPath, $IssBuild)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

Write-Host "Installing release payload into $StageDir"
& $cmakeCommand.Source --install $BuildDir --prefix $StageDir
if ($LASTEXITCODE -ne 0) {
    throw "cmake --install failed with exit code $LASTEXITCODE."
}

$ClientExe = Join-Path $StageDir "peerdesk-client.exe"
$ServerExe = Join-Path $StageDir "peerdesk-server.exe"
Require-File -Path $ClientExe -Label "Client executable"
Require-File -Path $ServerExe -Label "Server executable"

Write-Host "Deploying Qt runtime with $WindeployQt"
foreach ($executable in @($ClientExe, $ServerExe)) {
    $deployArguments = @(
        "--release",
        "--compiler-runtime",
        "--no-translations",
        "--dir", $StageDir
    )
    if ($executable -eq $ClientExe) {
        $deployArguments += @("--include-plugins", "qoffscreen")
    }
    $deployArguments += $executable
    & $WindeployQt @deployArguments
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed for $executable with exit code $LASTEXITCODE."
    }
}

$CompilerRuntime = @(
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll"
)
foreach ($runtime in $CompilerRuntime) {
    $target = Join-Path $StageDir $runtime
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        continue
    }
    if (-not $MinGwBin) {
        throw "Missing $runtime and MinGW bin was not found."
    }
    $source = Join-Path $MinGwBin $runtime
    Require-File -Path $source -Label "MinGW runtime $runtime"
    Copy-Item -LiteralPath $source -Destination $target
}

$RequiredFiles = @(
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6Network.dll",
    "Qt6Multimedia.dll",
    "platforms\qwindows.dll",
    "platforms\qoffscreen.dll",
    "LICENSE",
    "README.md",
    "USER-GUIDE.txt"
)
foreach ($relative in $RequiredFiles) {
    Require-File -Path (Join-Path $StageDir $relative) `
        -Label "Installed payload file $relative"
}

$imagePlugins = @(
    Get-ChildItem -LiteralPath (
        Join-Path $StageDir "imageformats") `
        -Filter "*.dll" -File -ErrorAction SilentlyContinue)
if ($imagePlugins.Count -eq 0) {
    throw "No Qt image format plugins were deployed."
}

$multimediaPlugins = @(
    Get-ChildItem -LiteralPath (
        Join-Path $StageDir "multimedia") `
        -Filter "*.dll" -File -ErrorAction SilentlyContinue)
if ($multimediaPlugins.Count -eq 0) {
    throw "No Qt multimedia plugins were deployed."
}

$unexpected = @(
    Get-ChildItem -LiteralPath $StageDir -Recurse -File |
        Where-Object {
            $_.Name -match "(?i)(tests|link-harness)\.exe$" -or
            $_.Extension -in @(".pdb", ".ilk")
        })
if ($unexpected.Count -ne 0) {
    throw "Unexpected development files in installer payload: $(
        ($unexpected | ForEach-Object FullName) -join ', ')"
}

$issText = [IO.File]::ReadAllText($IssSource, [Text.Encoding]::UTF8)
$utf8WithBom = New-Object System.Text.UTF8Encoding($true)
[IO.File]::WriteAllText($IssBuild, $issText, $utf8WithBom)

Write-Host "Compiling $InstallerName"
& $IsccPath `
    "/DAppVersion=$Version" `
    "/DPayloadDir=$StageDir" `
    "/DArtifactDir=$DistDir" `
    $IssBuild
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed with exit code $LASTEXITCODE."
}
Require-File -Path $InstallerPath -Label "Installer artifact"

$hash = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash
$hashLine = "$hash  $InstallerName$([Environment]::NewLine)"
[IO.File]::WriteAllText(
    $HashPath,
    $hashLine,
    (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Installer: $InstallerPath"
Write-Host "SHA-256: $HashPath"

#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InstallerPath,
    [string]$WorkDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if (-not $WorkDirectory) {
    $WorkDirectory = Join-Path $RepoRoot "dist\installer-test"
}
if (-not [IO.Path]::IsPathRooted($WorkDirectory)) {
    $WorkDirectory = Join-Path $RepoRoot $WorkDirectory
}
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)

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

function Invoke-CheckedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$Label,
        [int]$TimeoutSeconds = 180
    )

    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $ArgumentList `
        -WindowStyle Hidden `
        -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try {
            $process.Kill()
        } catch {
        }
        throw "$Label timed out after $TimeoutSeconds seconds."
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0) {
        throw "$Label failed with exit code $($process.ExitCode)."
    }
}

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Condition,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$TimeoutSeconds = 15
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description."
}

Require-File -Path $InstallerPath -Label "Installer"
$WorkDirectory = Get-WorkspacePath `
    -Path $WorkDirectory -Label "WorkDirectory"

$AppId = "{DFA1EBD0-B348-4B32-A0A7-7F4E5CDEAE93}"
$UninstallRegistryKey = "HKCU:\Software\Microsoft\Windows\" +
    "CurrentVersion\Uninstall\$AppId`_is1"
if (Test-Path -LiteralPath $UninstallRegistryKey) {
    throw "PeerDesk is already installed for this user. " +
        "Uninstall it before running the installer smoke test."
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

Remove-SafeDirectory -Path $WorkDirectory -Label "WorkDirectory"
New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
$InstallDir = Join-Path $WorkDirectory "app"
$InstallLog = Join-Path $WorkDirectory "install.log"
$UninstallLog = Join-Path $WorkDirectory "uninstall.log"
$ScreenshotPath = Join-Path $WorkDirectory "client-smoke.png"

$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$RunValueName = "PeerDesk"
$hadRunValue = $false
$savedRunValue = ""
$savedRunKind = "String"
if (-not (Test-Path -LiteralPath $RunKey)) {
    New-Item -Path $RunKey -Force | Out-Null
}
$runItem = Get-Item -LiteralPath $RunKey
if ($runItem.GetValueNames() -contains $RunValueName) {
    $hadRunValue = $true
    $savedRunValue = $runItem.GetValue(
        $RunValueName, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $savedRunKind = $runItem.GetValueKind(
        $RunValueName).ToString()
}

$AppDataDirectory = Join-Path $env:APPDATA "PeerDesk"
$MarkerPath = Join-Path $AppDataDirectory (
    "installer-smoke-$PID.marker")
$ProgramsDirectory = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::Programs)
$ShortcutPath = Join-Path $ProgramsDirectory "PeerDesk\PeerDesk.lnk"

try {
    Write-Host "Installing silently into $InstallDir"
    Invoke-CheckedProcess `
        -FilePath $InstallerPath `
        -ArgumentList @(
            "/VERYSILENT",
            "/SUPPRESSMSGBOXES",
            "/NORESTART",
            "/SP-",
            "/LANG=chinesesimplified",
            "/DIR=$InstallDir",
            "/LOG=$InstallLog",
            "/TASKS="
        ) `
        -Label "PeerDesk installer" `
        -TimeoutSeconds 300

    $clientPath = Join-Path $InstallDir "peerdesk-client.exe"
    $serverPath = Join-Path $InstallDir "peerdesk-server.exe"
    $uninstallerPath = Join-Path $InstallDir "unins000.exe"
    Require-File -Path $clientPath -Label "Installed client"
    Require-File -Path $serverPath -Label "Installed server"
    Require-File -Path $uninstallerPath -Label "Uninstaller"
    Require-File `
        -Path (Join-Path $InstallDir "platforms\qwindows.dll") `
        -Label "Qt platform plugin"
    Require-File `
        -Path (Join-Path $InstallDir "platforms\qoffscreen.dll") `
        -Label "Qt offscreen platform plugin"
    Require-File `
        -Path (Join-Path $InstallDir "USER-GUIDE.txt") `
        -Label "User guide"
    if (-not (Test-Path -LiteralPath $ShortcutPath -PathType Leaf)) {
        throw "Start menu shortcut was not created: $ShortcutPath"
    }

    $versionOutput = (& $serverPath --version 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Installed server --version failed with exit code $LASTEXITCODE."
    }
    if ($versionOutput -notmatch [regex]::Escape($Version)) {
        throw "Installed server reported an unexpected version: $versionOutput"
    }

    $previousPlatform = $env:QT_QPA_PLATFORM
    try {
        $env:QT_QPA_PLATFORM = "offscreen"
        Invoke-CheckedProcess `
            -FilePath $clientPath `
            -ArgumentList @(
                "--page", "remote",
                "--capture", "test",
                "--audio-test",
                "--screenshot", $ScreenshotPath,
                "--exit-after", "10000"
            ) `
            -Label "Installed client screenshot smoke" `
            -TimeoutSeconds 30
    } finally {
        if ($null -eq $previousPlatform) {
            Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
        } else {
            $env:QT_QPA_PLATFORM = $previousPlatform
        }
    }
    Require-File -Path $ScreenshotPath -Label "Client smoke screenshot"
    $screenshot = Get-Item -LiteralPath $ScreenshotPath
    if ($screenshot.Length -lt 1024) {
        throw "Client smoke screenshot is unexpectedly small."
    }
    $signature = [IO.File]::ReadAllBytes($ScreenshotPath)
    $pngSignature = [byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
    for ($index = 0; $index -lt $pngSignature.Length; $index++) {
        if ($signature[$index] -ne $pngSignature[$index]) {
            throw "Client smoke screenshot is not a PNG file."
        }
    }

    New-Item -ItemType Directory -Path $AppDataDirectory -Force |
        Out-Null
    [IO.File]::WriteAllText(
        $MarkerPath,
        "installer smoke marker$([Environment]::NewLine)",
        (New-Object System.Text.UTF8Encoding($false)))

    Set-ItemProperty `
        -LiteralPath $RunKey `
        -Name $RunValueName `
        -Value "`"$clientPath`" --background" `
        -Type String

    Write-Host "Uninstalling silently"
    Invoke-CheckedProcess `
        -FilePath $uninstallerPath `
        -ArgumentList @(
            "/VERYSILENT",
            "/SUPPRESSMSGBOXES",
            "/NORESTART",
            "/LOG=$UninstallLog"
        ) `
        -Label "PeerDesk uninstaller" `
        -TimeoutSeconds 180

    Wait-Until `
        -Description "program removal" `
        -Condition {
            -not (Test-Path -LiteralPath $clientPath) -and
            -not (Test-Path -LiteralPath $serverPath)
        }
    Wait-Until `
        -Description "start menu shortcut removal" `
        -Condition {
            -not (Test-Path -LiteralPath $ShortcutPath)
        }
    if (Test-Path -LiteralPath $UninstallRegistryKey) {
        throw "Uninstall registry entry was not removed."
    }
    $runItem = Get-Item -LiteralPath $RunKey
    if ($runItem.GetValueNames() -contains $RunValueName) {
        throw "Login startup value was not removed by the uninstaller."
    }
    if (-not (Test-Path -LiteralPath $MarkerPath -PathType Leaf)) {
        throw "Uninstaller removed user configuration data."
    }

    Write-Host "Installer smoke test passed."
    Write-Host "Install log: $InstallLog"
    Write-Host "Uninstall log: $UninstallLog"
} finally {
    if ($hadRunValue) {
        $runType = [Microsoft.Win32.RegistryValueKind]$savedRunKind
        Set-ItemProperty `
            -LiteralPath $RunKey `
            -Name $RunValueName `
            -Value $savedRunValue `
            -Type $runType
    } else {
        Remove-ItemProperty `
            -LiteralPath $RunKey `
            -Name $RunValueName `
            -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $MarkerPath) {
        Remove-Item -LiteralPath $MarkerPath -Force
    }
}

[CmdletBinding()]
param(
    [string] $WslDistro = "openSUSE-Tumbleweed",
    [string] $QtPrefix = "",
    [string] $SdkPrefix = "",
    [string] $StemTeXRoot = "",
    [string] $StemTeXStageRoot = "",
    [string] $WindowsBuildRoot = "",
    [string] $WindowsVersion = "",
    [string] $WindowsFileVersion = "",
    [string] $AppImageVersion = "",
    [string] $PackageOutputRoot = "",
    [string] $AppImageTool = "",
    [string] $AppImageRuntimeFile = "",
    [int] $WindowsJobs = 8,
    [switch] $DownloadAppImageTool,
    [switch] $DownloadAppImageRuntime,
    [switch] $SkipWindows,
    [switch] $SkipLinux,
    [switch] $SkipWindowsBuild,
    [switch] $SkipLinuxBuild,
    [switch] $SkipWindowsPackage,
    [switch] $SkipAppImagePackage
)

$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$workspaceRoot = Split-Path -Parent $repoRoot

if (!$WindowsBuildRoot) {
    $WindowsBuildRoot = Join-Path $workspaceRoot "windows_build"
}
$WindowsBuildRoot = [System.IO.Path]::GetFullPath($WindowsBuildRoot)

if (!$AppImageVersion) {
    $AppImageVersion = Get-Date -Format "yyyyMMdd"
}
if (!$PackageOutputRoot) {
    $PackageOutputRoot = Join-Path $workspaceRoot "packages"
}
$PackageOutputRoot = [System.IO.Path]::GetFullPath($PackageOutputRoot)

$windowsDist = Join-Path $WindowsBuildRoot "dist"
$linuxAppImageDir = Join-Path $workspaceRoot "linux_build\appimage"
$results = [ordered]@{}
$copiedResults = [ordered]@{}

function Invoke-Step([string] $Name, [scriptblock] $Body) {
    Write-Host ""
    Write-Host "==> $Name" -ForegroundColor Cyan
    & $Body
    Write-Host "<== $Name" -ForegroundColor Green
}

function Invoke-External([string] $FilePath, [string[]] $Arguments) {
    Write-Host ("+ {0} {1}" -f $FilePath, ($Arguments -join " "))
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function ConvertTo-WslPath([string] $WindowsPath) {
    $fullPath = [System.IO.Path]::GetFullPath($WindowsPath)
    $drive = $fullPath.Substring(0, 1).ToLowerInvariant()
    $rest = $fullPath.Substring(2) -replace "\\", "/"
    return "/mnt/$drive$rest"
}

function Quote-Bash([string] $Value) {
    return "'" + ($Value -replace "'", "'\''") + "'"
}

function Get-NewestFile([string] $Directory, [string] $Filter) {
    if (!(Test-Path -LiteralPath $Directory)) {
        return $null
    }
    Get-ChildItem -LiteralPath $Directory -Filter $Filter -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Resolve-CMakeCommand {
    $command = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidate = "C:\Qt\Tools\CMake_64\bin\cmake.exe"
    if (Test-Path -LiteralPath $candidate) {
        return $candidate
    }

    throw "Cannot find cmake.exe. Install CMake or add it to PATH."
}

Write-Host "Mengshee local package build"
Write-Host "  Repo:               $repoRoot"
Write-Host "  Windows build root: $WindowsBuildRoot"
Write-Host "  StemTeX source:     $StemTeXRoot"
Write-Host "  StemTeX stage:      $StemTeXStageRoot"
Write-Host "  Linux AppImage dir: $linuxAppImageDir"
Write-Host "  Package output:     $PackageOutputRoot"
Write-Host "  WSL distro:         $WslDistro"
Write-Host "  AppImage version:   $AppImageVersion"

if (!$SkipWindows) {
    $cmake = Resolve-CMakeCommand
    $windowsDriver = Join-Path $repoRoot "windows-build\cmake\build-windows.cmake"
    $packageDriver = Join-Path $repoRoot "windows-build\cmake\package-windows.cmake"
    $windowsPackageVersion = $WindowsVersion
    if (!$windowsPackageVersion) {
        $windowsPackageVersion = (Get-Content -LiteralPath (Join-Path $repoRoot "VERSION.txt") -Raw).Trim()
    }
    if ($windowsPackageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "Windows version must contain four numeric components, got '$windowsPackageVersion'"
    }
    $windowsOutputDir = Join-Path $windowsDist "mengshee-$windowsPackageVersion"
    $commonWindowsArgs = @(
        "-DWORKSPACE_ROOT=$WindowsBuildRoot",
        "-DJOBS=$WindowsJobs"
    )
    if ($QtPrefix) {
        $commonWindowsArgs += "-DQT_PREFIX=$QtPrefix"
    }
    if ($SdkPrefix) {
        $commonWindowsArgs += "-DSDK_PREFIX=$SdkPrefix"
    }
    if ($StemTeXRoot) {
        $commonWindowsArgs += "-DSTEMTEX_ROOT=$([System.IO.Path]::GetFullPath($StemTeXRoot))"
    }
    if ($StemTeXStageRoot) {
        $commonWindowsArgs += "-DSTEMTEX_STAGE_ROOT=$([System.IO.Path]::GetFullPath($StemTeXStageRoot))"
    }

    if (!$SkipWindowsBuild) {
        Invoke-Step "Windows canonical build" {
            $args = @($commonWindowsArgs)
            if ($SkipWindowsPackage) {
                $args += "-DSKIP_PACKAGE=ON"
            } else {
                if ($WindowsVersion) {
                    $args += "-DVERSION=$WindowsVersion"
                }
                if ($WindowsFileVersion) {
                    $args += "-DFILE_VERSION=$WindowsFileVersion"
                }
            }
            $args += @("-P", $windowsDriver)
            Invoke-External $cmake $args
        }
    } elseif (!$SkipWindowsPackage) {
        Invoke-Step "Windows package from deployed runtime" {
            $args = @(
                "-DWORKSPACE_ROOT=$WindowsBuildRoot"
            )
            if ($WindowsVersion) {
                $args += "-DVERSION=$WindowsVersion"
            }
            if ($WindowsFileVersion) {
                $args += "-DFILE_VERSION=$WindowsFileVersion"
            }
            $args += @("-P", $packageDriver)
            Invoke-External $cmake $args
        }
    }

    if (!$SkipWindowsPackage) {
        $installerPath = Join-Path $windowsOutputDir "Mengshee-$windowsPackageVersion-Setup.exe"
        if (!(Test-Path -LiteralPath $installerPath -PathType Leaf)) {
            throw "Windows installer was not found at $installerPath"
        }
        $results["Windows installer"] = [System.IO.Path]::GetFullPath($installerPath)
    }
}

if (!$SkipLinux) {
    if (!$SkipAppImagePackage) {
        $repoWsl = ConvertTo-WslPath $repoRoot

        $linuxLines = @(
            "set -eu",
            "cd $(Quote-Bash $repoWsl)"
        )
        if (!$SkipLinuxBuild) {
            $linuxLines += "linux-build/scripts/build-okular-local.sh"
        }

        $appImageEnv = @("VERSION=$(Quote-Bash $AppImageVersion)")
        if ($AppImageTool) {
            $appImageEnv += "APPIMAGETOOL=$(Quote-Bash (ConvertTo-WslPath $AppImageTool))"
        }
        if ($AppImageRuntimeFile) {
            $appImageEnv += "APPIMAGE_RUNTIME_FILE=$(Quote-Bash (ConvertTo-WslPath $AppImageRuntimeFile))"
        }
        if ($DownloadAppImageTool) {
            $appImageEnv += "DOWNLOAD_APPIMAGETOOL=1"
        }
        if ($DownloadAppImageRuntime) {
            $appImageEnv += "DOWNLOAD_APPIMAGE_RUNTIME=1"
        }
        $linuxLines += (($appImageEnv + @("linux-build/scripts/build-okular-appimage.sh")) -join " ")

        Invoke-Step "Linux build and AppImage" {
            Invoke-External "wsl.exe" @("-d", $WslDistro, "--", "bash", "-lc", ($linuxLines -join " && "))
        }

        $appImage = Get-NewestFile $linuxAppImageDir "Okular-dev-$AppImageVersion-*.AppImage"
        if (!$appImage) {
            throw "AppImage was not found under $linuxAppImageDir"
        }
        $results["Linux AppImage"] = $appImage.FullName
    } elseif (!$SkipLinuxBuild) {
        Invoke-Step "Linux full build" {
            $repoWsl = ConvertTo-WslPath $repoRoot
            $linuxLines = @(
                "set -eu",
                "cd $(Quote-Bash $repoWsl)",
                "linux-build/scripts/build-okular-local.sh"
            )
            Invoke-External "wsl.exe" @("-d", $WslDistro, "--", "bash", "-lc", ($linuxLines -join " && "))
        }
    }
}

if ($results.Count -gt 0) {
    Invoke-Step "Collect package outputs" {
        New-Item -ItemType Directory -Force -Path $PackageOutputRoot | Out-Null
        foreach ($entry in $results.GetEnumerator()) {
            $source = $entry.Value
            $destination = Join-Path $PackageOutputRoot (Split-Path -Leaf $source)
            Copy-Item -LiteralPath $source -Destination $destination -Force
            $copiedResults[$entry.Key] = $destination
            Write-Host ("Copied {0}: {1}" -f $entry.Key, $destination)
        }
    }
}

Write-Host ""
Write-Host "Built package outputs:" -ForegroundColor Green
foreach ($entry in $results.GetEnumerator()) {
    Write-Host ("  {0}: {1}" -f $entry.Key, $entry.Value)
}

if ($copiedResults.Count -gt 0) {
    Write-Host ""
    Write-Host "Collected package outputs:" -ForegroundColor Green
    foreach ($entry in $copiedResults.GetEnumerator()) {
        Write-Host ("  {0}: {1}" -f $entry.Key, $entry.Value)
    }
}

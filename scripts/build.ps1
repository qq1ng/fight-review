<#
.SYNOPSIS
  Builds FightReview.dll with MSVC + Ninja and optionally copies it into the GW2 addons folder.
.EXAMPLE
  .\scripts\build.ps1 -Deploy
#>
param(
    [ValidateSet("release", "debug")] [string] $Preset = "release",
    [switch] $Deploy,
    [string] $AddonsDir = "I:\Guild Wars 2\addons"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ desktop workload found." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

# Run configure + build inside the MSVC developer environment. vcvars looks for vswhere on PATH.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$ErrorActionPreference = "Continue" # PowerShell 5.1 turns native stderr output into errors
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && cmake --preset $Preset && cmake --build --preset $Preset 2>&1"
$exit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($exit -ne 0) { throw "Build failed (exit code $exit)." }

$dll = Join-Path $root "build\$Preset\FightReview.dll"
Write-Host "Built $dll"

if ($Deploy) {
    $target = Join-Path $AddonsDir "FightReview.dll"
    if (Test-Path $target) {
        # A loaded DLL can't be overwritten but can be renamed; Nexus picks up the new file.
        $old = "$target.old"
        Remove-Item $old -ErrorAction SilentlyContinue
        try { Copy-Item $dll $target -Force } catch { Rename-Item $target $old; Copy-Item $dll $target }
    } else {
        Copy-Item $dll $target
    }
    Write-Host "Deployed to $target"
}

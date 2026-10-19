param(
    [Parameter(Mandatory = $true)]
    [string]$IrDirectory,
    [string]$Output = "artifacts/llapi-real-shim",
    [string]$LlvmRoot = "$env:LOCALAPPDATA\Temp\hosihikari-llvm\clang+llvm-23.1.3-x86_64-pc-windows-msvc",
    [string]$LeviLaminaRoot = "C:\Users\root\source\repos\hosihikari\LeviLamina",
    [switch]$LinkCarrier,
    [string]$CarrierImportLibrary = "",
    [string[]]$AdditionalLinkLibrary = @(),
    [switch]$SkipRun
)

$ErrorActionPreference = "Stop"

$GeneratorRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$IrDirectory = (Resolve-Path $IrDirectory).Path
$Output = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $GeneratorRoot $Output }
$Manifest = Join-Path $IrDirectory "ir-manifest.json"
if (-not (Test-Path $Manifest)) {
    throw "ir-manifest.json was not found under $IrDirectory."
}

$EmitArgs = @("--native-root", $LeviLaminaRoot)
if ($LinkCarrier) {
    $EmitArgs += "--link-carrier"
}

$GeneratorProject = Join-Path $GeneratorRoot "src\Hosihikari.Bindings.Generator\Hosihikari.Bindings.Generator.csproj"
dotnet run --project $GeneratorProject -c Release -- emit $Manifest $IrDirectory $Output @EmitArgs

$NativeDirectory = Join-Path $Output "native"
New-Item -ItemType Directory -Force -Path $NativeDirectory | Out-Null
$Dll = Join-Path $NativeDirectory "Hosihikari.Bindings.LeviLamina.Shim.dll"

$ShimSource = Get-ChildItem (Join-Path $Output "shim") -Recurse -Filter binding_table.cpp |
    Where-Object { $_.FullName -match 'levilamina' } |
    Select-Object -First 1
if (-not $ShimSource) {
    throw "No levilamina binding_table.cpp was emitted under $Output."
}

$LinkLibraries = @($AdditionalLinkLibrary)
$FmtLib = Get-ChildItem "$env:LOCALAPPDATA\.xmake\packages\f\fmt" -Recurse -Filter fmt.lib -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($FmtLib) {
    $LinkLibraries += $FmtLib.FullName
}

$BuildArgs = @{
    ShimDirectory = $ShimSource.Directory.FullName
    OutputDll = $Dll
    LlvmRoot = $LlvmRoot
    LeviLaminaRoot = $LeviLaminaRoot
}
if ($LinkCarrier) {
    if ([string]::IsNullOrWhiteSpace($CarrierImportLibrary)) {
        $CarrierImportLibrary = Join-Path $LeviLaminaRoot "build\windows\x64\release\LeviLamina.lib"
    }

    if (-not (Test-Path $CarrierImportLibrary)) {
        throw "The carrier import library was not found at $CarrierImportLibrary."
    }

    $LinkLibraries += $CarrierImportLibrary
    $BuildArgs.DelayLoadLibrary = @("LeviLamina.dll")
}

if ($LinkLibraries.Count -gt 0) {
    $BuildArgs.AdditionalLinkLibrary = $LinkLibraries
}

& (Join-Path $PSScriptRoot "build-shim.ps1") @BuildArgs

if ($SkipRun) {
    Write-Output "Built $Dll (smoke run skipped)."
    return
}

$SmokeProject = Join-Path $GeneratorRoot "tests\BindingShimSmoke\Smoke.csproj"
dotnet run --project $SmokeProject -c Release "-p:InteropDirectory=$(Join-Path $Output 'interop')" -- $Dll

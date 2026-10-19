param(
    [Parameter(Mandatory = $true)]
    [string]$IrDirectory,
    [string]$Output = "artifacts/llapi-real-shim",
    [string]$LlvmRoot = "$env:LOCALAPPDATA\Temp\hosihikari-llvm\clang+llvm-23.1.3-x86_64-pc-windows-msvc",
    [string]$LeviLaminaRoot = "C:\Users\root\source\repos\hosihikari\LeviLamina"
)

$ErrorActionPreference = "Stop"

$GeneratorRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$IrDirectory = (Resolve-Path $IrDirectory).Path
$Output = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $GeneratorRoot $Output }
$Manifest = Join-Path $IrDirectory "ir-manifest.json"
if (-not (Test-Path $Manifest)) {
    throw "ir-manifest.json was not found under $IrDirectory."
}

$GeneratorProject = Join-Path $GeneratorRoot "src\Hosihikari.Bindings.Generator\Hosihikari.Bindings.Generator.csproj"
dotnet run --project $GeneratorProject -c Release -- emit $Manifest $IrDirectory $Output --native-root $LeviLaminaRoot

$NativeDirectory = Join-Path $Output "native"
New-Item -ItemType Directory -Force -Path $NativeDirectory | Out-Null
$Dll = Join-Path $NativeDirectory "Hosihikari.Bindings.LeviLamina.Shim.dll"

$ShimSource = Get-ChildItem (Join-Path $Output "shim") -Recurse -Filter binding_table.cpp |
    Where-Object { $_.FullName -match 'levilamina' } |
    Select-Object -First 1
if (-not $ShimSource) {
    throw "No levilamina binding_table.cpp was emitted under $Output."
}

& (Join-Path $PSScriptRoot "build-shim.ps1") `
    -ShimDirectory $ShimSource.Directory.FullName `
    -OutputDll $Dll `
    -LlvmRoot $LlvmRoot `
    -LeviLaminaRoot $LeviLaminaRoot

$SmokeProject = Join-Path $GeneratorRoot "tests\BindingShimSmoke\Smoke.csproj"
dotnet run --project $SmokeProject -c Release "-p:InteropDirectory=$(Join-Path $Output 'interop')" -- $Dll

param(
    [Parameter(Mandatory = $true)]
    [string]$Pdb,
    [Parameter(Mandatory = $true)]
    [string]$Exe,
    [Parameter(Mandatory = $true)]
    [string]$Elf,
    [string]$Output = "artifacts/symbol-ir/prototype.json",
    [string]$Version = "1.21.3",
    [string[]]$Classes = @("Actor", "BlockPos"),
    [string]$LlvmRoot = "$env:LOCALAPPDATA\Temp\hosihikari-llvm\clang+llvm-23.1.3-x86_64-pc-windows-msvc",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Pdb = (Resolve-Path $Pdb).Path
$Exe = (Resolve-Path $Exe).Path
$Elf = (Resolve-Path $Elf).Path
$Output = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $RepoRoot $Output }
New-Item -ItemType Directory -Force -Path (Split-Path $Output -Parent) | Out-Null

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -LlvmRoot $LlvmRoot
}

$Frontend = Join-Path $RepoRoot "build\frontend\Release\hosihikari-symbol-ir-frontend.exe"
if (-not (Test-Path $Frontend)) {
    throw "Symbol frontend was not built: $Frontend"
}

& $Frontend `
    --pdb $Pdb `
    --exe $Exe `
    --elf $Elf `
    --output $Output `
    --version $Version `
    --classes ($Classes -join ",")

if ($LASTEXITCODE -ne 0) {
    throw "symbol frontend exited with $LASTEXITCODE"
}

Write-Output "Wrote $Output"

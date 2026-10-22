param(
    [string]$LeviLaminaRoot = "C:\Users\root\source\repos\hosihikari\LiteLoaderBDS",
    [string]$EnttRoot = "C:\Users\root\AppData\Local\Temp\entt-41aab92\src",
    [string]$Output = "artifacts/levi-1.21.3-header-ir",
    [string]$LlvmRoot = "$env:LOCALAPPDATA\Temp\hosihikari-llvm\clang+llvm-23.1.3-x86_64-pc-windows-msvc",
    [string[]]$AdditionalIncludePath = @(),
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$LeviLaminaRoot = (Resolve-Path $LeviLaminaRoot).Path
$EnttRoot = (Resolve-Path $EnttRoot).Path
$Output = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $RepoRoot $Output }
$SourceRoot = Join-Path $LeviLaminaRoot "src"

$Headers = @(
    "mc/world/actor/Actor.h",
    "mc/world/actor/Mob.h",
    "mc/world/actor/player/Player.h",
    "mc/math/Vec3.h",
    "mc/world/level/BlockPos.h",
    "mc/world/level/ChunkPos.h",
    "mc/world/level/BlockSource.h",
    "mc/world/level/Level.h",
    "mc/world/item/registry/ItemStack.h",
    "mc/world/phys/AABB.h",
    "mc/network/packet/ActorLink.h",
    "mc/world/ActorUniqueID.h",
    "mc/world/ActorRuntimeID.h",
    "mc/world/effect/MobEffectInstance.h",
    "mc/world/actor/DistanceSortedActor.h",
    "mc/world/item/ItemDescriptor.h",
    "mc/math/Vec2.h",
    "mc/common/Brightness.h",
    "mc/common/BrightnessPair.h",
    "mc/deps/core/data/InterpolationPair.h",
    "mc/world/level/LevelSeed64.h",
    "mc/deps/core/mce/Color.h",
    "mc/enums/PlatformType.h",
    "mc/enums/ARVRPlatform.h",
    "mc/enums/AppFocusState.h",
    "mc/enums/OsVersion.h",
    "mc/enums/DeviceSunsetTier.h",
    "mc/enums/UIScalingRules.h",
    "mc/enums/VRControllerType.h",
    "mc/network/NetworkConnectionType.h",
    "mc/deps/core/secure_storage/SecureStorageKey.h"
)
foreach ($header in $Headers) {
    if (-not (Test-Path (Join-Path $SourceRoot $header))) {
        throw "LeviLamina header was not found: $header"
    }
}
if (-not (Test-Path (Join-Path $EnttRoot "entt/entt.hpp"))) {
    throw "pinned entt headers were not found under $EnttRoot."
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -LlvmRoot $LlvmRoot
}

$Frontend = Join-Path $RepoRoot "build\frontend\Release\hosihikari-ir-frontend.exe"
if (-not (Test-Path $Frontend)) {
    throw "IR frontend was not built: $Frontend"
}

$Umbrella = Join-Path $Output "umbrella.hpp"
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$UmbrellaText = ($Headers | ForEach-Object { "#include `"$_`"" }) -join "`n"
[System.IO.File]::WriteAllText($Umbrella, "$UmbrellaText`n", (New-Object System.Text.UTF8Encoding($false)))

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsRoot = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsRoot) { throw "Visual Studio with the C++ toolchain was not found." }

$Msvc = Get-ChildItem (Join-Path $VsRoot "VC\Tools\MSVC") | Sort-Object Name | Select-Object -Last 1
$SdkRoot = "C:\Program Files (x86)\Windows Kits\10\Include"
$Sdk = Get-ChildItem $SdkRoot | Sort-Object Name | Select-Object -Last 1

$Includes = @("-I", $EnttRoot, "-I", $SourceRoot, "-I", (Join-Path $SourceRoot "mc"))
foreach ($package in Get-ChildItem "$env:LOCALAPPDATA\.xmake\packages" -Recurse -Directory -Filter "include" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\e\\entt\\' } | Sort-Object FullName -Unique) {
    $Includes += @("-I", $package.FullName)
}
foreach ($include in $AdditionalIncludePath) {
    $Includes += @("-I", (Resolve-Path $include).Path)
}

$CompilerArgs = @(
    "-std=c++20",
    "-fms-extensions",
    "-fms-compatibility",
    "-fms-compatibility-version=19.44",
    "-D_WIN64",
    "-D_AMD64_",
    "-DNOMINMAX",
    "-DWIN32_LEAN_AND_MEAN"
) + $Includes + @(
    "-isystem", (Join-Path $Msvc.FullName "include"),
    "-isystem", (Join-Path $SdkRoot "$($Sdk.Name)\ucrt"),
    "-isystem", (Join-Path $SdkRoot "$($Sdk.Name)\shared"),
    "-isystem", (Join-Path $SdkRoot "$($Sdk.Name)\um")
)

& $Frontend `
    --surface bedrock `
    --output $Output `
    --root $SourceRoot `
    --filter (Join-Path $SourceRoot "mc") `
    --configuration win-x64-server `
    --module mc `
    --source $Umbrella `
    --flatten-anonymous-fields `
    -- @CompilerArgs

if ($LASTEXITCODE -ne 0) {
    throw "IR frontend exited with $LASTEXITCODE"
}

dotnet run --project (Join-Path $RepoRoot "src\Hosihikari.Bindings.Generator\Hosihikari.Bindings.Generator.csproj") -c Release -- validate-ir "$Output/ir-manifest.json" "$Output"

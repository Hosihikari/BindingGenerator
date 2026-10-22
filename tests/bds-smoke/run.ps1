param(
    [Parameter(Mandatory = $true)]
    [string]$ServerRoot,
    [int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Stop"

$ServerRoot = (Resolve-Path $ServerRoot).Path
$Result = Join-Path $ServerRoot "plugins\hosihikari-symbol-smoke\symbol-smoke-result.txt"
$Stdout = Join-Path $ServerRoot "symbol-smoke-server.out.log"
$Stderr = Join-Path $ServerRoot "symbol-smoke-server.err.log"

Remove-Item $Result, $Stdout, $Stderr -ErrorAction SilentlyContinue
$Process = Start-Process -FilePath (Join-Path $ServerRoot "bedrock_server_mod.exe") `
    -WorkingDirectory $ServerRoot `
    -RedirectStandardOutput $Stdout `
    -RedirectStandardError $Stderr `
    -PassThru -WindowStyle Hidden

for ($Index = 0; $Index -lt $TimeoutSeconds; $Index++) {
    Start-Sleep -Seconds 1
    if (Test-Path $Result) { break }
    if ($Process.HasExited) { break }
}

Start-Sleep -Seconds 1
if (-not $Process.HasExited) {
    Stop-Process -Id $Process.Id -Force
    $Process.WaitForExit()
}

if (-not (Test-Path $Result)) {
    Write-Output "no result file"
    if (Test-Path $Stdout) { Get-Content $Stdout -Tail 40 }
    if (Test-Path $Stderr) { Get-Content $Stderr -Tail 40 }
    exit 1
}

$Text = Get-Content $Result -Raw
Write-Output $Text.TrimEnd()
if ($Text -notmatch "(?m)^result=OK$") {
    exit 1
}

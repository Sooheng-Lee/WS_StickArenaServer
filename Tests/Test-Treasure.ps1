param(
    [string]$ServerPath = "$PSScriptRoot\..\bin\x64\Debug\VS_StickArenaServer.exe"
)

$ErrorActionPreference = 'Stop'
Add-Type -Path "$PSScriptRoot\TreasureUdpTests.cs"
$probe = [System.Net.Sockets.UdpClient]::new(0)
$port = $probe.Client.LocalEndPoint.Port
$probe.Dispose()
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = (Resolve-Path -LiteralPath $ServerPath).Path
$start.Arguments = [string]$port
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::Start($start)
$output = $process.StandardOutput.ReadToEndAsync()
$errors = $process.StandardError.ReadToEndAsync()
try {
    Start-Sleep -Milliseconds 300
    [TreasureUdpTests]::Run($port)
    Write-Output 'PASS: real UDP 20-second spawn/hits, pickup broadcast, duplicate retry, competing pickup rejection, collected late-join state, and refill.'
}
finally {
    if (!$process.HasExited) { $process.Kill() }
    $process.WaitForExit()
    $stdout = $output.GetAwaiter().GetResult()
    $stderr = $errors.GetAwaiter().GetResult()
    $process.Dispose()
    if ($stderr) { Write-Output $stderr }
}

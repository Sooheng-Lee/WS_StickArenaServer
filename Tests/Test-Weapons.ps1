param([string]$ServerPath = "$PSScriptRoot/../bin/x64/Release/VS_StickArenaServer.exe")
$ErrorActionPreference = 'Stop'
Add-Type -Path "$PSScriptRoot/WeaponUdpTests.cs"
$probe = [System.Net.Sockets.UdpClient]::new(0)
$port = $probe.Client.LocalEndPoint.Port
$probe.Dispose()
$server = Start-Process -FilePath (Resolve-Path -LiteralPath $ServerPath).Path -ArgumentList "$port" -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Milliseconds 300
    [WeaponUdpTests]::Run($port)
    Write-Output 'PASS: equipment round-trip, late join, invalid ammo rejection, unequip, and stale-state rejection.'
}
finally {
    if (!$server.HasExited) { $server.Kill(); $server.WaitForExit() }
    $server.Dispose()
}

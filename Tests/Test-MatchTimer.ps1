param([string]$ServerPath = "$PSScriptRoot\..\bin\x64\Release\VS_StickArenaServer.exe")
$ErrorActionPreference = 'Stop'
$probe = [Net.Sockets.UdpClient]::new(0)
$port = $probe.Client.LocalEndPoint.Port
$probe.Dispose()
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = (Resolve-Path -LiteralPath $ServerPath).Path
$start.Arguments = [string]$port
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($start)
$output = $process.StandardOutput.ReadToEndAsync()
$errors = $process.StandardError.ReadToEndAsync()
$a = [Net.Sockets.UdpClient]::new(0)
$b = [Net.Sockets.UdpClient]::new(0)
$a.Connect('127.0.0.1', $port)
$b.Connect('127.0.0.1', $port)
$a.Client.ReceiveTimeout = 400
$b.Client.ReceiveTimeout = 400
$idA = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
$idB = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
function Send-Packet($Socket, [string]$Packet) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($Packet)
    [void]$Socket.Send($bytes, $bytes.Length)
}
function Register($Socket, $Id) {
    Send-Packet $Socket "SA2|type=REGISTER|session=$Id|name=41|character=41|mesh=|anim="
}
function Receive-Until($Socket, [scriptblock]$Condition, [int]$Seconds = 4) {
    $limit = [DateTime]::UtcNow.AddSeconds($Seconds)
    $from = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
    while ([DateTime]::UtcNow -lt $limit) {
        try { $text = [Text.Encoding]::ASCII.GetString($Socket.Receive([ref]$from)) }
        catch [Net.Sockets.SocketException] { continue }
        $fields = @{}
        foreach ($part in ($text -split '\|' | Select-Object -Skip 1)) {
            $pair = $part -split '=', 2
            $fields[$pair[0]] = $pair[1]
        }
        if (& $Condition $fields) { return $fields }
    }
    throw 'Expected UDP packet was not received'
}
try {
    Start-Sleep -Milliseconds 300
    Register $a $idA
    $initial = Receive-Until $a { param($f) $f.type -eq 'MATCH_STATE' }
    if ([int]$initial.remaining_ms -lt 179000 -or [int]$initial.remaining_ms -gt 180000) { throw 'Not a three minute match' }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Milliseconds 1200
    Register $b $idB
    $late = Receive-Until $b { param($f) $f.type -eq 'MATCH_STATE' }
    if ($late.epoch -ne $initial.epoch -or [int]$late.remaining_ms -ge [int]$initial.remaining_ms) { throw 'Late join reset timer' }
    $seq = 0
    $nextHeartbeat = 0.0
    $lastRemaining = 180000
    while ($watch.Elapsed.TotalSeconds -lt 185) {
        # Drain the observer too so its UDP receive buffer cannot fill over three minutes.
        $from = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        while ($b.Available -gt 0) { [void]$b.Receive([ref]$from) }
        if ($watch.Elapsed.TotalSeconds -ge $nextHeartbeat) {
            Register $a $idA
            Register $b $idB
            ++$seq
            Send-Packet $a "SA3|type=PLAYER_STATE|session=$idA|seq=$seq|action=MOVE|state=MOVING|input=1,0,0|pos=100,200,90|vel=400,0,0|rot=0,0,0|attack=0"
            $nextHeartbeat = $watch.Elapsed.TotalSeconds + 1
        }
        $timer = Receive-Until $a { param($f) $f.type -eq 'MATCH_STATE' }
        $remaining = [int]$timer.remaining_ms
        if ($remaining -gt $lastRemaining) { throw 'Timer increased' }
        $lastRemaining = $remaining
        if ($remaining -eq 0) { break }
    }
    if ($lastRemaining -ne 0 -or $watch.Elapsed.TotalSeconds -lt 179) { throw 'Incorrect expiry' }
    while ($b.Available -gt 0) { [void]$b.Receive([ref]$from) }
    ++$seq
    Send-Packet $a "SA3|type=PLAYER_STATE|session=$idA|seq=$seq|action=MOVE|state=MOVING|input=1,0,0|pos=9999,9999,90|vel=400,0,0|rot=0,0,0|attack=0"
    $frozen = Receive-Until $b { param($f) $f.type -eq 'PLAYER_STATE' -and $f.session -eq $idA -and [int]$f.seq -eq $seq }
    if ($frozen.pos -ne '100,200,90' -or $frozen.vel -ne '0,0,0' -or $frozen.input -ne '0,0,0' -or $frozen.state -ne 'IDLE') { throw "Server did not freeze movement: $($frozen | Out-String)" }
    Register $a $idA
    $ended = Receive-Until $a { param($f) $f.type -eq 'MATCH_STATE' }
    if ([int]$ended.remaining_ms -ne 0) { throw 'Registration reopened match' }
    Write-Output 'PASS: real UDP three minute expiry, shared late join timer, heartbeats, server position freeze, no restart on registration'
} finally {
    $a.Dispose()
    $b.Dispose()
    if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
}

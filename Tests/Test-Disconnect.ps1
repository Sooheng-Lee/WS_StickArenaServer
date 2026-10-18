param([string]$ServerPath = "$PSScriptRoot\..\bin\x64\Debug\VS_StickArenaServer.exe")
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
$client = [Net.Sockets.UdpClient]::new(0)
$other = [Net.Sockets.UdpClient]::new(0)
$client.Connect('127.0.0.1', $port)
$other.Connect('127.0.0.1', $port)
$client.Client.ReceiveTimeout = 500
function Send-Packet($Socket, [string]$Packet) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($Packet)
    [void]$Socket.Send($bytes, $bytes.Length)
}
$id = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
$registration = "SA2|type=REGISTER|session=$id|name=41|character=41|mesh=|anim="
try {
    Start-Sleep -Milliseconds 300
    Send-Packet $client $registration
    $from = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
    $ack = [Text.Encoding]::ASCII.GetString($client.Receive([ref]$from))
    if ($ack -cne "SA2|type=REGISTER_ACK|session=$id") { throw 'Registration failed' }
    Send-Packet $other "SA2|type=LEAVE|session=$id"
    Start-Sleep -Milliseconds 100
    Send-Packet $client $registration
    $ack = [Text.Encoding]::ASCII.GetString($client.Receive([ref]$from))
    if ($ack -cne "SA2|type=REGISTER_ACK|session=$id") { throw 'Spoofed leave removed player' }
    1..3 | ForEach-Object { Send-Packet $client "SA2|type=LEAVE|session=$id" }
    Send-Packet $client $registration
    try {
        [void]$client.Receive([ref]$from)
        throw 'Delayed registration resurrected the disconnected session'
    } catch [Net.Sockets.SocketException] {
        if ($_.Exception.SocketErrorCode -ne [Net.Sockets.SocketError]::TimedOut) { throw }
    }
    # Simulate termination without a LEAVE packet: close the only active client socket.
    $crashedId = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    Send-Packet $other "SA2|type=REGISTER|session=$crashedId|name=42|character=41|mesh=|anim="
    $other.Client.ReceiveTimeout = 1000
    $ack = [Text.Encoding]::ASCII.GetString($other.Receive([ref]$from))
    if ($ack -cne "SA2|type=REGISTER_ACK|session=$crashedId") { throw 'Crash test registration failed' }
    $other.Dispose()
    Start-Sleep -Seconds 12
} finally {
    $client.Dispose()
    $other.Dispose()
    if (!$process.HasExited) { $process.Kill() }
    $process.WaitForExit()
    $stdout = $output.GetAwaiter().GetResult()
    $stderr = $errors.GetAwaiter().GetResult()
    $process.Dispose()
}
if ($stdout -notmatch "Player left $id \| players=0") { throw "Player vector was not emptied: $stdout $stderr" }
if ($stdout -notmatch "Player left $id \| players=0 \| reason=LEAVE \| name=A \| endpoint=") { throw "Missing normal exit details: $stdout" }
if ($stdout -notmatch "Player left $crashedId \| players=0 \| reason=TIMEOUT \| name=B \| endpoint=") { throw "Missing crash/timeout removal log: $stdout" }
if ([regex]::Matches($stdout, 'Player left ').Count -ne 2) { throw 'Removal logged more than once' }
Write-Output 'PASS: real UDP leave empties player vector immediately, spoof rejection, duplicate leave, delayed registration protection, abrupt disconnect timeout removal and console logs.'

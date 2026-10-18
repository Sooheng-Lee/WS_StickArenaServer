param(
    [string]$ServerPath = "$PSScriptRoot\..\bin\x64\Debug\VS_StickArenaServer.exe"
)

$ErrorActionPreference = 'Stop'
$udp = [System.Net.Sockets.UdpClient]::new(0)
$udp.Client.ReceiveTimeout = 1500
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

function ConvertTo-Hex([string]$Value) {
    return ([BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($Value))).Replace('-', '')
}
function Send-Registration([string]$Packet, [string]$Id) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($Packet)
    [void]$udp.Send($bytes, $bytes.Length, '127.0.0.1', $port)
    $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $ack = [Text.Encoding]::ASCII.GetString($udp.Receive([ref]$remote))
    if ($ack -cne "SA2|type=REGISTER_ACK|session=$Id") { throw "Unexpected ACK: $ack" }
}

try {
    Start-Sleep -Milliseconds 300
    $id = '0123456789abcdef0123456789abcdef'
    $name = ConvertTo-Hex '플레이어|테스트=1'
    $character = ConvertTo-Hex '/Game/Characters/BP_Player.BP_Player_C'
    $mesh = ConvertTo-Hex '/Game/Characters/SK_Player.SK_Player'
    $anim = ConvertTo-Hex '/Game/Animations/ABP_Player.ABP_Player_C'
    $packet = "SA2|type=REGISTER|session=$id|name=$name|character=$character|mesh=$mesh|anim=$anim"
    Send-Registration $packet $id
    # A lost ACK must cause an idempotent retry, not a duplicate vector entry.
    Send-Registration $packet $id
    $secondId = 'abcdef0123456789abcdef0123456789'
    Send-Registration ($packet.Replace($id, $secondId)) $secondId
    # Missing meshes/animation classes are valid for an unconfigured character.
    $thirdId = '11111111111111111111111111111111'
    Send-Registration "SA2|type=REGISTER|session=$thirdId|name=54657374|character=$character|mesh=|anim=" $thirdId
    foreach ($invalid in @(($packet.Replace('name=', 'unknown=')), ($packet + '|extra=1'), ($packet.Replace("name=$name", 'name=GG')))) {
        $bytes = [Text.Encoding]::ASCII.GetBytes($invalid)
        [void]$udp.Send($bytes, $bytes.Length, '127.0.0.1', $port)
        $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
        $receivedInvalidAck = $false
        try { [void]$udp.Receive([ref]$remote); $receivedInvalidAck = $true }
        catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut) { throw }
        }
        if ($receivedInvalidAck) { throw 'Malformed registration was acknowledged' }
    }
    $action = [Text.Encoding]::ASCII.GetBytes("SA1|session=$id|seq=1|action=ATTACK")
    [void]$udp.Send($action, $action.Length, '127.0.0.1', $port)
    Start-Sleep -Milliseconds 100
}
finally {
    $udp.Dispose()
    if (!$process.HasExited) { $process.Kill() }
    $process.WaitForExit()
    $stdout = $output.GetAwaiter().GetResult()
    $stderr = $errors.GetAwaiter().GetResult()
    $process.Dispose()
}
if ([regex]::Matches($stdout, 'Registered player ').Count -ne 3 -or $stdout -notmatch 'players=3') {
    throw "Unexpected vector contents: $stdout"
}
if ($stdout -notmatch 'action=ATTACK') { throw 'Existing action packet was not handled' }
if (!$stdout.Contains('name=플레이어|테스트=1') -or !$stdout.Contains('mesh=/Game/Characters/SK_Player.SK_Player') -or
    !$stdout.Contains('anim=/Game/Animations/ABP_Player.ABP_Player_C')) {
    throw "Registration fields did not round-trip: $stdout"
}
if ([regex]::Matches($stderr, 'Dropped invalid player registration').Count -ne 3) {
    throw "Malformed packets were not rejected: $stderr"
}
Write-Output 'PASS: registration ACK, retry deduplication, multiple players, Unicode/delimiters, empty assets, malformed packets, existing actions.'

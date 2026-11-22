$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$profile = Join-Path $PSScriptRoot 'mode4.profraw'
$liveFile = Join-Path $root 'logs\live-commands.txt'

if (!(Test-Path -LiteralPath $liveFile)) { throw "Live command file not found: $liveFile" }
$running = Get-Process -Name kyty_emulator -ErrorAction SilentlyContinue
if (!$running) { throw 'Kyty is not running. Start pgo-capture.cmd first.' }
if ($profile -match '\s') {
	throw "The profile path cannot contain spaces: $profile"
}

$id = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$content = "id $id" + [Environment]::NewLine + "pgo $profile" + [Environment]::NewLine
[System.IO.File]::WriteAllText($liveFile, $content, [System.Text.Encoding]::ASCII)
Write-Host "Sent profile-save command. Kyty will write $profile and then exit."

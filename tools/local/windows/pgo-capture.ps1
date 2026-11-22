$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$exe = Join-Path $root 'kyty_emulator.exe'
$pgoExe = Join-Path $PSScriptRoot 'kyty_emulator_pgo.exe'
$backup = Join-Path $PSScriptRoot 'kyty_emulator.release-backup.exe'
$launcher = Join-Path $root 'KytyLauncher.exe'
$logs = Join-Path $root 'logs'
$liveFile = Join-Path $logs 'live-commands.txt'
$profile = Join-Path $PSScriptRoot 'mode4.profraw'

if (!(Test-Path -LiteralPath $exe)) { throw "Release emulator not found: $exe" }
if (!(Test-Path -LiteralPath $pgoExe)) { throw "Instrumented emulator not found: $pgoExe" }
if (!(Test-Path -LiteralPath $launcher)) { throw "KytyLauncher.exe not found: $launcher" }
if (Get-Process -Name kyty_emulator -ErrorAction SilentlyContinue) {
	throw 'Close Kyty completely before starting the PGO capture.'
}
if (Test-Path -LiteralPath $backup) {
	throw "A previous backup already exists: $backup. Restore it before continuing."
}
if ($profile -match '\s') {
	throw "The PGO output path cannot contain spaces because the live pgo command uses a space-delimited path: $profile"
}

New-Item -ItemType Directory -Force $logs | Out-Null
if (Test-Path -LiteralPath $profile) { Remove-Item -LiteralPath $profile -Force }
Copy-Item -LiteralPath $exe -Destination $backup

try {
	Copy-Item -LiteralPath $pgoExe -Destination $exe -Force
	$env:KYTY_PREFETCH_WAIT = '1'
	$env:KYTY_PREFETCH_WAIT_S = '1200'
	$env:KYTY_LIVE_FILE = $liveFile
	$env:LLVM_PROFILE_FILE = $profile

	Write-Host 'Mode 4 PGO capture started.'
	Write-Host 'Wait for "Shader prefetch wait: done" in the emulator log.'
	Write-Host 'Then load the same gameplay area, play normally for a few minutes, and run pgo-save.cmd.'
	Write-Host 'The pgo-save command writes the profile and closes the emulator.'
	$process = Start-Process -FilePath $launcher -WorkingDirectory $root -PassThru
	$process.WaitForExit()
	while (Get-Process -Name kyty_emulator -ErrorAction SilentlyContinue) {
		Start-Sleep -Seconds 1
	}
}
finally {
	$running = Get-Process -Name kyty_emulator -ErrorAction SilentlyContinue
	if ($running) {
		Write-Warning 'Kyty is still running. Close it, then manually restore kyty_emulator.release-backup.exe over the root kyty_emulator.exe.'
	} elseif (Test-Path -LiteralPath $backup) {
		Copy-Item -LiteralPath $backup -Destination $exe -Force
		Remove-Item -LiteralPath $backup -Force
		Write-Host 'Original release emulator restored.'
	}
}

if (Test-Path -LiteralPath $profile) {
	Write-Host "PGO profile created: $profile"
} else {
	Write-Warning 'No PGO profile was created. Check that pgo-save.cmd was run while the instrumented game was active.'
}

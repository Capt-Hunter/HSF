# Records the game's real display output (ffmpeg ddagrab = desktop duplication, NVENC) while pressing SPACE (jump) a few times.
# Why not OBS Game Capture: it hooks the game's Present, so it only sees the real frames; DLSS-G's generated frames are not in it.
# dup_frames=0: ddagrab only emits when the screen changed, so every recorded frame is a frame the display showed (variable fps);
# showcase.py resamples to a constant 240 fps with --cfr 240.
# Usage: powershell -File record.ps1 -Label on [-Jumps 3] [-Gap 4] [-Window "Castlevania Belmont's Curse"]
# Writes <repo>\HSF\build\showcase\rec\<Label>.mkv + <Label>.json (jump times, seconds after ffmpeg started). Refuses while an
# anti-cheat game runs (guard.ps1). The game must stay the foreground window (DLSS-G switches itself off otherwise).
param(
  [Parameter(Mandatory)][string]$Label,
  [int]$Jumps = 3,
  [double]$Gap = 4.0,
  [double]$Lead = 2.5,
  [int]$Fps = 155,
  [string]$Window = "Castlevania Belmont's Curse"
)
$ErrorActionPreference = 'Stop'
$tools = Split-Path $PSScriptRoot -Parent
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'guard.ps1')
if ($LASTEXITCODE) { exit 1 }

Add-Type @"
using System; using System.Runtime.InteropServices;
public class RecK {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
function Key($vk, $down) { $sc = [byte][RecK]::MapVirtualKey($vk, 0); [RecK]::keybd_event([byte]$vk, $sc, $(if ($down) { 0 } else { 2 }), [UIntPtr]::Zero) }
function Tap($vk, $ms = 120) { Key $vk $true; Start-Sleep -Milliseconds $ms; Key $vk $false }

$game = Get-Process | Where-Object { $_.MainWindowTitle -eq $Window } | Select-Object -First 1
if (-not $game) { throw "game window '$Window' not found" }
function Focus-Game {
  for ($i = 0; $i -lt 10; $i++) {
    if ([RecK]::GetForegroundWindow() -eq $game.MainWindowHandle) { return }
    Tap 0x12 20   # an Alt tap lets SetForegroundWindow through
    [void][RecK]::SetForegroundWindow($game.MainWindowHandle)
    Start-Sleep -Milliseconds 300
  }
  throw "could not bring the game to the foreground"
}

$ffmpeg = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg*\ffmpeg-*\bin\ffmpeg.exe" | Select-Object -First 1).FullName
if (-not $ffmpeg) { $ffmpeg = (Get-Command ffmpeg).Source }
$recDir = Join-Path $tools '..\build\showcase\rec'
New-Item -ItemType Directory -Force $recDir | Out-Null
$dest = Join-Path (Resolve-Path $recDir) "$Label.mkv"
Remove-Item $dest -ErrorAction SilentlyContinue
$total = [math]::Ceiling($Lead + $Jumps * $Gap + 1)

Focus-Game
$t0 = Get-Date
$p = Start-Process $ffmpeg -WindowStyle Hidden -PassThru -ArgumentList @(
  '-y', '-v', 'error', '-f', 'lavfi', '-i', "ddagrab=framerate=${Fps}:dup_frames=0:draw_mouse=0:output_idx=0",
  '-t', $total, '-fps_mode', 'passthrough', '-c:v', 'h264_nvenc', '-preset', 'p4', '-rc', 'constqp', '-qp', '14', "`"$dest`"")
Start-Sleep -Seconds $Lead
$jumpAt = @()
for ($j = 1; $j -le $Jumps; $j++) {
  Focus-Game
  Tap 0x20 120                                  # SPACE = jump
  $jumpAt += [math]::Round(((Get-Date) - $t0).TotalSeconds, 2)
  Write-Output ("jump {0} at {1:HH:mm:ss.fff} (t={2} s)" -f $j, (Get-Date), $jumpAt[-1])
  Start-Sleep -Seconds $Gap
}
$p.WaitForExit()
if (-not (Test-Path $dest)) { throw "ffmpeg wrote nothing" }
@{ jumps = $jumpAt } | ConvertTo-Json | Set-Content ([IO.Path]::ChangeExtension($dest, 'json')) -Encoding ascii
"recorded: $dest ({0:N1} MB)" -f ((Get-Item $dest).Length / 1MB)

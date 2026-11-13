# Installs HSF for a native D3D12 game with its own Streamline: hsf.addon64 and an HSF.ini with DLSS-G inside the game and everything else off. No helper, no shaders.
# -LogOnly installs the logging probe instead, which changes nothing the game sends.
# The log comes out as <game>\HSF_sl_probe.log, and the last one moves to HSF_sl_probe.old.log. Refuses while the game (which locks the add-on) or an anti-cheat game is running.
# -Remove takes the add-on and HSF.ini out again and leaves the log.
# -DynamicRes says this game's vectors change size, so DLSS-G's dynamic resolution flag is on from the first frame. HSF saves the same thing itself when it sees it, and an install keeps it.
# -BringIn is for a game with no Streamline of its own: HSF starts Streamline from HSF_streamline and feeds DLSS-G from LaunchPad's flow and ReShade's depth, and the log is HSF_sl_bring.log.
param([Parameter(Mandatory)][string]$Game, [switch]$Remove, [switch]$LogOnly, [switch]$BringIn, [int]$Frames = 1, [switch]$DynamicRes, [string]$LaunchPadFrom = "")
powershell -NoProfile -File "$PSScriptRoot\guard.ps1"; if ($LASTEXITCODE) { exit 1 }
$full = (Resolve-Path $Game).Path.TrimEnd('\')
if (Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) }) { "A program from $Game is running: close the game first."; exit 1 }
if ($Remove) {
  Remove-Item -Force -ErrorAction SilentlyContinue "$Game\hsf.addon64", "$Game\HSF.ini"
  Remove-Item -Force -Recurse -ErrorAction SilentlyContinue "$Game\HSF_streamline"
  "removed HSF from $Game (log kept)"
  exit 0
}
Copy-Item -Force "$PSScriptRoot\..\build\x64\hsf.addon64" "$Game\hsf.addon64"
if ($BringIn) {
  # HSF's own Streamline, next to the game's and not over it: only the DLLs DLSS-G needs.
  New-Item -ItemType Directory -Force "$Game\HSF_streamline" | Out-Null
  foreach ($dll in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll') { Copy-Item -Force "$PSScriptRoot\..\..\Streamline SDK\bin\x64\$dll" "$Game\HSF_streamline\$dll" }
  # HSF.fx and iMMERSE LaunchPad, for the flow and depth. The add-on switches their techniques on. LaunchPad comes from a game that already has it (-LaunchPadFrom).
  $shaders = "$Game\reshade-shaders\Shaders"
  New-Item -ItemType Directory -Force $shaders, "$Game\reshade-shaders\Textures" | Out-Null
  Copy-Item -Force "$PSScriptRoot\..\shaders\HSF.fx" "$shaders\HSF.fx"
  if (-not (Test-Path "$shaders\MartysMods_LAUNCHPAD.fx")) {
    if (-not $LaunchPadFrom) { "LaunchPad is not in this game: pass -LaunchPadFrom <a game folder that has it>."; exit 1 }
    $from = "$LaunchPadFrom\reshade-shaders"
    Copy-Item -Force "$from\Shaders\MartysMods_LAUNCHPAD.fx" "$shaders\"
    Copy-Item -Force -Recurse "$from\Shaders\MartysMods" "$shaders\"
    Copy-Item -Force "$from\Textures\iMMERSE_bluenoise*.png" "$Game\reshade-shaders\Textures\"
  }
}
if ((Test-Path "$Game\HSF.ini") -and (Select-String -Path "$Game\HSF.ini" -Pattern '^DlssgDynamicRes=1' -Quiet)) { $DynamicRes = $true }
$mode = if ($BringIn) { "SlBringIn=1`r`nDlssgFrames=$Frames" } elseif ($LogOnly) { "StreamlineProbe=1" } else { "InGameDlssg=1`r`nDlssgFrames=$Frames" + $(if ($DynamicRes) { "`r`nDlssgDynamicRes=1" }) }
Set-Content -Path "$Game\HSF.ini" -Value "[HSF]`r`nExtra=0`r`nOverlay=0`r`nScore=0`r`n$mode" -Encoding Unicode
if (Test-Path "$Game\HSF_sl_probe.log") { Move-Item -Force "$Game\HSF_sl_probe.log" "$Game\HSF_sl_probe.old.log" }
if (Test-Path "$Game\HSF_sl_bring.log") { Move-Item -Force "$Game\HSF_sl_bring.log" "$Game\HSF_sl_bring.old.log" }
"installed " + $(if ($BringIn) { "Streamline brought in, with flow and depth" } elseif ($LogOnly) { "log only" } else { "DLSS-G inside the game" }) + ": " + (Get-FileHash "$Game\hsf.addon64" -Algorithm SHA256).Hash.Substring(0, 8)
$others = @(Get-ChildItem "$Game\*.addon64" | Where-Object { $_.Name -ne "hsf.addon64" })
if ($others.Count -gt 0) { "still in the folder: " + (($others | ForEach-Object { $_.Name }) -join ", ") }

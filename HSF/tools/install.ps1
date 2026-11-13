# Installs the freshly built HSF add-on, HSF.fx (asks LaunchPad for its optical flow), Streamline's DLLs and the DLSS-G helper into a D3D11 or DX9 game.
# Refuses while the game (which locks the add-on) or an anti-cheat game is running.
# 32-bit or 64-bit: -Bits 32|64, or found from the game folder (a *.addon32 there, e.g. gitc-uplift.addon32, means 32-bit).
# The previous add-on is kept as _hsf_experimental\hsf.addon<bits>.prev.
# HSF.ini: an existing one is kept as it is (HSF's defaults need no file). -FreshIni replaces it with a short test file: frame generation on,
# motion source and meaning automatic, overlay on, -DlssgFrames generated per real frame.
param([Parameter(Mandatory)][string]$Game, [int]$DlssgFrames = 1, [int]$Bits = 0, [switch]$FreshIni)
powershell -NoProfile -File "$PSScriptRoot\guard.ps1"; if ($LASTEXITCODE) { exit 1 }
$full = (Resolve-Path $Game).Path.TrimEnd('\')
if (Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) }) { "A program from $Game is running: close the game first (the add-on file is locked)."; exit 1 }
if ($Bits -eq 0) { $Bits = if (Get-ChildItem "$Game\*.addon32" -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne "hsf.addon32" }) { 32 } else { 64 } }
$name = if ($Bits -eq 32) { "hsf.addon32" } else { "hsf.addon64" }
$built = if ($Bits -eq 32) { "$PSScriptRoot\..\build\x86\hsf.addon32" } else { "$PSScriptRoot\..\build\x64\hsf.addon64" }
New-Item -ItemType Directory -Force "$Game\_hsf_experimental" | Out-Null
if (Test-Path "$Game\$name") { Copy-Item -Force "$Game\$name" "$Game\_hsf_experimental\$name.prev" }
Copy-Item -Force $built "$Game\$name"
Copy-Item -Force "$PSScriptRoot\..\shaders\HSF.fx" "$Game\reshade-shaders\Shaders\HSF.fx"
New-Item -ItemType Directory -Force "$Game\HSF_streamline" | Out-Null
Copy-Item -Force "$PSScriptRoot\..\..\Streamline SDK\bin\x64\*.dll" "$Game\HSF_streamline"
Copy-Item -Force "$PSScriptRoot\..\build\x64\hsf-dlssg-helper64.exe" "$Game\HSF_streamline"
$ini = "$Game\HSF.ini"
if ($FreshIni -or -not (Test-Path $ini)) {
  [System.IO.File]::WriteAllLines($ini, [string[]]@("[HSF]", "Extra=1", "Motion=auto", "Source=auto", "Overlay=1", "DlssgFrames=$DlssgFrames"))
  $iniNote = "HSF.ini written (frame generation on, $($DlssgFrames + 1)x)"
} else {
  $iniNote = "HSF.ini kept as it was"
}
"installed $name ($Bits-bit): " + (Get-FileHash "$Game\$name" -Algorithm SHA256).Hash.Substring(0, 8) + ", helper: " + (Get-FileHash "$Game\HSF_streamline\hsf-dlssg-helper64.exe" -Algorithm SHA256).Hash.Substring(0, 8) + "; $iniNote"

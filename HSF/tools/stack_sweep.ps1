# Stack sweep: runs build\x64\d3d12_scene.exe (a stand-in D3D12 game) with the real mod stack users run, in every mix that has broken before, and prints
# one PASS or FAIL line per case, then a total. Exit code 0 = all passed.
#   ReShade 6.8.0 (as dxgi.dll) + HSF as a fresh install (this build, clean HSF.ini), with or without RenoDX DLSS and GITC Uplift;
#   Streamline: none (HSF brings its own), the game's 2.14 or the game's 2.13; ReShade loaded before Streamline or after;
#   swap chain: SDR, HDR10 with Resident Evil 7's flags (0x842), FP16, borderless.
# A case passes when the scene runs to its end (no crash, no hang) and DLSS-G showed more than one frame per real frame. "bare" cases run the scene
# with Streamline and nothing else, to prove the scene and the card before blaming HSF.
# Usage: tools\stack_sweep.ps1 [-Full] [-Only <text>] [-Seconds 20] [-Clean <folder>]
#   -Full     every mix (about 60 cases); the default is a shorter list that still covers each mod, each Streamline, each order and each swap chain.
#   -Only     runs only the cases whose name contains this text.
#   -Clean    where the clean files are: ReShade's installer, renodx-dlss.addon64, GITC Uplift, Streamline 2.13. Read and copied, never changed.
# Each case's folder stays in build\sweep\<case> with every log; results go to build\sweep\results.txt.
# Keep the scene window in front while it runs: DLSS-G makes nothing without focus. Refuses while an anti-cheat game is up (guard.ps1).
param([switch]$Full, [string]$Only = "", [int]$Seconds = 20, [string]$Clean = "$env:USERPROFILE\Desktop\DLSS")
powershell -NoProfile -File "$PSScriptRoot\guard.ps1"; if ($LASTEXITCODE) { exit 1 }
$root = (Resolve-Path "$PSScriptRoot\..").Path
$sweep = "$root\build\sweep"
$files = "$sweep\_files"
$scene = "$root\build\x64\d3d12_scene.exe"
if (-not (Test-Path $scene)) { "build\x64\d3d12_scene.exe is missing: run tools\build.cmd x64"; exit 1 }
New-Item -ItemType Directory -Force $sweep, $files | Out-Null

# ---- The parts, copied once from the clean folder and the build ----
$reshade = "$files\ReShade64.dll"
if (-not (Test-Path $reshade)) {
  $setup = Get-ChildItem "$Clean\ReShade_Setup_*_Addon.exe" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  if (-not $setup) { "No ReShade_Setup_*_Addon.exe in $Clean"; exit 1 }
  # The installer is a zip behind an exe. Windows' tar reads it; .NET's zip reader does not.
  tar -xf $setup.FullName -C $files ReShade64.dll
  if (-not (Test-Path $reshade)) { "ReShade64.dll not found inside $($setup.Name)"; exit 1 }
}
$renodx = "$Clean\renodx-dlss.addon64"
$uplift = "$Clean\GITC Uplift"
$sl213 = "$Clean\DLSS 310.8.0 streamline 2.13 dll"
$sl214 = "$root\build\dlssg_prod"
foreach ($need in $renodx, "$uplift\Addons\gitc-uplift.addon64", "$sl213\sl.interposer.dll", "$sl214\sl.interposer.dll") {
  if (-not (Test-Path $need)) { "Missing: $need"; exit 1 }
}

# ---- The cases ----
# mods: hsf, renodx, uplift, both, bare (no ReShade, no HSF). sl: none, 214, 213. order: rf (ReShade first), sf (Streamline first). swap: sdr, hdr10, fp16, borderless.
$cases = New-Object System.Collections.Generic.List[object]
function Add-Case($mods, $sl, $order, $swap) { $cases.Add([pscustomobject]@{ Name = "$mods-sl$sl-$order-$swap"; Mods = $mods; Sl = $sl; Order = $order; Swap = $swap }) }
Add-Case bare 214 rf sdr
Add-Case bare 213 rf sdr
# DLSS-G itself shows nothing on an FP16 swap chain in D3D12 (status 0, 1 per real frame): this one fails until HSF turns FP16 into HDR10 there.
Add-Case bare 214 rf fp16
$modsList = 'hsf', 'renodx', 'uplift', 'both'
foreach ($mods in $modsList) {
  Add-Case $mods none rf sdr
  foreach ($sl in '214', '213') { foreach ($order in 'rf', 'sf') { Add-Case $mods $sl $order sdr } }
}
$swapMods = if ($Full) { $modsList } else { 'hsf', 'renodx' }
$swapSl = if ($Full) { 'none', '214', '213' } else { 'none', '214' }
foreach ($mods in $swapMods) { foreach ($sl in $swapSl) { foreach ($swap in 'hdr10', 'fp16', 'borderless') { Add-Case $mods $sl rf $swap } } }
if ($Only) { $cases = @($cases | Where-Object { $_.Name -like "*$Only*" }) }
"stack sweep: $($cases.Count) cases, $Seconds s each. Keep the scene window in front and leave the PC alone."

# ---- One case ----
function Install-Case($case, $dir) {
  if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
  New-Item -ItemType Directory -Force $dir | Out-Null
  Copy-Item $scene $dir
  if ($case.Mods -ne 'bare') {
    Copy-Item $reshade "$dir\dxgi.dll"
    [IO.File]::WriteAllLines("$dir\ReShade.ini", [string[]]@('[GENERAL]', 'EffectSearchPaths=.\reshade-shaders\Shaders\**', 'TextureSearchPaths=.\reshade-shaders\Textures\**',
        '[OVERLAY]', 'TutorialProgress=4', 'ShowClock=0'))
    # HSF as the release zip lays it out, with a clean HSF.ini.
    Copy-Item "$root\build\x64\hsf.addon64" $dir
    New-Item -ItemType Directory -Force "$dir\HSF_streamline", "$dir\reshade-shaders\Shaders", "$dir\reshade-shaders\Textures" | Out-Null
    foreach ($dll in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll') { Copy-Item "$sl214\$dll" "$dir\HSF_streamline\" }
    Copy-Item "$root\build\x64\hsf-dlssg-helper64.exe" "$dir\HSF_streamline\"
    Copy-Item "$root\shaders\HSF.fx" "$dir\reshade-shaders\Shaders\"
    [IO.File]::WriteAllLines("$dir\HSF.ini", [string[]]@('; HSF settings, a clean start. HSF fills this in itself: do not edit it.', '[HSF]'))
    if ($case.Mods -in 'renodx', 'both') { Copy-Item $renodx $dir }
    if ($case.Mods -in 'uplift', 'both') {
      Copy-Item -Recurse "$uplift\Addons\*" $dir
      Copy-Item "$uplift\Shaders\*" "$dir\reshade-shaders\Shaders\"
    }
  }
  # The game's own Streamline, in a folder of its own as games keep it.
  if ($case.Sl -ne 'none') {
    $from = if ($case.Sl -eq '213') { $sl213 } else { $sl214 }
    New-Item -ItemType Directory -Force "$dir\game_streamline" | Out-Null
    Get-ChildItem "$from\*.dll" | Where-Object { $_.Name -like 'sl.*' -or $_.Name -like 'nvngx_*' } | Copy-Item -Destination "$dir\game_streamline\"
  }
}

# One launch of the scene. Returns what it saw; launch 1 is the fresh install, launch 2 runs with the HSF.ini HSF wrote at launch 1
# (on a D3D12 game HSF picks its route at launch 1 and it starts at launch 2, as for a user).
function Launch-Case($case, $dir, $n) {
  $sceneArgs = @('--seconds', $Seconds, '--fps', 240, '--log', "`"$dir\scene_launch$n.log`"")
  if ($case.Sl -ne 'none') { $sceneArgs += '--sl', "`"$dir\game_streamline\sl.interposer.dll`"", '--order', $(if ($case.Order -eq 'sf') { 'sl-first' } else { 'reshade-first' }) }
  switch ($case.Swap) {
    'hdr10' { $sceneArgs += '--hdr10', '--flags', '0x842' }
    'fp16' { $sceneArgs += '--fp16' }
    'borderless' { $sceneArgs += '--borderless' }
  }
  $log = "$dir\scene_launch$n.log"
  $p = Start-Process "$dir\d3d12_scene.exe" -ArgumentList $sceneArgs -WorkingDirectory $dir -PassThru -RedirectStandardOutput "$dir\scene_stdout$n.txt" -RedirectStandardError "$dir\scene_stderr$n.txt"
  $null = $p.Handle
  $hang = -not $p.WaitForExit(($Seconds + 40) * 1000)
  if ($hang) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500 }
  # The helper and Uplift's helper go with the scene; any left over would hold the next launch's files.
  Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dir + '\', [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 500
  if (Test-Path "$dir\ReShade.log") { Copy-Item -Force "$dir\ReShade.log" "$dir\ReShade_launch$n.log" }
  $text = if (Test-Path $log) { Get-Content -Raw $log } else { '' }
  $code = if ($hang) { 'hang' } else { $p.ExitCode }
  $shown = 0
  foreach ($m in [regex]::Matches($text, 'presented (\d+)')) { $shown = [math]::Max($shown, [int]$m.Groups[1].Value) }
  [pscustomobject]@{ Hang = $hang; Code = $code; Ended = $text -match 'end: ran to the end'; Shown = $shown; NoFocus = ([regex]::Matches($text, 'NO FOCUS')).Count }
}

function Run-Case($case) {
  $dir = "$sweep\$($case.Name)"
  Install-Case $case $dir
  $why = @()
  $launches = if ($case.Mods -eq 'bare') { 1 } else { 2 }
  $last = $null
  foreach ($n in 1..$launches) {
    $l = Launch-Case $case $dir $n
    if ($l.Hang) { $why += "launch ${n}: HANG (killed)" }
    elseif ($l.Code -ne 0) { $why += "launch ${n}: exit $($l.Code)" + $(if (-not $l.Ended) { ' before its end (crash)' } else { ' at quit' }) }
    if ($l.NoFocus -gt 0) { $why += "launch ${n}: no focus $($l.NoFocus) s" }
    $last = $l
  }
  # What DLSS-G showed at the last launch, and whether HSF was the one driving it.
  $shown = $last.Shown
  $route = 'game'
  if ($case.Mods -ne 'bare') {
    $reshadeLog = if (Test-Path "$dir\ReShade_launch$launches.log") { Get-Content -Raw "$dir\ReShade_launch$launches.log" } else { '' }
    if ($reshadeLog -notmatch 'HSF \d[\d.]* \(64-bit\) loaded') { $why += 'HSF did not load' }
    $probe = if (Test-Path "$dir\HSF_sl_probe.log") { Get-Content -Raw "$dir\HSF_sl_probe.log" } else { '' }
    $bring = if (Test-Path "$dir\HSF_sl_bring.log") { Get-Content -Raw "$dir\HSF_sl_bring.log" } else { '' }
    $hsfShown = 0
    foreach ($m in [regex]::Matches($probe, 'DLSS-G now shows (\d+) per real frame')) { $hsfShown = [math]::Max($hsfShown, [int]$m.Groups[1].Value) }
    foreach ($m in [regex]::Matches($bring, 'showing (\d+) per real frame')) { $hsfShown = [math]::Max($hsfShown, [int]$m.Groups[1].Value) }
    $route = if ($case.Sl -eq 'none') { 'HSF bring-in' } else { 'HSF in game' }
    if ($hsfShown -lt 2) { $why += "$route`: HSF saw DLSS-G show $hsfShown per real frame" }
    $shown = [math]::Max($shown, $hsfShown)
  } elseif ($shown -lt 2) { $why += "DLSS-G showed $shown per real frame" }
  $ok = $why.Count -eq 0
  $detail = if ($ok) { "${shown}x, $route" } else { ($why -join '; ') }
  [pscustomobject]@{ Ok = $ok; Name = $case.Name; Detail = $detail }
}

$results = foreach ($case in $cases) {
  $r = Run-Case $case
  Write-Host ("{0}  {1,-34} {2}" -f $(if ($r.Ok) { 'PASS' } else { 'FAIL' }), $r.Name, $r.Detail)
  $r
}
$lines = @($results | ForEach-Object { "{0}  {1,-34} {2}" -f $(if ($_.Ok) { 'PASS' } else { 'FAIL' }), $_.Name, $_.Detail })
$failed = @($results | Where-Object { -not $_.Ok })
$total = "{0} passed, {1} failed" -f ($lines.Count - $failed.Count), $failed.Count
$total
[IO.File]::WriteAllLines("$sweep\results.txt", [string[]]($lines + $total))
if ($failed.Count -gt 0) { exit 1 }
exit 0

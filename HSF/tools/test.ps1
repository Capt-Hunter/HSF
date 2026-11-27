# Runs every offline test in build\x64 and build\x86 and prints one PASS or FAIL line each, then a total. Exit code 0 = all passed.
# Usage: tools\test.cmd [x64|x86] [nohelper]   (no arch = both; nohelper skips helper_exit_test.ps1, which starts the DLSS-G helper)
# Each test's full output goes to build\testlogs\<arch>_<test>.txt. Tests that need Streamline load it from build\dlssg_prod.
# Run tools\build.cmd first. Refuses to run while an anti-cheat game is up (guard.ps1).
$Options = @($args)
powershell -NoProfile -File "$PSScriptRoot\guard.ps1"; if ($LASTEXITCODE) { exit 1 }
$root = (Resolve-Path "$PSScriptRoot\..").Path
$logs = "$root\build\testlogs"
New-Item -ItemType Directory -Force $logs | Out-Null
$arches = @($Options | Where-Object { $_ -in 'x64', 'x86' }); if ($arches.Count -eq 0) { $arches = 'x64', 'x86' }
$withHelper = -not ($Options -contains 'nohelper')
$streamline = "$root\build\dlssg_prod"
$timeoutMs = 180000
$built = @([regex]::Matches((Get-Content -Raw "$root\CMakeLists.txt"), 'add_executable\((\w+_test)\b') | ForEach-Object { $_.Groups[1].Value })
$pass = 0; $fail = 0; $failed = @()

function Report($name, $ok, $detail) {
  if ($ok) { $script:pass++; "PASS  {0,-28} {1}" -f $name, $detail }
  else { $script:fail++; $script:failed += $name; "FAIL  {0,-28} {1}" -f $name, $detail }
}

# Runs one test exe with its output in $log. Returns its exit code ($null when it did not start or end) and a line for the report.
function RunTest($exe, $testArgs, $log) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $start = @{ FilePath = $exe.FullName; WorkingDirectory = $exe.DirectoryName; NoNewWindow = $true; PassThru = $true
    RedirectStandardOutput = $log; RedirectStandardError = "$log.err" }
  if ($testArgs) { $start.ArgumentList = $testArgs }
  try { $p = Start-Process @start -ErrorAction Stop } catch { return @{ code = $null; detail = "did not start: $($_.Exception.Message)" } }
  $null = $p.Handle  # keeps the exit code readable after the process ends
  if (-not $p.WaitForExit($timeoutMs)) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    return @{ code = $null; detail = "timed out after $($timeoutMs / 1000) s (log: $log)" }
  }
  $p.WaitForExit()
  Get-Content "$log.err" -ErrorAction SilentlyContinue | Add-Content $log; Remove-Item "$log.err" -ErrorAction SilentlyContinue
  $code = $p.ExitCode
  if ($null -eq $code) { return @{ code = $null; detail = "no exit code (log: $log)" } }
  $detail = "exit $code, $([int]$sw.Elapsed.TotalSeconds) s"
  if ($code -ne 0) { $detail += " (log: $log)" }
  return @{ code = $code; detail = $detail }
}

foreach ($arch in $arches) {
  # Only the tests CMakeLists.txt still builds: an old exe left in the build folder is not run.
  $tests = @(Get-ChildItem "$root\build\$arch\*_test.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.BaseName -in $built -and $_.Name -notin @('helper_exit_test.exe', 'xmx_programs_test.exe') } | Sort-Object Name)
  if ($tests.Count -eq 0) { Report "$arch (no tests)" $false "run tools\build.cmd $arch"; continue }
  foreach ($exe in $tests) {
    $name = "$arch\$($exe.BaseName)"
    $log = "$logs\${arch}_$($exe.BaseName).txt"
    # The Streamline tests take the Streamline folder (or its interposer) and a log path of their own.
    $testArgs = switch ($exe.BaseName) {
      'sl_game_test' { "`"$streamline\sl.interposer.dll`" `"$logs\${arch}_sl_game.log`"" }
      'sl_bring_test' { "`"$streamline`" `"$logs\${arch}_sl_bring.log`"" }
      default { '' }
    }
    if ($exe.BaseName -eq 'sl_copies_test') { $testArgs = "`"$logs\${arch}_sl_copies.log`"" }
    if ($exe.BaseName -match '^sl_' -and $exe.BaseName -ne 'sl_copies_test' -and -not (Test-Path "$streamline\sl.interposer.dll")) { Report $name $false "no Streamline in $streamline"; continue }
    $result = RunTest $exe $testArgs $log
    # sl_bring_test's window must have focus, or DLSS-G generates nothing, which says nothing about HSF: one more try, then say so.
    $noFocus = { $exe.BaseName -eq 'sl_bring_test' -and $result.code -ne 0 -and
      @(Select-String -Path $log -Pattern 'fed (\d+) frames, (\d+) with focus' | Where-Object { $_.Matches[0].Groups[1].Value -ne $_.Matches[0].Groups[2].Value }).Count -gt 0 }
    if (& $noFocus) { $result = RunTest $exe $testArgs $log; if (& $noFocus) { $result.detail += ' - its window had no focus twice: rerun with nothing in front' } }
    Report $name ($result.code -eq 0) $result.detail
  }
}

# The helper must die with its game: helper_exit_test.ps1 prints one line per case. A helper still alive after 10 s, or one that took more
# than 5 s to go, is a failure.
if ($withHelper -and ($arches -contains 'x86')) {
  $log = "$logs\helper_exit.txt"
  $text = @(& powershell -NoProfile -File "$PSScriptRoot\helper_exit_test.ps1" 2>&1 | ForEach-Object { "$_" })
  $text | Set-Content $log
  $cases = @($text | Where-Object { $_ -match 'game killed after' })
  $slow = @($cases | Where-Object { $_ -match 'STILL ALIVE' -or ($_ -match 'gone (\d+) ms' -and [int]$Matches[1] -gt 5000) })
  if ($cases.Count -eq 0) { Report 'helper_exit' $false "no results (log: $log)" }
  elseif ($slow.Count -gt 0) { Report 'helper_exit' $false "$($slow.Count) of $($cases.Count) cases slow or alive (log: $log)" }
  else { Report 'helper_exit' $true "$($cases.Count) cases, helper gone each time" }
}

""
"$pass passed, $fail failed"
if ($fail -gt 0) { "Failed: " + ($failed -join ', '); exit 1 }
exit 0

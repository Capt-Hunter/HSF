# Checks that hsf-dlssg-helper64.exe dies with its game (tests/helper_exit_test.cpp, built by tools\build.cmd into build\x86). Copies the helper
# next to Streamline's production DLLs in build\dlssg_prod, then for each case (job / watchdog only; stand-in game killed during the helper's
# Streamline set-up and after it) kills the stand-in like a crash and reports how long the helper took to go.
# Pass = gone at once with the job, within ~3 s without it. Run tools\build.cmd first.
powershell -NoProfile -File "$PSScriptRoot\guard.ps1"; if ($LASTEXITCODE) { exit 1 }
$root = Resolve-Path "$PSScriptRoot\.."
$test = "$root\build\x86\helper_exit_test.exe"
if (-not (Test-Path $test)) { "missing ${test}: run tools\build.cmd"; exit 1 }
Copy-Item -Force "$root\build\x64\hsf-dlssg-helper64.exe" "$root\build\dlssg_prod\"
$helper = "$root\build\dlssg_prod\hsf-dlssg-helper64.exe"
foreach ($case in @(@{ms = 500; job = ""}, @{ms = 6000; job = ""}, @{ms = 500; job = "nojob"}, @{ms = 6000; job = "nojob"}, @{ms = 6000; job = "stop"})) {
  $text = if ($case.job) { & $test $helper $case.ms $case.job } else { & $test $helper $case.ms }
  $hp = [int](($text | Select-String "helper pid (\d+)").Matches[0].Groups[1].Value)
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ((Get-Process -Id $hp -ErrorAction SilentlyContinue) -and $sw.ElapsedMilliseconds -lt 10000) { Start-Sleep -Milliseconds 20 }
  $alive = [bool](Get-Process -Id $hp -ErrorAction SilentlyContinue)
  if ($alive) { Stop-Process -Id $hp -Force }
  $mode = if ($case.job) { $case.job } else { "job" }
  $result = if ($alive) { "STILL ALIVE after 10 s (killed by the test)" } else { "gone $($sw.ElapsedMilliseconds) ms after the game died" }
  "{0,-6} game killed after {1,5} ms: helper {2} {3}" -f $mode, $case.ms, $result, (($text | Select-String "^job|^helper state|^stop flag") -join "; ")
}

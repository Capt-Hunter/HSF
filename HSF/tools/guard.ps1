# Refuses (exit 1) while a game with anti-cheat is running, so no HSF test tool (test app with ReShade, probes, the debug-string catcher,
# PresentMon) ever runs next to one. The rule: nothing of ours runs while e.g. Call of Duty is up (account-ban risk).
# Usage, before any test: powershell -File guard.ps1; if ($LASTEXITCODE) { stop }
$protected = 'cod|cod\d*-cod|BlackOps|ModernWarfare|Warzone|Vanguard|vgc|EasyAntiCheat|EAAntiCheat|BEService|BattlEye|FACEIT|Ricochet|valorant|FortniteClient|r5apex|TslGame|RainbowSix|destiny2|eldenring|start_protected_game'
$hits = @(Get-Process | Where-Object { $_.Name -match "^($protected)" })
if ($hits.Count -gt 0) {
  "BLOCKED: anti-cheat game running: " + (($hits | ForEach-Object { $_.Name }) -join ", ") + ". Not running any HSF test tool."
  exit 1
}
"guard: no anti-cheat game running"
exit 0

@echo off
rem Runs every offline test and prints PASS/FAIL per test. Usage: test.cmd [x64|x86] [nohelper]  (see test.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test.ps1" %*
exit /b %ERRORLEVEL%

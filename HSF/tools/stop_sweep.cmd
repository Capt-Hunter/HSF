@echo off
rem Stops a running stack sweep (tools\stack_sweep.ps1): it sees this file within half a second, ends the scene and saves the results so far.
echo stop> "%~dp0..\build\sweep\STOP"
echo Stack sweep told to stop.

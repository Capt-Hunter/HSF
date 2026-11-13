@echo off
rem Builds hsf.addon64 (build\x64) and hsf.addon32 (build\x86). Usage: build.cmd [x64|x86]  (no argument = both)
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build
set ROOT=%~dp0..
if "%1"=="" (
  call "%~f0" x64 || exit /b 1
  call "%~f0" x86 || exit /b 1
  exit /b 0
)
if "%1"=="x64" (call "%VCVARS%\vcvars64.bat" >nul) else (call "%VCVARS%\vcvarsamd64_x86.bat" >nul)
cmake -S "%ROOT%" -B "%ROOT%\build\%1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl || exit /b 1
cmake --build "%ROOT%\build\%1" || exit /b 1

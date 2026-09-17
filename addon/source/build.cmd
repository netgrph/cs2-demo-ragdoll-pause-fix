@echo off
setlocal
rem Builds RagdollPauseFix.dll and copies it to ..\release\
rem Needs Visual Studio 2022 or newer with "Desktop development with C++".

rem Newest Visual Studio first; the first one with the C++ build tools wins.
set "VS="
for %%v in (18 2022) do for %%e in (Enterprise Professional Community BuildTools) do (
  if not defined VS if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VS=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e"
)
if not defined VS goto novs

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"

set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"
if not exist "%NINJA%" set "NINJA=ninja"

"%CMAKE%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build build || exit /b 1

if not exist "..\release" mkdir "..\release"
copy /y "bin\RagdollPauseFix.dll" "..\release\RagdollPauseFix.dll" >nul || exit /b 1
echo BUILD OK: %~dp0..\release\RagdollPauseFix.dll
exit /b 0

:novs
echo Visual Studio with C++ tools was not found.
echo Install "Desktop development with C++" from https://visualstudio.microsoft.com/
exit /b 1

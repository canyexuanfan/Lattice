@echo off
setlocal

set "TASK_BUILD_TEMP=%~dp0..\.tmp\msbuild-temp"
if not exist "%TASK_BUILD_TEMP%" mkdir "%TASK_BUILD_TEMP%"
if errorlevel 1 exit /b %errorlevel%

set "TEMP=%TASK_BUILD_TEMP%"
set "TMP=%TASK_BUILD_TEMP%"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
if errorlevel 1 exit /b %errorlevel%

msbuild "%~dp0..\Lattice.vcxproj" /p:Configuration=%1 /p:Platform=x64 /m
exit /b %errorlevel%

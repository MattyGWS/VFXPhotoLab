@echo off
setlocal
cd /d "%~dp0"

rem Always ask CMake/Ninja to update the application before launching it.
rem The build is incremental, so unchanged files are not recompiled.
powershell -ExecutionPolicy Bypass -File "scripts\build-windows.ps1"
if errorlevel 1 exit /b %errorlevel%

if not exist "build\release\VFXPhotoLab.exe" (
    echo Build completed, but VFXPhotoLab.exe was not found. 1>&2
    exit /b 1
)

"build\release\VFXPhotoLab.exe" %*

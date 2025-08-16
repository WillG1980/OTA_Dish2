@echo off

idf.py build
if errorlevel 1 (
    echo Error detected, exiting...
    exit /b 1
)

echo Add new files
git add .
echo Commit changed files
git diff --cached --quiet || (
    git commit -m "Auto-commit on build: %DATE% %TIME%" 
    FOR /F %%i IN ('git rev-list --count HEAD') DO git tag -a build-%%i -m "Build tag"
)
REM Get the version as the commit count
echo get new git version
FOR /F %%i IN ('git rev-list --count HEAD') DO SET VERSION=%%i
echo Determined version to be: %VERSION%
REM Replace placeholder in version.h.in and write to version.h
REM echo set version # in string
REM powershell -Command "(Get-Content version.h.in) -replace '@VERSION@', '%VERSION%' | Set-Content version.h"
idf.py build
if errorlevel 1 (
    echo Error detected, exiting...
    exit /b 1
)
@echo on
set WEB_FIRMWARE="Y:\firmware\ota-dishwasher\%VERSION%.bin"
rem mkdir %WEB_FIRMWARE%
copy C:\Projects\esp\OTA_DISHWASHER\build\*.bin  %WEB_FIRMWARE%
#idf.py flash monitor



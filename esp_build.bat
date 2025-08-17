@echo off
echo get new git version
FOR /F %%i IN ('git rev-list --count HEAD') DO SET /a VERSION=%%i+1
echo Determined new version to be: %VERSION%, testing the build
idf.py build
if errorlevel 1 (
    echo Error detected, exiting, no GIT commit
    exit /b 1
)
echo Add new files to GIT
git add .
echo Commit changed files
git diff --cached --quiet || (
    git commit -m "Auto-commit on build: %DATE% %TIME%" 
    FOR /F %%i IN ('git rev-list --count HEAD') DO git tag -a build-%%i -m "Build tag"
)
echo Copy files to firmware update repository
@echo on
set WEB_FIRMWARE="Y:\firmware\ota-dishwasher\%VERSION%.bin"
rem mkdir %WEB_FIRMWARE%
copy C:\Projects\esp\OTA_DISH2\OTA_DISH2\build\ota-dishwasher.bin  %WEB_FIRMWARE%
rem idf.py flash monitor



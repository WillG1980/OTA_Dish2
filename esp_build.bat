@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem --- Work in repo root ---
pushd "%~dp0" || (echo Failed to cd to script directory & exit /b 1)
git rev-parse --is-inside-work-tree >NUL 2>&1 || (echo Not a git repo & popd & exit /b 1)

rem --- Stage and (if needed) commit so tags point to an exact snapshot ---
git add -A
set DID_COMMIT=0 
git diff --cached --quiet || (
  git commit -m "Build attempt: %DATE% %TIME%"
  if errorlevel 1 (echo Commit failed & popd & exit /b 1)
  set DID_COMMIT=1
)

rem --- Identify version + commit ---
for /f %%i in ('git rev-list --count HEAD') do set VERSION=%%i
for /f %%i in ('git rev-parse --short HEAD') do set SHORTSHA=%%i
echo Building commit !SHORTSHA! as build !VERSION! ...

rem --- Build ---
idf.py build
set BUILD_RC=%ERRORLEVEL%

rem --- Tag result (both success and failure paths) ---
set "TAG_BASE=build-!VERSION!"
if %BUILD_RC% NEQ 0 ( set "TAG_STATUS=!TAG_BASE!-fail" ) else ( set "TAG_STATUS=!TAG_BASE!-ok" )

for %%T in ("!TAG_BASE!" "!TAG_STATUS!") do (
  rem Re-point tags to this commit (delete if present, then recreate annotated)
  git rev-parse -q --verify "refs/tags/%%~T" >NUL 2>&1 && git tag -d "%%~T" >NUL 2>&1
  git tag -a "%%~T" -m "%%~T on %DATE% %TIME% (commit !SHORTSHA!)" || (
    echo Failed to create tag %%~T & popd & exit /b 1
  )
)

if %BUILD_RC% NEQ 0 (
  echo Build FAILED. Tagged HEAD as "!TAG_STATUS!".
  rem Optional: if you do NOT want failed commits to remain, uncomment:
  rem if "!DID_COMMIT!"=="1" git reset --soft HEAD~1
  popd
  exit /b 1
)

rem --- Success: archive artifact ---
set "WEB_DIR=Y:\firmware\ota-dishwasher"
if not exist "!WEB_DIR!" mkdir "!WEB_DIR!" >NUL 2>&1

set "SRC_BIN=build\ota-dishwasher.bin"
if not exist "!SRC_BIN!" (
  echo Artifact missing: "!SRC_BIN!" & popd & exit /b 1
)

set "DST_BIN=!WEB_DIR!\ota-dishwasher-!VERSION!.bin"
copy /Y "!SRC_BIN!" "!DST_BIN!" >NUL || (echo Copy failed to "!DST_BIN!" & popd & exit /b 1)

echo Build OK. Tagged HEAD as "!TAG_STATUS!" and copied "!DST_BIN!".

rem Optional: push with tags
rem git push origin HEAD --follow-tags

popd
endlocal

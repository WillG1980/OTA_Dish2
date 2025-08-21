@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem --- Repo root ---
pushd "%~dp0" || (echo Failed to cd to script directory & exit /b 1)
git rev-parse --is-inside-work-tree >NUL 2>&1 || (echo Not a git repo & popd & exit /b 1)

rem --- Prepare a commit so tags point at an exact snapshot ---
git add -A
set DID_COMMIT=0
git diff --cached --quiet || (
  git commit -m "Build attempt: %DATE% %TIME%"
  if errorlevel 1 (echo Commit failed & popd & exit /b 1)
  set DID_COMMIT=1
)

rem --- Version + commit info ---
for /f %%i in ('git rev-list --count HEAD') do set VERSION=%%i
for /f %%i in ('git rev-parse --short HEAD') do set SHORTSHA=%%i
echo Building commit !SHORTSHA! as build !VERSION! ...

rem --- Build ---
idf.py build
set BUILD_RC=%ERRORLEVEL%

rem --- Tag result (always) ---
set "TAG_BASE=build-!VERSION!"
if %BUILD_RC% NEQ 0 ( set "TAG_STATUS=!TAG_BASE!-fail" ) else ( set "TAG_STATUS=!TAG_BASE!-ok" )

for %%T in ("!TAG_BASE!" "!TAG_STATUS!") do (
  git rev-parse -q --verify "refs/tags/%%~T" >NUL 2>&1 && git tag -d "%%~T" >NUL 2>&1
  git tag -a "%%~T" -m "%%~T on %DATE% %TIME% (commit !SHORTSHA!)" || (
    echo Failed to create tag %%~T & popd & exit /b 1
  )
)

if %BUILD_RC% NEQ 0 (
  echo Build FAILED. Tagged as "!TAG_STATUS!".
  rem ---- If you want to push fail tags too, flip PUSH_ON_FAIL to 1 ----
  set PUSH_ON_FAIL=0
  if "!PUSH_ON_FAIL!"=="1" (
    call :EnsureUpstreamOrSet
    git push --follow-tags || (echo Push failed & popd & exit /b 1)
  )
  popd & exit /b 1
)

rem --- Success path: archive artifact and push ---
set "WEB_DIR=Y:\firmware\ota-dishwasher"
if not exist "!WEB_DIR!" mkdir "!WEB_DIR!" >NUL 2>&1
set "SRC_BIN=build\ota-dishwasher.bin"
if not exist "!SRC_BIN!" (echo Artifact missing: "!SRC_BIN!" & popd & exit /b 1)
set "DST_BIN=!WEB_DIR!\!VERSION!.bin"
copy /Y "!SRC_BIN!" "!DST_BIN!" >NUL || (echo Copy failed to "!DST_BIN!" & popd & exit /b 1)
echo Build OK. Copied "!DST_BIN!".

call :EnsureUpstreamOrSet
git push --follow-tags || (echo Push failed & popd & exit /b 1)
echo Pushed branch and tags.

popd
endlocal
exit /b 0
 
:EnsureUpstreamOrSet
for /f %%b in ('git rev-parse --abbrev-ref HEAD') do set BRANCH=%%b
git rev-parse --abbrev-ref --symbolic-full-name @{u} >NUL 2>&1
if errorlevel 1 (
  echo No upstream set; pushing with -u to origin/!BRANCH! ...
  git push -u origin "!BRANCH!" || (echo Initial upstream push failed & exit /b 1)
)
exit /b 0

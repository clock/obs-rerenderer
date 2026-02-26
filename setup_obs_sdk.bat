@echo off
REM run this from the "x64 Native Tools Command Prompt for VS 2022"
REM it will:
REM   1. clone obs-studio into example_sources\obs-studio (headers)
REM   2. generate obs.lib and obs-frontend-api.lib from the installed dlls
REM   3. write obs_sdk.props for you

setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set OBS_INSTALL=C:\Program Files\obs-studio
set OBS_DLL=%OBS_INSTALL%\bin\64bit\obs.dll
set OBS_FRONTEND_DLL=%OBS_INSTALL%\bin\64bit\obs-frontend-api.dll
set EXAMPLE_SOURCES=%SCRIPT_DIR%example_sources
set OBS_SOURCE=%EXAMPLE_SOURCES%\obs-studio
set LIB_OUT=%SCRIPT_DIR%obs_sdk_libs

REM -------------------------------------------------------------------------
echo [1/4] checking for obs install at %OBS_INSTALL%
if not exist "%OBS_DLL%" (
    echo ERROR: obs.dll not found at %OBS_DLL%
    echo make sure OBS Studio is installed, or edit this script to point to your install
    exit /b 1
)
echo   found obs.dll

REM -------------------------------------------------------------------------
echo [2/4] cloning obs-studio source for headers (shallow, libobs only matters)
if exist "%OBS_SOURCE%\.git" (
    echo   already cloned, skipping
) else (
    if not exist "%EXAMPLE_SOURCES%" mkdir "%EXAMPLE_SOURCES%"
    git clone --depth=1 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git "%OBS_SOURCE%"
    if errorlevel 1 (
        echo ERROR: git clone failed — make sure git is installed and you have internet
        exit /b 1
    )
    cd /d "%OBS_SOURCE%"
    git sparse-checkout set libobs cmake
    cd /d "%SCRIPT_DIR%"
)
echo   headers at %OBS_SOURCE%\libobs

REM -------------------------------------------------------------------------
echo [3/4] generating obs.lib and obs-frontend-api.lib from installed dlls
if not exist "%LIB_OUT%" mkdir "%LIB_OUT%"

call :gen_lib "%OBS_DLL%" "%LIB_OUT%\obs.lib"
if errorlevel 1 exit /b 1

if exist "%OBS_FRONTEND_DLL%" (
    call :gen_lib "%OBS_FRONTEND_DLL%" "%LIB_OUT%\obs-frontend-api.lib"
) else (
    echo   obs-frontend-api.dll not found, skipping
)

REM -------------------------------------------------------------------------
echo [4/4] writing obs_sdk.props
(
echo ^<?xml version="1.0" encoding="utf-8"?^>
echo ^<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"^>
echo   ^<PropertyGroup Label="UserMacros"^>
echo     ^<OBS_INCLUDE^>%OBS_SOURCE%\libobs^</OBS_INCLUDE^>
echo     ^<OBS_LIB^>%LIB_OUT%^</OBS_LIB^>
echo     ^<OBS_INSTALL^>%OBS_INSTALL%^</OBS_INSTALL^>
echo   ^</PropertyGroup^>
echo ^</Project^>
) > "%SCRIPT_DIR%obs_sdk.props"
echo   wrote obs_sdk.props

echo.
echo done! open obs-rerenderer.sln and build.
exit /b 0

REM -------------------------------------------------------------------------
:gen_lib
REM %1 = path to .dll, %2 = output .lib path
set DLL_PATH=%~1
set LIB_PATH=%~2
set DEF_PATH=%~2.def
set DLL_NAME=%~n1

echo   generating %LIB_PATH%

REM get exports from the dll
dumpbin /exports "%DLL_PATH%" > "%DLL_PATH%.exports.tmp" 2>nul
if errorlevel 1 (
    echo ERROR: dumpbin failed — are you running from the x64 Native Tools Command Prompt?
    exit /b 1
)

REM build the .def file with powershell
powershell -NoProfile -Command ^
    "$lines = Get-Content '%DLL_PATH%.exports.tmp';" ^
    "$exports = $lines | Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)' } | ForEach-Object { $matches[1] };" ^
    "$def = 'LIBRARY %DLL_NAME%`r`nEXPORTS`r`n' + ($exports -join \"`r`n\");" ^
    "Set-Content -Path '%DEF_PATH%' -Value $def -Encoding ASCII"

del "%DLL_PATH%.exports.tmp" >nul 2>&1

REM create import lib from def
lib /nologo /machine:x64 /def:"%DEF_PATH%" /out:"%LIB_PATH%" >nul
if errorlevel 1 (
    echo ERROR: lib command failed
    exit /b 1
)
del "%DEF_PATH%" >nul 2>&1
echo     done
exit /b 0

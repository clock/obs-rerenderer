@echo off
REM called by vs post-build event — also runnable manually as admin
REM args: %1=dll path, %2=data dir, %3=obs install dir

set DLL=%~1
set DATA=%~2
set OBS=%~3

if "%OBS%"=="" (
    echo usage: deploy.bat ^<dll^> ^<data_dir^> ^<obs_install^>
    exit /b 1
)

echo deploying to %OBS%...

xcopy /Y /I "%DLL%" "%OBS%\obs-plugins\64bit\" >nul
if %errorlevel% GTR 1 (
    echo   dll copy failed - try running vs as administrator
    exit /b 0
)

xcopy /Y /I /E "%DATA%\*" "%OBS%\data\obs-plugins\obs-rerenderer\" >nul
if %errorlevel% GTR 1 (
    echo   data copy failed - try running vs as administrator
    exit /b 0
)

echo   done
exit /b 0

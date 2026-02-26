@echo off
setlocal

set MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.42.34433\bin\Hostx64\x64
set LIBS=%~dp0obs_sdk_libs
set OBS_BIN=C:\Program Files\obs-studio\bin\64bit

if not exist "%LIBS%" mkdir "%LIBS%"

echo generating obs.lib...
"%MSVC%\dumpbin.exe" /exports "%OBS_BIN%\obs.dll" > "%TEMP%\obs_exp.txt"
powershell -NoProfile -Command ^
  "$lines = Get-Content '%TEMP%\obs_exp.txt';" ^
  "$exports = $lines | Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)' } | ForEach-Object { $null = $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)'; $Matches[1] };" ^
  "('LIBRARY obs', 'EXPORTS') + $exports | Set-Content '%LIBS%\obs.def' -Encoding ASCII"
"%MSVC%\lib.exe" /nologo /machine:x64 /def:"%LIBS%\obs.def" /out:"%LIBS%\obs.lib"
del "%LIBS%\obs.def" 2>nul

echo generating obs-frontend-api.lib...
"%MSVC%\dumpbin.exe" /exports "%OBS_BIN%\obs-frontend-api.dll" > "%TEMP%\obsfe_exp.txt"
powershell -NoProfile -Command ^
  "$lines = Get-Content '%TEMP%\obsfe_exp.txt';" ^
  "$exports = $lines | Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)' } | ForEach-Object { $null = $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)'; $Matches[1] };" ^
  "('LIBRARY obs-frontend-api', 'EXPORTS') + $exports | Set-Content '%LIBS%\obs-frontend-api.def' -Encoding ASCII"
"%MSVC%\lib.exe" /nologo /machine:x64 /def:"%LIBS%\obs-frontend-api.def" /out:"%LIBS%\obs-frontend-api.lib"
del "%LIBS%\obs-frontend-api.def" 2>nul

echo done. libs in: %LIBS%

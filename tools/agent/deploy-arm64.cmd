@echo off
REM Deploy the AppSandbox guest binaries the way a real AppSandbox VM is provisioned:
REM   - binaries live in C:\Windows\AppSandbox\
REM   - the agent installs/starts itself as the "AppSandboxAgent" service (LocalSystem),
REM     which then spawns the channel helpers (input/clipboard/audio) into the console session.
REM Run elevated. Build first with build-arm64.cmd (produces the .exe files beside this script).

setlocal
set "DST=C:\Windows\AppSandbox"

echo === stopping any running agent/helpers ===
net stop AppSandboxAgent 2>nul
taskkill /f /im appsandbox-input.exe 2>nul
taskkill /f /im appsandbox-audio.exe 2>nul
taskkill /f /im appsandbox-clipboard.exe 2>nul
taskkill /f /im appsandbox-clipboard-reader.exe 2>nul
timeout /t 2 /nobreak >nul

if not exist "%DST%" mkdir "%DST%"

echo === copying binaries to %DST% ===
copy /y "%~dp0appsandbox-agent.exe" "%DST%\appsandbox-agent.exe" || ( echo COPY FAILED & exit /b 1 )
copy /y "%~dp0appsandbox-input.exe" "%DST%\appsandbox-input.exe" || ( echo COPY FAILED & exit /b 1 )
copy /y "%~dp0appsandbox-audio.exe" "%DST%\appsandbox-audio.exe" || ( echo COPY FAILED & exit /b 1 )
copy /y "%~dp0appsandbox-clipboard.exe" "%DST%\appsandbox-clipboard.exe" || ( echo COPY FAILED & exit /b 1 )
copy /y "%~dp0appsandbox-clipboard-reader.exe" "%DST%\appsandbox-clipboard-reader.exe" || ( echo COPY FAILED & exit /b 1 )

echo === installing + starting AppSandboxAgent service ===
"%DST%\appsandbox-agent.exe" --install

echo === service state ===
sc query AppSandboxAgent | findstr /i STATE
endlocal

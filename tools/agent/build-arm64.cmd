@echo off
REM Build the AppSandbox guest agent binaries for ARM64 with the installed VS2022.
REM
REM Visual Studio does not put its tools on PATH; this enters the VS Developer
REM Command Prompt environment first (located via vswhere, so it works wherever
REM VS is installed) and then uses the repo's vanilla cl invocations. No include
REM or lib path tweaks are needed — the dev prompt provides the Windows SDK.

setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo ERROR: VS2022 not found via vswhere & exit /b 1 )

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64
if errorlevel 1 ( echo ERROR: VsDevCmd failed & exit /b 1 )

cd /d "%~dp0"

echo === appsandbox-agent.exe (guest service) ===
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fe:appsandbox-agent.exe agent.c p9copy.c ..\transport\asb_transport.c ^
   ws2_32.lib
if errorlevel 1 ( echo BUILD FAILED ^(agent^) & exit /b 1 )

echo === appsandbox-input.exe (input injector + asb_transport) ===
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fe:appsandbox-input.exe appsandbox-input.c ..\transport\asb_transport.c ^
   user32.lib ws2_32.lib setupapi.lib advapi32.lib
if errorlevel 1 ( echo BUILD FAILED ^(input^) & exit /b 1 )

echo === appsandbox-audio.exe (WASAPI loopback capture + asb_transport) ===
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fe:appsandbox-audio.exe appsandbox-audio.c ..\transport\asb_transport.c ^
   ole32.lib ws2_32.lib wtsapi32.lib setupapi.lib advapi32.lib user32.lib
if errorlevel 1 ( echo BUILD FAILED ^(audio^) & exit /b 1 )

echo === appsandbox-clipboard.exe (ch5 writer + asb_transport) ===
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fe:appsandbox-clipboard.exe appsandbox-clipboard.c ..\transport\asb_transport.c ^
   ws2_32.lib user32.lib shell32.lib wtsapi32.lib userenv.lib setupapi.lib advapi32.lib
if errorlevel 1 ( echo BUILD FAILED ^(clipboard^) & exit /b 1 )

echo === appsandbox-clipboard-reader.exe (ch6 reader + asb_transport) ===
cl /nologo /O2 /W3 /DUNICODE /D_UNICODE /Fe:appsandbox-clipboard-reader.exe appsandbox-clipboard-reader.c ..\transport\asb_transport.c ^
   ws2_32.lib user32.lib shell32.lib wtsapi32.lib userenv.lib setupapi.lib advapi32.lib
if errorlevel 1 ( echo BUILD FAILED ^(clipboard-reader^) & exit /b 1 )

echo BUILT: %CD%\appsandbox-agent.exe
echo BUILT: %CD%\appsandbox-input.exe
echo BUILT: %CD%\appsandbox-audio.exe
echo BUILT: %CD%\appsandbox-clipboard.exe
echo BUILT: %CD%\appsandbox-clipboard-reader.exe
endlocal

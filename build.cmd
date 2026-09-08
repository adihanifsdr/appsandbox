@echo off
rem Membangun Nestbox dari checkout ini dan menaruhnya ke D:\Nestbox (folder
rem yang dituju shortcut). Tinggal klik ganda: menjalankan
rem tools\deploy-local.ps1, lalu jendelanya tetap terbuka supaya hasilnya
rem terbaca.
rem
rem Kalau Nestbox sedang jalan, Nestbox.exe dan nestbox_core.dll terkunci dan
rem tidak bisa diganti. Skrip ini menawarkan menutup Nestbox (secara normal,
rem bukan dipaksa), menyalin ulang tanpa build, lalu membukanya lagi.
rem Diam 20 detik = tidak (binari lama tetap dipakai sampai Nestbox ditutup).
rem
rem Argumen tambahan diteruskan ke deploy-local.ps1, misalnya:
rem     build.cmd -NoBuild
rem     build.cmd -Dest C:\path\to\install
setlocal
cd /d "%~dp0"
set DEST=D:\Nestbox

rem PowerShell 7 (pwsh) kalau ada; kalau tidak, Windows PowerShell 5.1 dengan
rem PSModulePath dikosongkan (warisan PSModulePath dari pwsh membuat 5.1 gagal
rem memuat modul bawaannya, mis. Get-FileHash tidak dikenal).
set PS=pwsh
where pwsh >nul 2>&1 || (set PS=powershell& set PSModulePath=)

%PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\deploy-local.ps1" %*
if errorlevel 1 (
    echo.
    echo GAGAL: deploy-local.ps1 keluar dengan kode %errorlevel%.
    goto :end
)

rem Binari root berbeda dari hasil build? Berarti Nestbox sedang jalan dan menguncinya.
fc /b "bin\Release\nestbox_core.dll" "%DEST%\nestbox_core.dll" >nul 2>&1
if not errorlevel 1 (
    fc /b "bin\Release\Nestbox.exe" "%DEST%\Nestbox.exe" >nul 2>&1
    if not errorlevel 1 goto :done
)
tasklist /FI "IMAGENAME eq Nestbox.exe" 2>nul | find /I "Nestbox.exe" >nul
if errorlevel 1 goto :done

echo.
echo Nestbox sedang jalan, jadi Nestbox.exe / nestbox_core.dll di %DEST% belum diganti.
choice /C YN /T 20 /D N /M "Tutup Nestbox, salin binari baru, lalu buka lagi"
if errorlevel 2 (
    echo Binari lama tetap dipakai. Tutup Nestbox lalu jalankan:  build.cmd -NoBuild
    goto :end
)

echo Menutup Nestbox...
taskkill /IM Nestbox.exe >nul 2>&1
for /L %%i in (1,1,30) do (
    tasklist /FI "IMAGENAME eq Nestbox.exe" 2>nul | find /I "Nestbox.exe" >nul || goto :closed
    timeout /T 1 /NOBREAK >nul
)
echo Nestbox belum tertutup setelah 30 detik; tutup sendiri lalu jalankan:  build.cmd -NoBuild
goto :end

:closed
%PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\deploy-local.ps1" -NoBuild
if errorlevel 1 (
    echo GAGAL: salin ulang keluar dengan kode %errorlevel%.
    goto :end
)
echo Membuka Nestbox lagi...
start "" "%DEST%\Nestbox.exe"

:done
echo.
echo OK: Nestbox di %DEST% sudah yang terbaru.

:end
echo.
pause

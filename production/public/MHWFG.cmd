@echo off
setlocal enableextensions

rem MHWFG optional launcher forwarder (candidate)
rem This script only starts the original MHWSS launcher from the game folder.
rem It is NOT required: launching MHWSSLauncher.exe directly is the normal path.
rem It sets or clears no diagnostic environment switches, writes no
rem configuration, changes no registry setting, downloads nothing, and does not
rem overwrite OptiScaler.ini or reset your saved menu settings.

cd /d "%~dp0" || (
    echo [MHWFG] Failed to switch to script directory.
    exit /b 1
)

rem --- Required files check ---
if not exist "MonsterHunterWorld.exe" (
    echo [MHWFG] MonsterHunterWorld.exe not found in the current directory.
    echo        Place this script in the Monster Hunter World game folder
    echo        next to MonsterHunterWorld.exe and try again.
    goto :fail
)
if not exist "MHWSSLauncher.exe" (
    echo [MHWFG] MHWSSLauncher.exe not found. MHWSS is not included in this package.
    echo        Obtain MHWSS separately and place it in the same game folder.
    goto :fail
)
if not exist "MHWSS.dll" (
    echo [MHWFG] MHWSS.dll not found. Ensure MHWSS is fully installed in the game folder.
    goto :fail
)
if not exist "d3d12.dll" (
    echo [MHWFG] d3d12.dll not found. Ensure OptiScaler is installed in the game folder.
    echo        If you have not installed OptiScaler yet, place the built OptiScaler.dll
    echo        as d3d12.dll in this directory.
    goto :fail
)

rem --- Launch the original MHWSS launcher ---
echo [MHWFG] Starting MHWSSLauncher...
start "" "MHWSSLauncher.exe"
goto :done

:fail
echo [MHWFG] Launch aborted. Resolve the issues above and try again.
exit /b 1

:done
endlocal
exit /b 0

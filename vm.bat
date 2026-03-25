@echo off
setlocal

set VBOX="C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
set VMNAME=AIOS
set RAW=%~dp0build\os.img
set VDI=%~dp0build\os.vdi

:: Check VBoxManage exists
if not exist %VBOX% (
    echo ERROR: VirtualBox not found at %VBOX%
    echo Install VirtualBox or update the path in this script.
    pause
    exit /b 1
)

:: Check raw image exists
if not exist "%RAW%" (
    echo ERROR: Disk image not found at %RAW%
    echo Run 'make clean ^&^& make all' first.
    pause
    exit /b 1
)

:: Power off VM if running
%VBOX% controlvm "%VMNAME%" poweroff >nul 2>&1
timeout /t 2 /nobreak >nul

:: Detach and unregister old VDI so we can replace it
%VBOX% showvminfo "%VMNAME%" >nul 2>&1
if not errorlevel 1 (
    %VBOX% storageattach "%VMNAME%" --storagectl "SATA" --port 0 --device 0 --medium none >nul 2>&1
)
%VBOX% closemedium disk "%VDI%" >nul 2>&1
if exist "%VDI%" del "%VDI%"

:: Convert raw image to VDI
echo Converting raw image to VDI...
%VBOX% convertfromraw "%RAW%" "%VDI%" --format VDI
if errorlevel 1 (
    echo ERROR: Failed to convert disk image.
    pause
    exit /b 1
)

:: Create VM if it doesn't exist
%VBOX% showvminfo "%VMNAME%" >nul 2>&1
if errorlevel 1 (
    echo Creating VM "%VMNAME%"...
    %VBOX% createvm --name "%VMNAME%" --ostype "Other_64" --register
    %VBOX% modifyvm "%VMNAME%" --memory 256 --vram 64 --cpus 1
    %VBOX% modifyvm "%VMNAME%" --firmware efi
    %VBOX% modifyvm "%VMNAME%" --graphicscontroller vboxvga
    %VBOX% modifyvm "%VMNAME%" --audio-driver dsound --audio-out on
    %VBOX% modifyvm "%VMNAME%" --nic1 nat --nictype1 82540EM
    %VBOX% modifyvm "%VMNAME%" --uart1 0x3F8 4 --uart-mode1 file %~dp0build\serial.log
    %VBOX% storagectl "%VMNAME%" --name "SATA" --add sata --controller IntelAhci --portcount 1
)

:: Attach fresh disk and start
%VBOX% storageattach "%VMNAME%" --storagectl "SATA" --port 0 --device 0 --type hdd --medium "%VDI%"
echo Starting %VMNAME%...
%VBOX% startvm "%VMNAME%"

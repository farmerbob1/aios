@echo off
echo ChaosFS Explorer
echo   1. Open raw image (build\os.img)
echo   2. Open VDI image (build\os.vdi)
echo.
set /p choice="Choose [1/2]: "
if "%choice%"=="2" (
    python3 tools\chaosfs_explorer.py build\os.vdi
) else (
    python3 tools\chaosfs_explorer.py build\os.img
)

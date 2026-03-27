@echo off
echo Extracting /screenshot.bmp from ChaosFS disk image...
echo.
python3 tools/extract_file.py build/os.img /screenshot.bmp screenshot.bmp
echo.
if exist screenshot.bmp (
    echo Saved to screenshot.bmp - opening...
    start screenshot.bmp
) else (
    echo No screenshot found. Make sure you:
    echo   1. Clicked the S button in the browser
    echo   2. Closed VirtualBox
    echo   3. Did NOT run vm.bat yet
)
echo.
pause

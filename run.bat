@echo off
"C:\Program Files\qemu\qemu-system-x86_64.exe" -cpu core2duo -m 256 -vga std -drive format=raw,file=%~dp0build\os.img -serial stdio -no-reboot -no-shutdown

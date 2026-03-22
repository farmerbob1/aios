@echo off
echo Click inside the QEMU window to capture the mouse. Ctrl+Alt+G to release.
"C:\Program Files\qemu\qemu-system-x86_64.exe" -cpu core2duo -m 256 -vga std -drive format=raw,file=%~dp0build\os.img -netdev user,id=net0,hostfwd=tcp::9090-:9090 -device e1000,netdev=net0 -device AC97,audiodev=audio0 -audiodev sdl,id=audio0,in.voices=0 -serial stdio -no-reboot -no-shutdown

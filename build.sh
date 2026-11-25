#!/bin/sh
# Builds VramReplay.exe with LLVM-MinGW against the FFmpeg 9.1 shared build, and copies the DLLs next to it.
set -e
cd "$(dirname "$0")"
PKG="$LOCALAPPDATA/Microsoft/WinGet/Packages"
LM=$(ls -d "$PKG"/MartinStorsjo.LLVM-MinGW.UCRT_*/llvm-mingw-* | head -1)
FF=$(ls -d "$PKG"/Gyan.FFmpeg.Shared_*/ffmpeg-*-shared | head -1)

mkdir -p bin build
"$LM/bin/x86_64-w64-mingw32-windres" res/app.rc -O coff -o build/app.res.o
# -mwindows: runs in the background with a tray icon, no console window
"$LM/bin/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -Wall -Wno-unused-function \
    -municode -mwindows -DUNICODE -D_UNICODE \
    -I"$FF/include" \
    src/main.cpp src/video.cpp src/audio.cpp src/ring.cpp src/saver.cpp src/tray.cpp src/mux.cpp src/recorder.cpp src/overlay.cpp build/app.res.o \
    -L"$FF/lib" -lavformat -lavcodec -lswresample -lavutil \
    -ld3d11 -ldxgi -lole32 -luuid -lwinmm -lshell32 -luser32 -lgdi32 -ladvapi32 -lversion -lwindowscodecs \
    -static-libstdc++ -static-libgcc \
    -o bin/VramReplay.exe
cp -u "$FF"/bin/avformat-*.dll "$FF"/bin/avcodec-*.dll "$FF"/bin/avutil-*.dll "$FF"/bin/swresample-*.dll bin/
# avcodec/avformat pull in these at load time as well
cp -u "$FF"/bin/swscale-*.dll bin/ 2>/dev/null || true
echo "built bin/VramReplay.exe"

# Clip editor
"$LM/bin/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -Wall -municode -mwindows -DUNICODE -D_UNICODE \
    -I"$FF/include" editor/edit.cpp build/app.res.o \
    -L"$FF/lib" -lavformat -lavcodec -lavutil -lws2_32 -lshell32 -lole32 -luuid -luser32 \
    -static-libstdc++ -static-libgcc -o bin/VramEdit.exe
cp -u editor/editor.html bin/
echo "built bin/VramEdit.exe"

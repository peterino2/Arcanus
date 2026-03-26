md "output" 2>nul
clang -Oz -DNDEBUG -Wall -Wextra -Werror -fno-builtin -fno-stack-protector -nostdlib -Wl,/nodefaultlib -Wl,/subsystem:windows -Wl,/entry:WinMainCRTStartup -o output\arcanus_release.exe platform.c -lkernel32 -luser32 -lgdi32 -ldxgi -ldxguid -ld3d11 -ld3dcompiler

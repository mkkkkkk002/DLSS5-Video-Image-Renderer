#!/usr/bin/env bash
set -e

MSVCROOT="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808"
WINKIT="C:/Program Files (x86)/Windows Kits/10"
SDKVER="10.0.22621.0"

export PATH="$MSVCROOT/bin/Hostx64/x64:$PATH"
export INCLUDE="$MSVCROOT/include;$WINKIT/Include/$SDKVER/shared;$WINKIT/Include/$SDKVER/ucrt;$WINKIT/Include/$SDKVER/um;$WINKIT/Include/$SDKVER/winrt"
export LIB="$MSVCROOT/lib/x64;$WINKIT/Lib/$SDKVER/um/x64;$WINKIT/Lib/$SDKVER/ucrt/x64"

cd "$(dirname "$0")"

echo "=== building dlss5nr_engine ==="
cl.exe /nologo /O2 /MD /EHa /std:c++17 /W3 \
    main.cpp ngx_params.cpp d3d12_ctx.cpp dlssnr.cpp video_pipe.cpp nvof_flow.cpp depth_anything.cpp blend_pass.cpp densify_pass.cpp meta_io.cpp \
    /Fe:dlss5nr_engine.exe \
    /link d3d12.lib dxgi.lib d3d11.lib d3dcompiler.lib

echo "=== BUILD OK ==="

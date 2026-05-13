#!/bin/bash
# macOS Apple Silicon MinGW-w64 全量编译 MeshService64.exe
# 用法: cd meshagent/MeshAgent && bash build-win64.sh

set -e

CC=x86_64-w64-mingw32-gcc
CXX=x86_64-w64-mingw32-g++
STRIP=x86_64-w64-mingw32-strip

# ---- 编译标志 ----
INC="-I. -Iopenssl/include -Imicrostack -Imicroscript -Imeshcore -Imeshconsole -Imeshservice"
DEFS="-DWIN32 -D_WINSERVICE -D_LINKVM -DMESH_AGENTID=4 -DMICROSTACK_PROXY -DMICROSTACK_NO_STDAFX"
CFLAGS="-std=gnu99 -g -Wall -fno-strict-aliasing ${INC} ${DEFS} -include mingw-compat.h"
CXXFLAGS="-std=c++11 -g -Wall -fno-strict-aliasing ${INC} ${DEFS}"

OUTDIR="build/win64"
mkdir -p "$OUTDIR"
echo "Output: $OUTDIR/"

# ---- 1. microstack ----
echo "[1/6] microstack..."
for f in \
    ILibAsyncServerSocket ILibAsyncSocket ILibAsyncUDPSocket ILibParsers \
    ILibMulticastSocket ILibRemoteLogging ILibWebClient ILibWebServer \
    ILibCrypto ILibSimpleDataStore ILibProcessPipe ILibIPAddressMonitor; do
    $CC $CFLAGS -c microstack/${f}.c -o $OUTDIR/${f}.o
done
echo "  OK"

# ---- 2. microscript ----
echo "[2/6] microscript..."
for f in \
    duktape duk_module_duktape \
    ILibDuktapeModSearch ILibDuktape_ChildProcess ILibDuktape_CompressedStream \
    ILibDuktape_Debugger ILibDuktape_Dgram ILibDuktape_DuplexStream \
    ILibDuktape_EncryptionStream ILibduktape_EventEmitter ILibDuktape_fs \
    ILibDuktape_GenericMarshal ILibDuktape_Helpers ILibDuktape_HttpStream \
    ILibDuktape_MemoryStream ILibDuktape_net ILibDuktape_NetworkMonitor \
    ILibDuktape_Polyfills ILibDuktape_ReadableStream ILibDuktape_ScriptContainer \
    ILibDuktape_SHA256 ILibDuktape_SimpleDataStore ILibDuktape_WebRTC \
    ILibDuktape_WritableStream; do
    $CC $CFLAGS -c microscript/${f}.c -o $OUTDIR/${f}.o
done
echo "  OK"

# ---- 3. zlib + meshcore + paintbrush ----
echo "[3/6] meshcore + paintbrush_overlay_win..."
for f in \
    meshcore/zlib/adler32 meshcore/zlib/deflate meshcore/zlib/inffast \
    meshcore/zlib/inflate meshcore/zlib/inftrees meshcore/zlib/trees \
    meshcore/zlib/zutil meshcore/agentcore meshcore/meshinfo; do
    $CC $CFLAGS -c ${f}.c -o $OUTDIR/$(basename ${f}).o
done
# paintbrush_overlay_win.cpp (C++ 文件)
$CXX $CXXFLAGS -c meshcore/paintbrush_overlay_win.cpp -o $OUTDIR/paintbrush_overlay_win.o
echo "  OK"

# ---- 4. Windows KVM ----
echo "[4/6] Windows KVM..."
$CC $CFLAGS -c meshcore/KVM/Windows/kvm.c -o $OUTDIR/kvm.o
$CC $CFLAGS -c meshcore/KVM/Windows/input.c -o $OUTDIR/input.o
$CC $CFLAGS -c meshcore/wincrypto.cpp -o $OUTDIR/wincrypto.o
$CXX $CXXFLAGS -c meshcore/KVM/Windows/tile.cpp -o $OUTDIR/tile.o
echo "  OK"

# ---- 5. Service entry ----
echo "[5/6] Service entry..."
$CC $CFLAGS -c meshservice/ServiceMain.c -o $OUTDIR/ServiceMain.o
$CXX $CXXFLAGS -c meshservice/firewall.cpp -o $OUTDIR/firewall.o
echo "  OK"

# ---- 6. Link ----
echo "[6/6] Linking MeshService64.exe..."
WINLIBS="-lws2_32 -lshlwapi -lcrypt32 -ladvapi32 -luser32 -lgdi32 -lgdiplus \
    -lcomctl32 -lole32 -lshell32 -lsetupapi -lpsapi -ldbghelp -lwtsapi32 \
    -lwinhttp -loleaut32 -lssp -liphlpapi"

$CXX -o $OUTDIR/MeshService64.exe $OUTDIR/*.o \
    openssl/libstatic/bsd/libcrypto64MT.lib openssl/libstatic/bsd/libssl64MT.lib \
    $WINLIBS -static-libgcc -static-libstdc++ 2>&1 && \
    $STRIP $OUTDIR/MeshService64.exe && \
    echo "  DONE" && ls -lh $OUTDIR/MeshService64.exe && \
    file $OUTDIR/MeshService64.exe || \
    echo "  LINK FAILED - try with -lbcrypt -lcrypt32"

echo ""
echo "========================================="
echo " Build complete: $OUTDIR/MeshService64.exe"
echo "========================================="

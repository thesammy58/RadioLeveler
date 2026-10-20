#!/usr/bin/env bash
# Builds RadioLeveler.asi and its RadioLeveler.pdb (debug symbols) into dist/, next to the
# default RadioLeveler.ini and SHA256SUMS.txt. Clang and LLD are used because they can write
# PDB files.
#
# Releases are built with llvm-mingw 20260922 (msvcrt), the compiler every tested build was
# made with: https://github.com/mstorsjo/llvm-mingw/releases/tag/20260922
# Unpack it anywhere and put its bin folder on the PATH (or set LLVM_MINGW to its folder).
# The output is reproducible: the same llvm-mingw gives byte-identical files wherever it and
# the repository are unpacked. Paths inside the PDB are recorded under C:\RadioLeveler\.
#
# Without llvm-mingw, the system Clang is used with the 32-bit MinGW-w64 headers and
# libraries (Debian/Ubuntu: sudo apt-get install clang lld g++-mingw-w64-i686-win32).
#
#   ./build.sh            dist/RadioLeveler.asi, .pdb, .ini and SHA256SUMS.txt
#   ./build.sh --tests    also obj/test/TS3.exe, the plug-in ABI test (run it on Windows or Wine)
set -euo pipefail
cd "$(dirname "$0")"

python3 tools/sync_ini.py >/dev/null || { python3 tools/sync_ini.py; exit 1; }
mkdir -p dist obj

if [ -z "${LLVM_MINGW:-}" ] && command -v i686-w64-mingw32-clang++ >/dev/null 2>&1; then
    LLVM_MINGW="$(dirname "$(dirname "$(readlink -f "$(command -v i686-w64-mingw32-clang++)")")")"
fi
if [ -n "${LLVM_MINGW:-}" ]; then
    # Run llvm-mingw's clang through a relative link, so no install path ends up in the PDB.
    ln -sfn "$LLVM_MINGW" obj/llvm-mingw
    CXX=(obj/llvm-mingw/bin/clang --target=i686-w64-mingw32 --driver-mode=g++ -no-canonical-prefixes)
    LINK=(-fuse-ld=lld -stdlib=libc++ -rtlib=compiler-rt -unwindlib=libunwind)
else
    CXX=(clang++ --target=i686-w64-mingw32)
    LINK=(-fuse-ld=lld -static-libgcc -static-libstdc++)
fi
FLAGS=(-std=c++17 -O2 -msse2 -mfpmath=sse -fno-exceptions -fno-rtti)

"${CXX[@]}" "${FLAGS[@]}" -g -gcodeview -gno-codeview-command-line -fdebug-compilation-dir=. \
    -Wall -Wextra -Wno-unused-parameter -c src/radio_leveler.cpp -o obj/radio_leveler.o
"${CXX[@]}" "${LINK[@]}" -shared -static -Wl,--no-insert-timestamp -Wl,-s -Wl,--pdb=dist/RadioLeveler.pdb \
    -Xlinker -Xlink=-pdbaltpath:%_PDB% -Xlinker '-Xlink=-pdbsourcepath:C:\RadioLeveler' \
    -o dist/RadioLeveler.asi obj/radio_leveler.o
cp RadioLeveler.ini dist/
(cd dist && sha256sum RadioLeveler.asi RadioLeveler.pdb | tee SHA256SUMS.txt)

if [ "${1:-}" = "--tests" ]; then
    mkdir -p obj/test
    "${CXX[@]}" "${LINK[@]}" "${FLAGS[@]}" -static -o obj/test/TS3.exe tests/abi_test.cpp
    cp dist/RadioLeveler.asi obj/test/
    echo "built obj/test/TS3.exe: run it from obj/test, next to its copy of RadioLeveler.asi"
fi

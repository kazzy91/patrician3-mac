#!/bin/sh
# Links the boot runtime against every translated module in out/build/<module>/ (run scripts/build.sh first).
#   runtime/build.sh   ->  out/boot
set -e
cd "$(dirname "$0")/.."
B=out/build
[ -f $B/p3/p3.a ] || { echo "out/build/p3/p3.a missing: run scripts/build.sh first" >&2; exit 1; }
# The module list: the exe first, then every other module that was built.
MODS="p3"
for d in $B/*/; do
    m=$(basename "$d")
    [ "$m" != p3 ] && [ -f "$d/$m.a" ] && MODS="$MODS $m"
done
{
    echo '#include "module.h"'
    for m in $MODS; do echo "extern const ModuleTables ${m}_module;"; done
    printf 'const ModuleTables *const all_modules[] = {'
    for m in $MODS; do printf '&%s_module, ' "$m"; done
    echo '};'
    echo "const unsigned module_count = sizeof all_modules / sizeof *all_modules;"
} > $B/modules.c
LIBS=""
for m in $MODS; do LIBS="$LIBS $B/$m/$m.a"; done
clang -arch arm64 -O1 -g -Wall -Wno-unused-function -Wno-misleading-indentation -fobjc-arc \
    -I recomp -I runtime \
    runtime/boot.c runtime/heap.c runtime/files.c runtime/kernel32.c runtime/user32.c runtime/misc.c runtime/registry.c \
    runtime/display.c runtime/com.c runtime/ddraw.c runtime/d3d.c runtime/png.c runtime/extra.c runtime/mss32.c \
    runtime/input.c runtime/threads.c runtime/window.m \
    $B/modules.c $LIBS -lz -framework Cocoa -framework Metal -framework QuartzCore \
    -Wl,-stack_size,0x4000000 -o out/boot
echo "ok: out/boot (modules: $MODS)"

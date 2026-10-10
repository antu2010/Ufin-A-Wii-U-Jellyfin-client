#!/usr/bin/env bash
# Checks a finished build for things the Wii U's RPX format can't hold,
# which otherwise only show up as elf2rpl "Unsupported relocation type"
# errors: thread-local storage (thread_local / __thread, relocation types
# 67-78). Run from the repo after `make` in build/.
set -u
READELF="${DEVKITPPC:-/opt/devkitpro/devkitPPC}/bin/powerpc-eabi-readelf"
[ -x "$READELF" ] || { echo "powerpc-eabi-readelf not found (is DEVKITPPC set?)"; exit 2; }
bad=0
while IFS= read -r obj; do
    n=$("$READELF" -r "$obj" 2>/dev/null | grep -c "TPREL\|DTPREL\|DTPMOD\|R_PPC_TLS")
    if [ "$n" != 0 ]; then
        echo "thread-local storage in: ${obj#build/CMakeFiles/ufin.dir/} ($n relocations)"
        "$READELF" -r "$obj" | grep "TPREL\|TLS" | awk '{print "    " $5}' | sort -u | head -5
        bad=1
    fi
done < <(find build/CMakeFiles/ufin.dir -name '*.o')
[ "$bad" = 0 ] && echo "OK: no thread-local storage in the build"
exit $bad

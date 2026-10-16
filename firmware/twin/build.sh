#!/bin/bash
# build.sh: builds the virtual CHOMPI from the working tree into build/: libtwin.a (twin.h's API
# on FRIZZ's own firmware) and frizz-twin (run.sh's script player). Rebuilds only what changed.
# build.sh wasm: the same for the browser, into web/build/ (needs Emscripten: emsdk in
# ~/opt/emsdk, or em++ on the PATH).
#
# The firmware (code/src) and libDaisy's UI, Switch, 4021 and MIDI code are copied unchanged
# into build/tree, with host/ (the board's stand-ins) and test/host/fatfs.h (the card) beside
# them, so every include resolves inside it.
#
# TWIN_FIRMWARE=DIR builds another firmware (a code/src from another commit, as compare.sh
# does), TWIN_BUILD=DIR builds into DIR instead of build/, and TWIN_DEFINES adds compiler
# flags (-DFRIZZ_BENCH=1 for the CPU bench's firmware).
source "$(dirname "$0")/../test/lib.sh"   # T, REPO, INC (DaisySP), the host DaisySP
TW=$REPO/firmware/twin
B=${TWIN_BUILD:-$TW/build}
FIRMWARE=${TWIN_FIRMWARE:-$REPO/firmware/code/src}
TREE=$B/tree
LIBDAISY=$REPO/firmware/code/libs/libDaisy/src
mkdir -p "$B"
ARGS=("$@")
# a build that has something to build takes the host lock first (../test/hostlock.sh, #439),
# running this again under it; one with nothing to do (run.sh's, serve.py's every load) doesn't
# wait for it. As a check's twin (lib.sh's twin_for, under unit.sh) it doesn't: a single check
# may run beside a heavy run
heavy() { source "$T/hostlock.sh" "${ARGS[@]}"; }

stage()
{
    rm -rf "$TREE.new"
    mkdir -p "$TREE.new/lib" "$TREE.new/src"
    for f in ui/UI.h ui/UI.cpp ui/UiEventQueue.h util/FIFO.h util/Stack.h util/ringbuffer.h \
             dev/sr_4021.h hid/switch.h hid/switch.cpp hid/midi.h hid/midi.cpp \
             hid/midi_parser.h hid/midi_parser.cpp hid/MidiEvent.h util/CpuLoadMeter.h; do
        mkdir -p "$TREE.new/lib/$(dirname "$f")"
        cp "$LIBDAISY/$f" "$TREE.new/lib/$f"
    done
    cp -r "$TW/host/." "$TREE.new/lib/"
    cp "$REPO/firmware/test/host/fatfs.h" "$TREE.new/lib/"
    cp "$FIRMWARE"/*.h "$FIRMWARE"/*.cpp "$TREE.new/src/"
    cp "$TW"/*.cpp "$TW"/*.h "$TREE.new/src/"
}

stage
SRCS="src/twin.cpp src/script.cpp src/encoder.cpp lib/ui/UI.cpp lib/hid/switch.cpp lib/hid/midi.cpp lib/hid/midi_parser.cpp"
# FxChain::Run in a translation unit of its own, as on the CHOMPI (#286): a firmware from before
# it has none
[ -f "$TREE.new/src/FxChain.cpp" ] && SRCS="$SRCS src/FxChain.cpp"

if [ "$1" = wasm ]; then
    command -v em++ > /dev/null || source ~/opt/emsdk/emsdk_env.sh > /dev/null 2>&1 \
        || { echo "no Emscripten: install emsdk to ~/opt/emsdk"; exit 1; }
    WEB=$TW/web/build
    VERSION=$(git -C "$REPO" rev-parse --short HEAD)$(git -C "$REPO" diff --quiet HEAD -- firmware/code firmware/twin || echo "+changes")
    FLAGS="-O3 -std=gnu++14 -funsigned-char -I$TREE/lib -I$TREE/src $INC"
    STAMP=$( (em++ --version; echo "$FLAGS $VERSION"; find "$TREE.new" -type f | sort | xargs cat) | md5sum | cut -d' ' -f1)
    if [ -f "$WEB/frizz-twin.wasm" ] && [ "$(cat "$WEB/twin.stamp" 2>/dev/null)" = "$STAMP" ]; then
        rm -rf "$TREE.new"
        exit 0
    fi
    heavy
    rm -rf "$TREE" && mv "$TREE.new" "$TREE"
    mkdir -p "$WEB" "$B/daisysp-wasm"
    if [ ! -f "$B/daisysp-wasm/libdaisysp.a" ]; then
        echo "building DaisySP for the browser"
        # clang's library doesn't bring size_t along as g++'s does
        for f in $(find "$DAISYSP" -name '*.cpp'); do
            em++ -O3 -std=gnu++14 -w -include cstddef -c "$f" $INC \
                -o "$B/daisysp-wasm/$(basename "$f" .cpp).o" &
        done
        wait
        for f in $(find "$DAISYSP" -name '*.cpp'); do
            [ -f "$B/daisysp-wasm/$(basename "$f" .cpp).o" ] \
                || { echo "DaisySP didn't build for the browser"; exit 1; }
        done
        emar rcs "$B/daisysp-wasm/libdaisysp.a" "$B"/daisysp-wasm/*.o
    fi
    echo "building the twin for the browser ($VERSION)"
    # main() runs as a fiber, which needs Asyncify
    em++ $FLAGS -w -DTWIN_VERSION="\"$VERSION\"" $(for f in $SRCS; do echo "$TREE/$f"; done) \
        "$TREE/src/wasm.cpp" "$B/daisysp-wasm/libdaisysp.a" -o "$WEB/frizz-twin.js" \
        -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 -sMODULARIZE -sEXPORT_NAME=FrizzTwin \
        -sENVIRONMENT=web,worker,node -sINITIAL_MEMORY=128MB -sALLOW_MEMORY_GROWTH \
        -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPU8,UTF8ToString,stringToNewUTF8 \
        || { echo "the twin didn't build for the browser"; exit 1; }
    echo "$STAMP" > "$WEB/twin.stamp"
    exit 0
fi

# -funsigned-char: char is unsigned on the CHOMPI, signed on x86
FLAGS="-O2 -g -std=gnu++14 -funsigned-char $TWIN_DEFINES -I$TREE/lib -I$TREE/src $INC"
STAMP=$( (g++ --version; echo "$FLAGS"; find "$TREE.new" -type f | sort | xargs cat) | md5sum | cut -d' ' -f1)
if [ -f "$B/frizz-twin" ] && [ "$(cat "$B/twin.stamp" 2>/dev/null)" = "$STAMP" ]; then
    rm -rf "$TREE.new"
    exit 0
fi
heavy
rm -rf "$TREE" && mv "$TREE.new" "$TREE"
echo "building the twin"

mkdir -p "$B/obj"
rm -f "$B/obj"/*.o
# the firmware's own files: warnings as the device build would show them aren't the twin's to fix
for f in $SRCS; do
    g++ $FLAGS -w -c "$TREE/$f" -o "$B/obj/$(basename "$f" .cpp).o" &
done
wait
for f in $SRCS; do
    [ -f "$B/obj/$(basename "$f" .cpp).o" ] || { echo "the twin didn't build"; exit 1; }
done
rm -f "$B/libtwin.a"
ar rcs "$B/libtwin.a" "$B"/obj/*.o
g++ $FLAGS -Wall -c "$TREE/src/cli.cpp" -o "$B/cli.o"
g++ "$B/cli.o" "$B/libtwin.a" "$BUILD/libdaisysp_host.a" -o "$B/frizz-twin"
echo "$STAMP" > "$B/twin.stamp"

#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Philip Bos
# Every check that runs without an FM-1: the engine (with sanitizers), the codec against the firmware's decoder, the
# parameter table's copy, the app in fm1-base's host simulator, the protocol client against the FM-1's own protocol
# code, the screen mirror pixel for pixel. Needs cc, node, python3 (the venv for the simulator's fonts).
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
B=app/build
mkdir -p $B/test
[ -x .venv/bin/python ] && PATH=$ROOT/.venv/bin:$PATH
# fm1-base fastmath.h fm_exp2_nf shifts a negative exponent left (fine on two's complement, reported upstream):
# that one check off, every other finding fatal
SAN="-fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
W="-Wall -Wextra -Wdouble-promotion -Wno-unused-parameter -Wno-unused-function"

echo "== engine (ASan, UBSan)"
cc -O1 $SAN $W -DPLAT_HOST -Iapp/dsp -Ibase/include app/dsp/engine.c app/tests/dsp_test.c -lm -o $B/dsp_test_san
$B/dsp_test_san | tail -3
echo "== engine (-O2: timing)"
cc -O2 $W -DPLAT_HOST -Iapp/dsp -Ibase/include app/dsp/engine.c app/tests/dsp_test.c -lm -o $B/dsp_test
$B/dsp_test $B/test/engine.wav | tail -2

echo "== codec: companion encoder vs firmware decoder"
cc -O2 -Iapp/dsp app/tests/efdec.c -o $B/efdec
node app/tests/codec_test.mjs $B/efdec | tail -1

echo "== parameter table"
python3 tools/gen_efparams.py --check

echo "== protocol: companion client vs FM-1 code (ASan, UBSan)"
cc -O1 $SAN $W -DPLAT_HOST -Iapp/dsp -Iapp -Ibase/include app/tests/proto_host.c app/dsp/engine.c -lm -o $B/proto_host
node app/tests/proto_test.mjs $B/proto_host | tail -1

echo "== the flasher's sounds step against the FM-1's code; computers only"
node app/tests/sounds_test.mjs $B/proto_host | tail -1
node app/tests/flasher_test.mjs | tail -1

echo "== the companion's stylesheets"
node app/tests/css_test.mjs | tail -1

echo "== recording in the companion (headless Chrome, its fake audio input)"
CHROME=${CHROME:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
if [ -x "$CHROME" ]; then
  python3 companion/serve.py 8748 --no-open > /dev/null 2>&1 &
  SRV=$!
  sleep 1
  if COMPANION=http://localhost:8748/ node --experimental-websocket --no-warnings app/tests/recording_test.mjs > $B/test/recording.txt 2>&1; then
    tail -1 $B/test/recording.txt; kill $SRV
  else
    grep -E "FAIL|Error" $B/test/recording.txt; kill $SRV; exit 1
  fi
else
  echo "(no Chrome: skipped)"
fi

echo "== the app in the host simulator"
python3 base/tools/build.py app --host | tail -1
$B/host/echofield_host app/tests/basic.txt $B/test | tail -1 | grep -v FAILED || { $B/host/echofield_host app/tests/basic.txt $B/test | grep FAIL; exit 1; }

echo "== MIDI clock in"
$B/host/echofield_host app/tests/clock.txt $B/test | tail -1 | grep -v FAILED || { $B/host/echofield_host app/tests/clock.txt $B/test | grep FAIL; exit 1; }

echo "== screen mirror, pixel for pixel"
$B/host/echofield_host app/tests/mirror.txt $B/test > /dev/null
node app/tests/mirror_test.mjs $B/test | tail -1

echo "== the manual's screenshots (docs/img, and the companion's manual)"
mkdir -p docs/img companion/manual
$B/host/echofield_host app/tests/screens.txt docs/img > /dev/null
for f in 02-play 03-sample 03b-listen 05-filter 09-oracle 10-presets 12-storage 15-punch 16-help 14-pulse; do cp docs/img/$f.png companion/manual/; done
echo "all tests passed"

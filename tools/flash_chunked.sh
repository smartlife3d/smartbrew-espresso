#!/usr/bin/env bash
# Chunked ESP32-S3 flash. The native USB CDC link on this board drops during long sustained
# transfers ("Could not configure port: Device not configured"), which kills a single
# write_flash of the whole 1.4 MB app. Writing in small chunks means a drop only costs one
# chunk, and that chunk is simply retried.
#
# Usage: flash_chunked.sh <port> <bin> <offset> [chunk_kb]
set -uo pipefail

PORT="${1:?port}"
BIN="${2:?binary}"
OFFSET="${3:?offset hex}"
CHUNK_KB="${4:-96}"
CHUNK=$((CHUNK_KB * 1024))
SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
TOTAL=$(( (SIZE + CHUNK - 1) / CHUNK ))

echo "flashing $BIN ($SIZE bytes) to 0x$OFFSET in $TOTAL chunks of ${CHUNK_KB} KB"

# Leave the chip sitting in the ROM downloader; every chunk write must not toggle reset.
esptool.py --chip esp32s3 --port "$PORT" --baud 115200 --before default_reset --after no_reset \
    chip_id >/dev/null 2>&1 || { echo "could not enter the downloader"; exit 1; }

i=0
while [ "$i" -lt "$TOTAL" ]; do
    START=$((i * CHUNK))
    LEN=$CHUNK
    [ $((START + LEN)) -gt "$SIZE" ] && LEN=$((SIZE - START))
    ADDR=$(printf '0x%x' $((OFFSET + START)))
    PART="/tmp/flash_chunk.bin"
    dd if="$BIN" of="$PART" bs=1 skip="$START" count="$LEN" 2>/dev/null

    ok=0
    for attempt in 1 2 3 4 5; do
        if esptool.py --chip esp32s3 --port "$PORT" --baud 921600 --before no_reset --after no_reset \
                write_flash "$ADDR" "$PART" >/tmp/flash_chunk.log 2>&1; then
            ok=1
            break
        fi
        echo "  chunk $i failed (attempt $attempt) - re-entering the downloader"
        sleep 2
        esptool.py --chip esp32s3 --port "$PORT" --baud 115200 --before default_reset --after no_reset \
            chip_id >/dev/null 2>&1
    done

    if [ "$ok" -ne 1 ]; then
        echo "chunk $i (addr $ADDR, len $LEN) failed after 5 attempts"
        tail -5 /tmp/flash_chunk.log
        exit 1
    fi
    i=$((i + 1))
    if [ $((i % 4)) -eq 0 ] || [ "$i" -eq "$TOTAL" ]; then
        echo "  $i/$TOTAL chunks written"
    fi
done

echo "verifying..."
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 --before no_reset --after no_reset \
    verify_flash "$OFFSET" "$BIN" 2>&1 | tail -4

# Boot the freshly written image.
esptool.py --chip esp32s3 --port "$PORT" --baud 115200 --before no_reset --after hard_reset \
    chip_id >/dev/null 2>&1
echo "done - chip rebooted"

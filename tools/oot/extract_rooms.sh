#!/bin/bash
# Batch-extract OoT rooms from ROM to PRM files for CD streaming.
# Usage: bash tools/oot/extract_rooms.sh /path/to/oot.z64

set -e

ROM="$1"
if [ -z "$ROM" ]; then
    echo "Usage: $0 <rom_path>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$SCRIPT_DIR/../../data/oot/rooms"
mkdir -p "$OUTDIR"

ROOMS=(
    ydan_room_0      # Deku Tree - main room
    ydan_room_1      # Deku Tree - basement
    spot04_room_0    # Kokiri Forest
    spot00_room_0    # Hyrule Field
    Bmori1_room_0    # Forest Temple - entrance
    HIDAN_room_0     # Fire Temple - entrance
    MIZUsin_room_0   # Water Temple - entrance
    HAKAdan_room_0   # Shadow Temple - entrance
    spot15_room_0    # Lon Lon Ranch
    spot01_room_0    # Kakariko Village
)

EXTRACT="$SCRIPT_DIR/extract_room.py"

for room in "${ROOMS[@]}"; do
    echo "=== Extracting $room ==="
    python3 "$EXTRACT" "$ROM" "$room" --prm "$OUTDIR/$room.prm" || {
        echo "  [WARN] Failed to extract $room, skipping"
        continue
    }
    echo ""
done

echo "Done. Extracted rooms to $OUTDIR:"
ls -lh "$OUTDIR"/*.prm 2>/dev/null

#!/usr/bin/env bash
# ==============================================================================
# sync_web_assets.sh - Re-sync root data/index.html into Android assets
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SOURCE_HTML="$ROOT_DIR/data/index.html"
TARGET_DIR="$SCRIPT_DIR/app/src/main/assets/web"
TARGET_HTML="$TARGET_DIR/index.html"

if [ ! -f "$SOURCE_HTML" ]; then
    echo "Error: Source $SOURCE_HTML not found!"
    exit 1
fi

mkdir -p "$TARGET_DIR"
cp "$SOURCE_HTML" "$TARGET_HTML"

# Inject echo5_style.css in <head> if not already present
if ! grep -q "echo5_style.css" "$TARGET_HTML"; then
    sed -i '' 's|</head>|  <link rel="stylesheet" href="echo5_style.css">\n</head>|g' "$TARGET_HTML"
fi

# Inject echo5_bridge.js before </body> if not already present
if ! grep -q "echo5_bridge.js" "$TARGET_HTML"; then
    sed -i '' 's|</body>|  <script src="echo5_bridge.js"></script>\n</body>|g' "$TARGET_HTML"
fi

echo "Successfully synchronized $SOURCE_HTML -> $TARGET_HTML"

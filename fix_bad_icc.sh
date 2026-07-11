#!/bin/bash
# Strip ICC profiles from PNGs that have corrupted/malformed ICC chunks
# These cause ssimulacra2 to fail during tuning
set -e
dir="${1:-images}"
echo "Scanning $dir for PNGs with bad ICC profiles..."
count=0
for f in "$dir"/*.png; do
    [ -f "$f" ] || continue
    # Check if ssimulacra2 can read this file
    if ! ssimulacra2 "$f" "$f" >/dev/null 2>&1; then
        echo "  BAD: $f"
        convert "$f" -strip "$f" 2>/dev/null && echo "    -> stripped ICC" || echo "    -> FAILED to fix"
        count=$((count+1))
    fi
done
echo "Fixed $count files."

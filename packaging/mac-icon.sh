#!/bin/sh
set -eu
icon=$1 dir=$2 output=$3 sips=$4 iconutil=$5
mkdir -p "$dir"
for size in 16 32 128 256 512; do
    "$sips" -z "$size" "$size" "$icon" --out "$dir/icon_${size}x${size}.png" >/dev/null
    scaled=$((size * 2))
    "$sips" -z "$scaled" "$scaled" "$icon" --out "$dir/icon_${size}x${size}@2x.png" >/dev/null
done
"$iconutil" -c icns "$dir" -o "$output"

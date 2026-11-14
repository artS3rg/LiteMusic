#!/usr/bin/env bash
set -euo pipefail

bus=org.mpris.MediaPlayer2.LiteMusic
object=/org/mpris/MediaPlayer2
player=${bus%.*}.Player
G_DEBUG=fatal-criticals "$1" --verify-media-keys "$2" &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT

call() {
    gdbus call --session --timeout 2 --dest "$bus" --object-path "$object" --method "$@"
}

wait_for_status() {
    local expected=$1
    for (( attempt=0; attempt<40; ++attempt )); do
        if call org.freedesktop.DBus.Properties.Get "$player" PlaybackStatus 2>/dev/null | grep -q "'$expected'"; then
            return
        fi
        kill -0 "$pid"
        sleep 0.15
    done
    echo "LiteMusic did not report $expected via MPRIS" >&2
    exit 1
}

wait_for_position() {
    local min=$1 max=$2
    for (( attempt=0; attempt<40; ++attempt )); do
        local position
        position=$(call org.freedesktop.DBus.Properties.Get "$player" Position | sed -n 's/.*int64 \([0-9]*\).*/\1/p')
        if [[ -n "$position" ]] && (( position >= min && position <= max )); then
            return
        fi
        sleep 0.15
    done
    echo "LiteMusic did not reach the expected position via MPRIS" >&2
    exit 1
}

wait_for_status Playing
wait_for_position 150000 10000000
call "$player.Pause"
wait_for_status Paused
call "$player.SetPosition" /org/mpris/MediaPlayer2/Track/local 4500000
wait_for_position 4000000 5000000
call "$player.Previous"
wait_for_position 0 100000
wait_for_status Paused
call "$player.Play"
wait_for_status Playing
call "$player.PlayPause"
wait_for_status Paused
call "$player.PlayPause"
wait_for_status Playing

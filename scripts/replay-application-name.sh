#!/bin/sh

window=$(xdotool selectwindow)
window_name=$(xdotool getwindowclassname "$window" || xdotool getwindowname "$window" || echo "game")
window_name="$(echo "$window_name" | tr '/\\' '_')"
gpu-screen-recorder -w "$window" -f 60 -c mkv -a "$(pactl get-default-sink).monitor" -r 60 -o "$HOME/Videos/Replays/$window_name"

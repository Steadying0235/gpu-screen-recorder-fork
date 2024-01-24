#!/bin/sh

window=$(xdotool selectwindow)
window_name=$(xdotool getwindowclassname "$window" || xdotool getwindowname "$window" || echo "game")
gpu-screen-recorder -w "$window" -f 60 -c mkv -a "$(pactl get-default-sink).monitor" -r 60 -o "$HOME/Videos/replay/$window_name"

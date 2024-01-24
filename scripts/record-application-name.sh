#!/bin/sh

window=$(xdotool selectwindow)
window_name=$(xdotool getwindowclassname "$window" || xdotool getwindowname "$window" || echo "game")
gpu-screen-recorder -w "$window" -f 60 -a "$(pactl get-default-sink).monitor" -o "$HOME/Videos/recording/$window_name/$(date +"Video_%Y-%m-%d_%H-%M-%S.mp4")"

#!/bin/sh

pidof -q gpu-screen-recorder && exit 1
video="$HOME/Videos/$(date +"Video_%Y-%m-%d_%H-%M-%S.mp4")"
gpu-screen-recorder -w screen -f 60 -a "$(pactl get-default-sink).monitor" -o "$video"

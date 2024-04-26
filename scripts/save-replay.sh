#!/bin/sh -e

killall -SIGUSR1 gpu-screen-recorder
notify-send -t 1500 -u low -- "GPU Screen Recorder" "Replay saved"

#!/bin/sh

# Enables using colors for stdout.
if test -t 1; then
    # Now check the _number_ of colors.
    NUM_COLORS=$(tput colors)
    if test -n "${NUM_COLORS}" && test ${NUM_COLORS} -ge 8; then
        NORMAL=$(tput sgr0)
        BOLD=$(tput bold)
        UNDERLINE=$(tput smul)
        REVERSE=$(tput smso)
        BLINK=$(tput blink)
        BLACK=$(tput setaf 0)
        RED=$(tput setaf 1)
        GREEN=$(tput setaf 2)
        YELLOW=$(tput setaf 3)
        BLUE=$(tput setaf 4)
        MAGENTA=$(tput setaf 5)
        CYAN=$(tput setaf 6)
        WHITE=$(tput setaf 7)
    fi
fi




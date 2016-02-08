#!/usr/bin/env bash

# Provides an "atexit" mechanism for scripts.

# Array of commands to eval.
declare -a __atexit_cmds

# Helper for eval'ing commands.
__atexit() {
    for cmd in "${__atexit_cmds[@]}"; do
        eval ${cmd}
    done
}

# Usage: atexit command arg1 arg2 arg3
atexit() {
    # Determine the current number of commands.
    local length=${#__atexit_cmds[*]}

    # Add this command to the end.
    __atexit_cmds[${length}]="${*}"

    # Set the trap handler if this was the first command added.
    if [[ ${length} -eq 0 ]]; then
        trap __atexit EXIT
    fi
}

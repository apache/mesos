#!/bin/sh

# Utility script that exec's SSH without key checking so that we can check
# out code from GitHub without prompting the user.

exec ssh -o StrictHostKeyChecking=no $@

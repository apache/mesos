#!/bin/bash

if [ $# -lt 1 ]; then
  echo "Usage: create-swap <amount of MB>"
  exit 1
fi

if [ -e /mnt/swap ]; then
  echo "/mnt/swap already exists" >&2
  exit 1
fi

SWAP_MB=$1
if [[ "$SWAP_MB" != "0" ]]; then
  dd if=/dev/zero of=/mnt/swap bs=1M count=$SWAP_MB
  mkswap /mnt/swap
  swapon /mnt/swap
  echo "Added $SWAP_MB MB swap file /mnt/swap"
fi

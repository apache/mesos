#!/bin/sh
curl http://localhost:8080/ | grep 'RPC port' | sed 's/.*RPC port: \([0-9]*\),.*/\1/' > ports.txt
cat ports.txt

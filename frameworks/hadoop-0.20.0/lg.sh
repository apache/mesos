#!/bin/sh
PORT=$1
time bin/hadoop jar build/hadoop-0.20.1-dev-test.jar loadgen -jt localhost:$PORT -r 0 -keepmap 0.1 -indir rand -outdir out-$RANDOM

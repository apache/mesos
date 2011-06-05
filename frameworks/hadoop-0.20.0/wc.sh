#!/bin/sh
PORT=$1
time bin/hadoop jar build/hadoop-0.20.1-dev-examples.jar wordcount -jt localhost:$PORT randtext2 out-$RANDOM

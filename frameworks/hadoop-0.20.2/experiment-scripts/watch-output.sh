#!/bin/bash
while true; do echo; for i in `seq 0 15`; do echo -n "$i: "; tail -1 $1/$i.txt; done; sleep 3; done

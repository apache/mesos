#!/bin/sh
for i in `seq 1 16`; do echo "Starting JobTracker $i"; (bin/hadoop jobtracker 2>&1) > /mnt/jt$i.txt & sleep 6; done

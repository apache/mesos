#!/bin/bash
outdir=$1
shift
args=("$@")
echo "Placing output in directory $outdir"
mkdir $outdir
for i in `seq 0 $(($#-1))`; do
  port=${args[$i]}
  outfile=$outdir/$i.txt
  echo "Starting loadgen $i, writing to $outfile..."
  (./lg.sh $port 2>&1) >$outfile &
done
wait
echo "All done!"

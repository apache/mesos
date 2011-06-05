#!/bin/sh
jps | grep JobTracke | awk '{print $1}' | xargs kill

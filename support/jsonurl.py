#!/usr/bin/env python

# A utility that expects JSON data at a particular URL and lets you
# recursively extract keys from the JSON object as specified on the
# command line (each argument on the command line after the first will
# be used to recursively index into the JSON object). The name is a
# play off of 'curl'.

import json
import sys
import urllib2

url = sys.argv[1]

data = json.loads(urllib2.urlopen(url).read())

for arg in sys.argv[2:]:
  try:
    temp = data[arg]
    data = temp
  except KeyError:
    print >> sys.stderr, "'" + arg + "' was not found"
    sys.exit(1)

print data

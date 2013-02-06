#!/usr/bin/env python

import json
import sys
import urllib2

url = sys.argv[1]

data = json.loads(urllib2.urlopen(url).read())

for arg in sys.argv[2:]:
  temp = data[arg]
  data = temp

print data

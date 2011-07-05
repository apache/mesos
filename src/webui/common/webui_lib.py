# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from datetime import datetime
import os
import socket
import types

DATE_FORMAT = "%Y-%m-%d %H:%M:%S"

HOSTNAME = os.getenv("MESOS_PUBLIC_DNS")
if HOSTNAME == None:
  HOSTNAME = socket.gethostname()

TASK_STATES = ['STARTING', 'RUNNING', 'FINISHED', 'FAILED', 'KILLED', 'LOST']

def format_time(timestamp):
  if type(timestamp) in [types.IntType, types.LongType, types.FloatType]:
    return datetime.fromtimestamp(timestamp).strftime(DATE_FORMAT)
  else: # Assume it's a datetime object
    return timestamp.strftime(DATE_FORMAT)
  
def format_mem(mbytes):
  UNITS = ["MB", "GB", "TB"]
  num = float(mbytes)
  index = 0
  while num >= 1024 and index < len(UNITS)-1:
    index += 1
    num /= 1024
  return "%.1f %s" % (num, UNITS[index])

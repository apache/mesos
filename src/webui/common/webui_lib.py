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
  if type(timestamp) in [types.IntType, types.LongType]:
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

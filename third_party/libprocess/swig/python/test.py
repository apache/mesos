#!/usr/bin/env python

import pickle

from process import Process, PROCESS_MSGID


PING, PONG, FINISHED = range(PROCESS_MSGID, PROCESS_MSGID + 3)


class Ping(Process):
  def __init__(self, pong):
    Process.__init__(self)
    self.pong = pong

  def __call__(self):
    data = pickle.dumps("hello world")
    self.send(self.pong, PING, data, len(data))
    self.send(self.pong, FINISHED)


class Pong(Process):
  def __init__(self):
    Process.__init__(self)

  def handlePING(self):
    data, length = self.body()
    print pickle.loads(data)
    return True

  def handleFINISHED(self):
    return False

  def __call__(self):
    while {
      PING: lambda: self.handlePING(),
      FINISHED: lambda: self.handleFINISHED()
    }[self.receive()]():
      self.send(self._from(), PONG);


class PickledProcess(Process):
  def __init__(self):
    Process.__init__(self)

  def send(self, pid, msgid, *args):
    data = pickle.dumps(args)
    super(PickledProcess, self).send(pid, msgid, data, len(data))

  def body(self):
    data, length = super(PickledProcess, self).body()
    return pickle.loads(data)


class PickledPing(PickledProcess):
  def __init__(self, pong):
    Process.__init__(self)
    self.pong = pong

  def __call__(self):
    self.send(self.pong, PING, "hello world")
    self.send(self.pong, FINISHED)


class PickledPong(PickledProcess):
  def __init__(self):
    Process.__init__(self)

  def handlePING(self):
    print self.body()
    return True

  def handleFINISHED(self):
    return False

  def __call__(self):
    while {
      PING: lambda: self.handlePING(),
      FINISHED: lambda: self.handleFINISHED()
    }[self.receive()]():
      self.send(self._from(), PONG);


if __name__ == "__main__":
  pong = Process.spawn(PickledPong())
  ping = Process.spawn(PickledPing(pong))
  Process.wait(ping)
  Process.wait(pong)

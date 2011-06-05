#!/usr/bin/env ruby

require 'process'

PING = Process.PROCESS_MSGID
PONG = PING + 1
FINISHED = PONG + 1


class Ping < Process::Process
  def initialize(pong)
    super
    @pong = pong
  end

  def __call__
    send(pong, PING)
    send(pong, FINISHED)
    #GC.start
  end
end


class Pong < Process::Process
  def initialize
    super
  end

  def __call__
    while true do
      id = receive()
      if id == PING
        puts "Pong received PING"
        send(from(), PONG)
      else
        puts "Pong received FINISHED"
        return
      end
    end
  end
end

pong = Process::Process.spawn(Pong.new)
ping = Process::Process.spawn(Ping.new(pong))
Process::Process.wait(pong)
Process::Process.wait(ping)



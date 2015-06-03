# Libprocess User Guide

*Note* This User Guide is Work in Progress.

The library _libprocess_ provides high level elements for an actor programming
style with asynchronous message-handling and a variety of related basic system
primitives. It's API and implementation are written in C++.

## Introduction
The design of libprocess is inspired by [Erlang](http://erlang.org),
a language that implements the
[actor model](http://en.wikipedia.org/wiki/Actor_model).

As the name already suggests, one of the libprocess core concepts is a
[Process](#process). This is a single threaded, independent actor which can
communicate with other processes by sending and receiving [messages](#message).
These are serialized into [Protobuf messages](#protobuf) format and stored in
the recipient process' message buffer, from where its thread can process them
in a serial fashion.

Processes assume they have callers/clients and thus should avoid
blocking at all costs.

A process can be identified symbolically by its [PID](#pid).

Functional composition of different processes enabled by the concept
of a [Future](#future) and a [Promise](#promise). A `Future` is a
read-only placeholder for a result which might be computed
asynchronously, while the Promise on the other side is the writable
placeholder to fullfill the corresponding `Future`. Continutation
allow chaining of futures.

Local messaging between different processes is enabled via the
following concepts: [delay](#delay), [defer](#defer), and
[dispatch](#dispatch). Remote messaging is done via [send](#send),
[route](#route), and [install](#install).

Usually the above mention concepts are applied in comination in
certain pattern. See the [Table of Patterns](#table_pattern) for
details.

<!---
# Subprocess

Resource handling: #owned #shared

Networking: #Network, HTTP, Socket, help

testing: check gmock gtest

Logging, Monitoring, Statistics: #Logging, #Profiler, #System, #Statistics, #Timeseries, #Metrics

Time Concepts: #Clock, #Event, #Filter, #Time, #Timer, #Timeout, #RateLimiter

Cleanup: #Garbarge Collection, #Reaper
> Remove Run.hpp??
--->

## Overview
### Table of Concepts

* <a href="#async">Async</a>
* <a href="#defer">Defer</a>
* <a href="#delay">Delay</a>
* <a href="#dispatch">Dispatch</a>
* <a href="#future">Future</a>
* <a href="#id">Id</a>
* <a href="#pid">PID</a>
* <a href="#process">Process</a>
* <a href="#promise">Promise</a>

<!---
* <a href="#gc">Garbage Collection, Reaper</a>2
* <a href="#dispatch">Dispatch</a>2
* <a href="#event">Event</a>2
* <a href="#help">Help</a>3
* <a href="#http">Network, HTTP, Socket</a>3
* <a href="#id">ID</a>?3
* <a href="#io">IO</a>3
* <a href="#latch">Latch, Mutex</a>3
* <a href="#limiter">Limiter </a>3
* <a href="#logging">Logging</a>2
* <a href="#clock">Clock, Time, Timeout</a>2
* <a href="#gc">Garbage Collection, Reaper</a>2
* <a href="#process">Process, Executor, Once</a>2

* <a href="#profiler">Profiler</a>3
* <a href="#queue">Queue</a>3
* <a href="#statistics">Statistics, Timeseries</a>3
* * <a href="#adress">Adress</a>
* <a href="#async">Async</a>
--->
<a name="table_pattern"/>

### Table of Patterns
* <a href="#clockPattern">Clock Pattern</a>
* <a href="#clockPattern">Timer Pattern</a>
* <a href="#clockPattern">Reaper Pattern</a>

## Concepts

<a name="async"/>
## `async`

...

<a name="defer"/>
## `defer`

Defers a `dispatch` on some process (i.e., a deferred asynchronous
function/method invocation).

~~~{.cpp}
using namespace process;

class SomeProcess : public Process<SomeProcess>
{
public:
  void merge()
  {
    queue.get()
      .then(defer(self(), [] (int i) {
        ...;
      }));
  }

private:
  Queue<int> queue;
};
~~~

`defer` returns a new type (`Deferred<Return(Args)>`) that acts like a
standard function, and an API can force asynchronous callback
semantics by requiring that type.


<a name="delay"/>
## `delay`

...


<a name="dispatch"/>
## `dispatch`

`dispatch` performs asynchronous function (method) invocation (think
Erlangs `gen_server cast`, but no `call` analogue).


~~~{.cpp}
using namespace process;

class QueueProcess : public Process<QueueProcess>
{
public:
  void enqueue(int i) { this->i = i; }
  int dequeue() { return this->i; }

private:
  int i;
};


int main(int argc, char** argv)
{
  QueueProcess process;
  spawn(process);

  dispatch(process, &QueueProcess::enqueue, 42);
  dispatch(process, &QueueProcess::enqueue, 43);

  ...;
}
~~~


<a name="future"/>
## `Future`

...

The counterpart on the producer side is a [Promise](#promise).


<a name="id"/>
## ID

Generates a unique identifier string given a prefix. This is used to
provide `PID` names.


<a name="pid"/>
## `PID`

A `PID` provides a level of indirection for naming a process without
having an actual reference (pointer) to it (necessary for remote
processes).

~~~{.cpp}
using namespace process;

int main(int argc, char** argv)
{
  QueueProcess process;
  spawn(process);

  PID<QueueProcess> pid = process.self();

  dispatch(pid, &QueueProcess:enqueue, 42);

  terminate(pid);
  wait(pid);

  return 0;
}
~~~


<a name="process"/>
## `Process`

A process is an actor, effectively a cross between a thread and an object.

Creating/spawning a process is very cheap (no actual thread gets
created, and no thread stack gets allocated).

Each process has a queue of incoming events that it processes one at a
time.

Processes provide execution contexts (only one thread executing within
a process at a time so no need for per process synchronization).

~~~{.cpp}
using namespace process;

class MyProcess : public Process<MyProcess> {};

int main(int argc, char** argv)
{
  MyProcess process;
  spawn(process);
  terminate(process);
  wait(process);
  return 0;
}
~~~


<a name="promise"/>
## `Promise`

...

~~~{.cpp}
using namespace process;

template <typename T>
class QueueProcess : public Process<QueueProcess<T>>
{
public:
  Future<T> dequeue()
  {
    return promise.future();
  }

  void enqueue(T t)
  {
    promise.set(t);
  }

private:
  Promise<T> promise;
};


int main(int argc, char** argv)
{
  ...;

  Future<int> i = dispatch(process, &QueueProcess<int>::dequeue);

  dispatch(process, &QueueProcess<int>::enqueue, 42);

  i.await();

  ...;
}
~~~


<a name="route"/>
## `route`

~~~{.cpp}
using namespace process;
using namespace process::http;

class QueueProcess : public Process<QueueProcess>
{
public:
  QueueProcess() : ProcessBase("queue") {}

  virtual void initialize() {
    route("/enqueue", [] (Request request)
    {
      // Parse argument from 'request.query' or 'request.body.
      enqueue(arg);
      return OK();
    });
  }
};

// $ curl localhost:1234/queue/enqueue?value=42
~~~


<a name="then"/>
## `Future::then`

~~~{.cpp}
using namespace process;

int main(int argc, char** argv)
{
  ...;

  Future<int> i = dispatch(process, &QueueProcess<int>::dequeue);

  dispatch(process, &QueueProcess<int>::enqueue, 42);

  i.then([] (int i) {
    // Use 'i'.
  });

  ...;
}
~~~

When future is completed, callbacks get invoked.

<!---
Explain:
(1) When should a callback get invoked?
(2) Using what execution context?
       Synchronously: using the current thread, blocking whatever was
       Asynchronously: using a different thread than the current thread (but what thread?)?
--->


## Pattern/Examples

...


## Building

...


### Dependencies
> NOTE: Libprocess requires the following third party libraries: ...

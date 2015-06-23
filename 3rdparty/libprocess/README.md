# Libprocess Developer Guide

*Note* This Developer Guide is Work in Progress.

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
in a serial fashion. To always be responsive processes should avoid blocking at all costs.

A process can be identified symbolically by its [PID](#pid).

Basic communication between processes is supported by [send](#send),
[route](#route), and [install](#install).

At a higher level, functional composition of interactions between processes is facilitated by the concept
of a [Future](#future) and a [Promise](#promise). A `Future` is a
read-only placeholder for a result which might be computed
asynchronously. A Promise on the other side is a
handle providing write access to a referenced `Future`.
The following primitives compose closures with future results: [delay](#delay), [defer](#defer), and [dispatch](#dispatch). This gives rise to the pattern of [future chaining](#futureChaining).

This is one of the major [patterns](#table_pattern) which combine the [main concepts](#table_concepts) of libprocess.


## Overview
### Table of Concepts

<a name="table_concepts"/>
* <a href="#async">Async</a>
* <a href="#defer">Defer</a>
* <a href="#delay">Delay</a>
* <a href="#dispatch">Dispatch</a>
* <a href="#future">Future</a>
* <a href="#id">ID</a>
* <a href="#pid">PID</a>
* <a href="#process">Process</a>
* <a href="#promise">Promise</a>

<!---
* <a href="#gc">Garbage Collection, Reaper</a>
* <a href="#dispatch">Dispatch</a>
* <a href="#event">Event</a>
* <a href="#help">Help</a>
* <a href="#http">Network, HTTP, Socket</a>
* <a href="#io">IO</a>
* <a href="#latch">Latch, Mutex</a>
* <a href="#limiter">Limiter </a>
* <a href="#logging">Logging</a>
* <a href="#clock">Clock, Time, Timeout</a>
* <a href="#gc">Garbage Collection, Reaper</a>
* <a href="#process">Process, Executor, Once</a>

* <a href="#profiler">Profiler</a>
* <a href="#queue">Queue</a>
* <a href="#statistics">Statistics, Timeseries</a>
* <a href="#adress">Adress</a>
--->

<a name="table_pattern"/>

### Table of Patterns
* <a href="#futureChaining">Future Chaining</a>
* <a href="#clockPattern">Clock Pattern</a>
* <a href="#timerPattern">Timer Pattern</a>
* <a href="#reaperPattern">Reaper Pattern</a>

## Concepts

<a name="async"/>
## `async`
Async defines a function template for asynchronously executing function closures. It provides their results as [Futures](#future).

<a name="defer"/>
## `defer`

Defer creates a deferred [dispatch](#dispatch).

<!---
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
--->



<a name="delay"/>
## `delay`
Delay instead of [dispatching](#dispatch) for execution right away, it allows it to be scheduled after a certain time duration.



<a name="dispatch"/>
## `dispatch`
Dispatch schedules a method for asynchronous execution.

<!---
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
--->


<a name="future"/>
## `Future`

The libprocess futures mimic futures in other languages like Scala. It is a placeholder for a future value which is not (necessarily) ready yet. A future in libprocess is a C++ template which is specialized for the return type, for example Try. A future can either be: ready (carrying along a value which can be extracted with .get()), failed (in which case .error() will encode the reason for the failure) or discarded.

Futures can be created in numerous ways: awaiting the result of a method call with [defer](#defer), [dispatch](#dispatch), and [delay](#delay) or as the read-end of a [promise](#promise).



<a name="id"/>
## `ID`

Generates a unique identifier string given a prefix. This is used to
provide `PID` names.


<a name="pid"/>
## `PID`

A PID provides a level of indirection for naming a process without
having an actual reference (pointer) to it (necessary for remote
processes).

<!---
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
--->

<a name="process"/>
## `Process`

A process is an actor, effectively a cross between a thread and an object.

Creating/spawning a process is very cheap (no actual thread gets
created, and no thread stack gets allocated).

Each process has a queue of incoming events that it processes one at a
time.

Processes provide execution contexts (only one thread executing within
a process at a time so no need for per process synchronization).

<!---
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
--->

<a name="promise"/>
## `Promise`

A promise is an object that can fulfill a [futures](#future), i.e. assign a result value to it.

<!---
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
--->

<a name="route"/>
## `route`

Route installs an http endpoint onto a process.

<!---
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
--->

<a name="then"/>
## `Future::then`
Then allows to invoke callbacks once a future is completed.

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



<!---
Explain:
(1) When should a callback get invoked?
(2) Using what execution context?
       Synchronously: using the current thread, blocking whatever was
       Asynchronously: using a different thread than the current thread (but what thread?)?
--->


## Pattern/Examples

<a name="futureChaining" />
<a name="clockPattern" />
<a name="timerPattern" />
<a name="reaperPattern" />

## Building Libprocess


## Dependencies

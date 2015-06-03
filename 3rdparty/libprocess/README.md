# Libprocess User Guide

*Note* This User Guide is Work in Progress.

The library _libprocess_ provides high level elements for an actor programming
style with asynchronous message-handling and a variety of related basic system
primitives. It's API and implementation are written in C++.

##Introduction
The design of libprocess is inspired by [Erlang](http://erlang.org),
a language that implements the
[actor model](http://en.wikipedia.org/wiki/Actor_model).

As the name already suggests, one of the libprocess core concepts is a
[Process](#process). This is a single threaded, independent actor which can
communicate with other processes by sending and receiving [messages](#message).
These are serialized into [Protobuf messages](#protobuf) format and stored in
the recipient process' message buffer, from where its thread can process them
in a serial fashion.

processes assume they have callers/clients and thus should avoid blocking at all costs

A process can be identified symbolically by its [PID](#pid).

Functional composition of different processes enabled by the concept of [Futures](#future) and [Promises](#promise)
A future is a read-only placeholder for a result which might be computed asynchronously, while the [promise](#promise) on the other side is the writable placeholder to fullfill the corresponding future.
Continutation allow chaining of futures.

Local messaging between different processes is enabled via the following concepts: [delay](#delay), [defere](defere), [async](#async), [sequence](#sequence,), and [dispatch](#dispatch).
Furthermore libprocess allows for remote messaging via [send](#send), [route](#route), and [install](#install).

Usually the above mention concepts are applied in comination in certain pattern. See the [Table of Patterns](#table_pattern) for details.
<!---
#Subprocess

Resource handling: #owned #shared

Networking: #Network, HTTP, Socket, help

testing: check gmock gtest

Logging, Monitoring, Statistics: #Logging, #Profiler, #System, #Statistics, #Timeseries, #Metrics

Time Concepts: #Clock, #Event, #Filter, #Time, #Timer, #Timeout, #RateLimiter

Cleanup: #Garbarge Collection, #Reaper
> Remove Run.hpp??
--->

##Overview
###Table of Concepts

* <a href="#async">Async</a>
* <a href="#defer">Defer</a>
* <a href="#delay">Delay</a>
* <a href="#dispatch">Dispatch</a>
* <a href="#future">Future</a>
* <a href="#id">Id</a>
* <a href="#pid">PID</a>
* <a href="#process">Process</a>
* <a href="#promise">Promise</a>
* <a href="#protobuf">Protobuf</a>
* <a href="#sequence">Sequence</a>

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
* <a href="#sequence">Sequence</a>3
* <a href="#statistics">Statistics, Timeseries</a>3
* * <a href="#adress">Adress</a>
* <a href="#async">Async</a>
--->
<a name="table_pattern"/>

###Table of Patterns
* <a href="#clockPattern">Clock Pattern</a>
* <a href="#clockPattern">Timer Pattern</a>
* <a href="#clockPattern">Reaper Pattern</a>

##Concepts

<a name="async"/>
## `Async`

<a name="defer"/>
## `Defer`
defers a dispatch on some process (i.e., a deferred asynchronous function/method invocation)
```
class SomeProcess : public Process<SomeProcess> {
public:
  void merge() {
    queue.get()
      .then(defer(self(), [] (int i) {
        â€¦;
      }));
  }

private:
  Queue<int> queue;
};
```
defer returns a new type (Deferred<Return(Argsâ€¦)>) that acts like a standard function, and an API can force asynchronous callback semantics by requiring that typedefer returns a new type (Deferred<Return(Argsâ€¦)>) that acts like a standard function, and an API can force asynchronous callback semantics by requiring that type


<a name="delay"/>
## `Delay`

<a name="dispatch"/>
dispatch â‰ˆ message passing
asynchronous function (method) invocation
(think Erlangâ€™s gen_server cast, but no call analogue â€¦ avoid blocking operations!)

## `Dispatch`
```
class QueueProcess : public Process<QueueProcess> {
public:
  void enqueue(int i) { this->i = i; }
  int dequeue() { return this->i; }

private:
  int i;
};


int main(int argc, char** argv) {
  QueueProcess process;
  spawn(process);

  dispatch(process, &QueueProcess::enqueue, 42);
  dispatch(process, &QueueProcess::enqueue, 43);

  â€¦;
}
```

<a name="future"/>
## `Future`
A Future .... The counterpart on the producer side is a [Promise](#promise)

<a name="id"/>
## `ID`
The [namespace ID](@ref process::ID) contains
[a function](@ref process::ID::generate)
that generates unique ID strings given a prefix. This is used in the following to provide PID names.

<a name="pid"/>
## `PID`
A PID provides a level of indirection for naming a process without having an actual reference (pointer) to it (necessary for remote processes)
```
int main(int argc, char** argv) {
  QueueProcess process;
  spawn(process);

  PID<QueueProcess> pid = process.self();

  dispatch(pid, &QueueProcess:enqueue, 42);

  terminate(pid);
  wait(pid);

  return 0;
}
```
<a name="process"/>
## `Process`
process â‰ˆ thread
creating/spawning a process is very cheap (no actual thread gets created, and no thread stack gets allocated)
process â‰ˆ object
process â‰ˆ actor

each process has a â€œqueueâ€ of incoming â€œmessagesâ€
processes provide execution contexts (only one thread executing within a process at a time so no need for per process synchronization)
Lifecycle
```
using namespace process;

class MyProcess : public Process<MyProcess> {};

int main(int argc, char** argv) {
  MyProcess process;
  spawn(process);
  terminate(process);
  wait(process);
  return 0;
}
```

<a name="promise"/>

## `Promise`
```
template <typename T>
class QueueProcess : public Process<QueueProcess<T>> {
public:
  Future<T> dequeue() {
    return promise.future();
  }

  void enqueue(T t) {
    promise.set(t);
  }

private:
  Promise<T> promise;
};

int main(int argc, char** argv) {
  â€¦;

  Future<int> i = dispatch(process, &QueueProcess<int>::dequeue);

  dispatch(process, &QueueProcess<int>::enqueue, 42);

  i.await();

  â€¦;
}
```
tells caller they might need to wait â€¦ but if nobody should block, how do you wait?
Link to then


<a name="protobuf"/>
## `Protobuf`

<a name="route"/>
## `route`
```
using namespace process;
using namespace process::http;


class QueueProcess : public Process<QueueProcess> {
public:
  QueueProcess() : ProcessBase(â€œqueueâ€) {}

  virtual void initialize() {
    route(â€œ/enqueueâ€, [] (Request request) {
      // Parse argument from â€˜request.queryâ€™ or â€˜request.bodyâ€™.
      enqueue(arg);
      return OK();
    });
  }
};
```
curl localhost:1234/queue/enqueue?value=42

<a name="sequence"/>
## `Sequence`

<a name="then"/>
## `Then`
```
int main(int argc, char** argv) {
  â€¦;

  Future<int> i = dispatch(process, &QueueProcess<int>::dequeue);

  dispatch(process, &QueueProcess<int>::enqueue, 42);

  i.then([] (int i) {
    // Use â€˜iâ€™.
  });

  â€¦;
}
```
when future is satisfied, callbacks should be invoked â€¦
(1) when should a callback get invoked?
(2) using what execution context?
synchronously: using the current thread, blocking whatever was currently executing
asynchronously: using a different thread than the current thread (but what thread?)

Link to defere <a href="#defer">Defer</a>

<!---
The `Option` type provides a safe alternative to using `NULL`. An `Option` can be constructed explicitely or implicitely:

```
    Option<bool> o(true);
    Option<bool> o = true;
```

You can check if a value is present using `Option::isSome()` and `Option::isNone()` and retrieve the value using `Option::get()`:

```
    if (!o.isNone()) {
      ... o.get() ...
    }
```

Note that the current implementation *copies* the underlying values (see [Philosophy](#philosophy) for more discussion). Nothing prevents you from using pointers, however, *the pointer will not be deleted when the Option is destructed*:

```
    Option<std::string*> o = new std::string("hello world");
```

The `None` type acts as "syntactic sugar" to make using [Option](#option) less verbose. For example:

```
    Option<T> foo(const Option<T>& o) {
      return None(); // Can use 'None' here.
    }

    ...

    foo(None()); // Or here.

    ...

    Option<int> o = None(); // Or here.
```

Similar to `None`, the `Some` type can be used to construct an `Option` as well. In most circumstances `Some` is unnecessary due to the implicit `Option` constructor, however, it can still be useful to remove any ambiguities as well as when embedded within collections:

```
    Option<Option<std::string>> o = Some("42");

    std::map<std::string, Option<std::string>> values;
    values["value1"] = None();
    values["value2"] = Some("42");
```
--->
##Pattern/Examples

##Building

###Dependencies
> NOTE: Libprocess requires the following third party libraries:

# Libprocess Developer Guide

*Note* This Developer Guide is Work in Progress.

The library _libprocess_ provides high level elements for an
actor programming style with asynchronous message-handling and a
variety of related basic system primitives. Its API and
implementation are written in C++.


## Introduction

The design of libprocess is inspired by [Erlang](http://erlang.org),
a language that implements the
[actor model](http://en.wikipedia.org/wiki/Actor_model).

As the name already suggests, one of the libprocess core concepts is a
[Process](#process). This is a single threaded, independent actor which
communicates with other processes, locally and remotely, by sending and receiving [HTTP requests and responses](#http).

At a higher level, functional composition of processes is facilitated using [futures and promises](#futures-and-promises).


## Overview

### Table of Contents

* [Processes and the Asynchronous Pimpl Pattern](#processes)
* [Futures and Promises](#futures-and-promises)
* [HTTP](#http)
* [Testing](#testing)
* [Miscellaneous Primitives](#miscellaneous-primitives)

---

## Processes and the Asynchronous Pimpl Pattern

A `process` is an actor, effectively a cross between a thread and an object.

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
---->

### `delay`

`delay` instead of [dispatching](#dispatch) for execution right away, it allows it to be scheduled after a certain time duration.


### `dispatch`

`dispatch` schedules a method for asynchronous execution.

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
---->


### `ID`

Generates a unique identifier string given a prefix. This is used to
provide `PID` names.


### `PID`

A `PID` provides a level of indirection for naming a process without
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
---->

> NOTE: In most cases, a process should have a destructor that terminates
> and waits on the process to finish.  The process does not automatically
> terminate when the `Process` is deallocated.  i.e.:

~~~{.cpp}
class MyProcess : public Process<MyProcess> {
  virtual ~MyProcess()
  {
    terminate(this);
    wait(this);
  }
};
~~~

---

## Futures and Promises

The `Future` and `Promise` primitives are used to enable
programmers to write asynchronous, non-blocking, and highly
concurrent software.

A `Future` acts as the read-side of a result which might be
computed asynchronously. A `Promise`, on the other hand, acts
as the write-side "container". We'll use some examples to
explain the concepts.

First, you can construct a `Promise` of a particular type by
doing the following:

~~~{.cpp}
using namespace process;

int main(int argc, char** argv)
{
  Promise<int> promise;

  return 0;
}
~~~

A `Promise` is not copyable or assignable, in order to encourage
strict ownership rules between processes (i.e., it's hard to
reason about multiple actors concurrently trying to complete a
`Promise`, even if it's safe to do so concurrently).

You can get a `Future` from a `Promise` using the
`Promise::future()` method:

~~~{.cpp}
using namespace process;

int main(int argc, char** argv)
{
  Promise<int> promise;

  Future<int> future = promise.future();

  return 0;
}
~~~

Note that the templated type of the future must be the exact
same as the promise: you cannot create a covariant or
contravariant future. Unlike `Promise`, a `Future` can be both
copied and assigned:

~~~{.cpp}
using namespace process;

int main(int argc, char** argv)
{
  Promise<int> promise;

  Future<int> future = promise.future();

  // You can copy a future.
  Future<int> future2 = future;

  // You can also assign a future (NOTE: this future will never
  // complete because the Promise goes out of scope, but the
  // Future is still valid and can be used normally.)
  future = Promise<int>().future();

  return 0;
}
~~~

The result encapsulated in the `Future`/`Promise` can be in one
of four states: `PENDING`, `READY`, `FAILED`, `DISCARDED`. When
a `Promise` is first created the result is `PENDING`. When you
complete a `Promise` using the `Promise::set()` method the
result becomes `READY`:

~~~{.cpp}
using namespace process;

int main(int argc, char** argv)
{
  Promise<int> promise;

  Future<int> future = promise.future();

  promise.set(42);

  CHECK(future.isReady());

  return 0;
}
~~~

> NOTE: `CHECK` is a macro from `gtest` which acts like an
> `assert` but prints a stack trace and does better signal
> management. In addition to `CHECK`, we've also created
> wrapper macros `CHECK_PENDING`, `CHECK_READY`, `CHECK_FAILED`,
> `CHECK_DISCARDED` which enables you to more concisely do
> things like `CHECK_READY(future)` in your code. We'll use
> those throughout the rest of this guide.

TODO(benh):
* Using `Future` and `Promise` between actors, i.e., `dispatch` returning a `Future`
* `Promise::fail()`
* `Promise::discard()` and `Future::discard()`
* `Future::onReady()`, `Future::onFailed()`, `Future::onDiscarded()`
* `Future::then()`, `Future::repair()`, `Future::after`
* `defer`
* `Future::await()`

<!--
#### `Future::then`

`Future::then` allows to invoke callbacks once a future is completed.

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


### `Promise`

A `promise` is an object that can fulfill a [futures](#future), i.e. assign a result value to it.


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


### `defer`

`defer` allows the caller to postpone the decision whether to [dispatch](#dispatch) something by creating a callable object which can perform the dispatch at a later point in time.

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
-->

## `HTTP`

libprocess provides facilities for communicating between actors via HTTP
messages. With the advent of the HTTP API, HTTP is becoming the preferred mode
of communication.

### `route`

`route` installs an HTTP endpoint onto a process. Let's define a simple process
that installs an endpoint upon initialization:

~~~{.cpp}
using namespace process;
using namespace process::http;

class HttpProcess : public Process<HttpProcess>
{
protected:
  virtual void initialize()
  {
    route("/testing", None(), [](const Request& request) {
      return testing(request.query);
    });
  }
};


class Http
{
public:
  Http() : process(new HttpProcess())
  {
    spawn(process.get());
  }

  virtual ~Http()
  {
    terminate(process.get());
    wait(process.get());
  }

  Owned<HttpProcess> process;
};
~~~

Now if our program instantiates this class, we can do something like:
`$ curl localhost:1234/testing?value=42`

Note that the port at which this endpoint can be reached is the port libprocess
has bound to, which is determined by the `LIBPROCESS_PORT` environment variable.
In the case of the Mesos master or agent, this environment variable is set
according to the `--port` command-line flag.

### `get`

`get` will hit an HTTP endpoint with a GET request and return a `Future`
containing the response. We can pass it either a libprocess `UPID` or a `URL`.
Here's an example hitting the endpoint assuming we have a `UPID` named `upid`:

~~~{.cpp}
Future<Response> future = get(upid, "testing");
~~~

Or let's assume our serving process has been set up on a remote server and we
want to hit its endpoint. We'll construct a `URL` for the address and then call
`get`:

~~~{.cpp}
URL url = URL("http", "hostname", 1234, "/testing");

Future<Response> future = get(url);
~~~

### `post` and `requestDelete`

The `post` and `requestDelete` functions will similarly send POST and DELETE
requests to an HTTP endpoint. Their invocation is analogous to `get`.

### `Connection`

A `Connection` represents a connection to an HTTP server. `connect`
can be used to connect to a server, and returns a `Future` containing the
`Connection`. Let's open a connection to a server and send some requests:

~~~{.cpp}
Future<Connection> connect = connect(url);

connect.await();

Connection connection = connect.get();

Request request;
request.method = "GET";
request.url = url;
request.body = "Amazing prose goes here.";
request.keepAlive = true;

Future<Response> response = connection.send(request);
~~~

It's also worth noting that if multiple requests are sent in succession on a
`Connection`, they will be automatically pipelined.

## Clock Management and Timeouts

Asynchronous programs often use timeouts, e.g., because a process that initiates
an asynchronous operation wants to take action if the operation hasn't completed
within a certain time bound. To facilitate this, libprocess provides a set of
abstractions that simplify writing timeout logic. Importantly, test code has the
ability to manipulate the clock, in order to ensure that timeout logic is
exercised (without needing to block the test program until the appropriate
amount of system time has elapsed).

To invoke a function after a certain amount of time has elapsed, use `delay`:

~~~{.cpp}
using namespace process;

class DelayedProcess : public Process<DelayedProcess>
{
public:
  void action(const string& name)
  {
    LOG(INFO) << "hello, " << name;

    promise.set(Nothing());
  }

  Promise<Nothing> promise;
};


int main()
{
  DelayedProcess process;

  spawn(process);

  LOG(INFO) << "Starting to wait";

  delay(Seconds(5), process.self(), &DelayedProcess::action, "Neil");

  AWAIT_READY(process.promise.future());

  LOG(INFO) << "Done waiting";

  terminate(process);
  wait(process);

  return 0;
}
~~~

This invokes the `action` function after (at least) five seconds of time
have elapsed. When writing unit tests for this code, blocking the test for five
seconds is undesirable. To avoid this, we can use `Clock::advance`:

~~~{.cpp}

int main()
{
  DelayedProcess process;

  spawn(process);

  LOG(INFO) << "Starting to wait";

  Clock::pause();

  delay(Seconds(5), process.self(), &DelayedProcess::action, "Neil");

  Clock::advance(Seconds(5));

  AWAIT_READY(process.promise.future());

  LOG(INFO) << "Done waiting";

  terminate(process);
  wait(process);

  Clock::resume();

  return 0;
}
~~~


## Miscellaneous Primitives

### `async`

Async defines a function template for asynchronously executing function closures. It provides their results as [futures](#futures-and-promises).

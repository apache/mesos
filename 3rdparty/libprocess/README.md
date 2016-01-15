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


### `defer`

Objects like `Future` allow attaching callbacks that get executed
_synchronously_ on certain events, such as the completion of a future
(e.g., `Future::then` and `Future::onReady`) or the failure of a
future (e.g., `Future::onFailed`). It's usually desireable, however, to
execute these callbacks _asynchronously_, and `defer` provides a mechanism
to do so.

`defer` is similar to [`dispatch`](#dispatch), but rather than
enqueing the execution of a method or function on the specified
process immediately (i.e., synchronously), it returns a `Deferred`,
which is a callable object that only after getting _invoked_ will
dispatch the method or function on the specified process. Said another
way, using `defer` is a way to _defer_ a `dispatch`.

As an example, consider the following function, which spawns a process
and registers two callbacks on it, one using `defer` and another
without it:

~~~{.cpp}
using namespace process;

void foo()
{
  ProcessBase process;
  spawn(process);

  Deferred<void(int)> deferred = defer(
      process,
      [](int i) {
        // Invoked _asynchronously_ using `process` as the
        // execution context.
      });

  Promise<int> promise;

  promise.future().then(deferred);

  promise.future().then([](int i) {
    // Invoked synchronously from the execution context of
    // the thread that completes the future!
  });

  // Executes both callbacks synchronously, which _dispatches_
  // the deferred lambda to run asynchronously in the execution
  // context of `process` but invokes the other lambda immediately.
  promise.set(42);

  terminate(process);
}
~~~

As another example, consider this excerpt from the Mesos project's
`src/master/master.cpp`:

~~~{.cpp}
// Start contending to be a leading master and detecting the current leader.
// NOTE: `.onAny` passes the relevant future to its callback as a parameter, and
// `lambda::_1` facilitates this when using `defer`.
contender->contend()
  .onAny(defer(self(), &Master::contended, lambda::_1));
~~~

Why use `defer` in this context rather than just executing
`Master::detected` synchronously? To answer this, we need to remember
that when the promise associated with the future returned from
`contender->contend()` is completed that will synchronously invoke all
registered callbacks (i.e., the `Future::onAny` one in the example
above), _which may be in a different process_! Without using `defer`
the process responsible for executing `contender->contend()` will
potentially cause `&Master::contended` to get executed simultaneously
(i.e., on a different thread) than the `Master` process! This creates
the potential for a data race in which two threads access members of
`Master` concurrently. Instead, using `defer` (with `self()`) will
dispatch the method _back_ to the `Master` process to be executed at a
later point in time within the single-threaded execution context of
the `Master`. Using `defer` here precisely allows us to capture these
semantics.

A natural question that folks often ask is whether or not we ever
_don't_ want to use `defer(self(), ...)`, or even just 'defer`. In
some circumstances, you actually don't need to defer back to your own
process, but you often want to defer. A good example of that is
handling HTTP requests. Consider this example:

~~~{.cpp}
using namespace process;

using std::string;

class HttpProcess : public Process<HttpProcess>
{
public:
  virtual void initialize()
  {
    route("/route", None(), [](const http::Request& request) {
      return functionWhichReturnsAFutureOfString()
        .then(defer(self(), [](const string& s) {
          return http::OK("String returned in body: " + s);
        }));
    });
  }
};
~~~

Now, while this is totally legal and correct code, the callback
executed after `functionWhichReturnsAFutureOfString` is completed
_does not_ need to be executed within the execution context of
`HttpProcess` because it doesn't require any state from `HttpProcess`!
In this case, rather than forcing the execution of the callback within
the execution context of `HttpProcess`, which will block other
callbacks _that must_ be executed by `HttpProcess`, we can simply just
run this lambda using an execution context that libprocess can pick
for us (from a pool of threads). We do so by removing `self()` as the
first argument to `defer`:

~~~{.cpp}
using namespace process;

using std::string;

class HttpProcess : public Process<HttpProcess>
{
public:
  virtual void initialize()
  {
    route("/route", None(), [](const http::Request& request) {
      return functionWhichReturnsAFutureOfString()
        .then(defer([](const string& s) {
          return http::OK("String returned in body: " + s);
        }));
    });
  }
};
~~~

_Note that even in this example_ we still want to use `defer`! Why?
Because otherwise we are blocking the execution context (i.e.,
process, thread, etc) that is _completing_ the future because it is
synchronously executing the callbacks! Instead, we want to let the
callback get executed asynchronously by using `defer.`

Let's construct a simple example that illustrates a problem that can
be introduced by omitting `defer` from callback registrations. We'll
define a method for our `HttpProcess` class which accepts an input
string and returns a future via an asyncronous method called
`asyncStoreData`:

~~~{.cpp}
using namespace process;

using std::string;

class HttpProcess : public Process<HttpProcess>
{
public:
  // Returns the number of bytes stored.
  Future<int> inputHandler(const string input);

protected:
  // Returns the number of bytes stored.
  Future<int> asyncStoreData(const string input);

  int storedCount;
}


Future<int> HttpProcess::inputHandler(const string input)
{
  LOG(INFO) << "HttpProcess received input: " << input;

  return asyncStoreData(input)
    .then([this](int bytes) -> Future<int> {
      this->storedCount += bytes;

      LOG(INFO) << "Successfully stored input. "
                << "Total bytes stored so far: " << this->storedCount;

      return bytes;
    });
}
~~~

When the callback is registered on the `Future<int>` returned by
`asyncStoreData`, a lambda is passed directly to `then`. This means
that the lambda will be executed in whatever execution context
eventually fulfills the future. If the future is fulfilled in a
different execution context (i.e., inside a different libprocess
process), then it's possible that the instance of `HttpProcess` that
originally invoked `inputHandler` will have been destroyed, making
`this` a dangling pointer. In order to avoid this possibility, the
callback should be registered as follows:

~~~{.cpp}
  return asyncStoreData(input)
    .then(defer(self(), [this](int bytes) -> Future<int> {
      ...
    }));
~~~

The lambda is then guaranteed to execute within the execution context
of the current process, and we know that `this` will still be a valid
pointer. We should write libprocess code that makes no assumptions
about the execution context in which a given future is fulfilled. Even
if we can verify that a future will be fulfilled in the current
process, registering a callback without `defer` makes the code more
fragile by allowing the possibility that another contributor will make
changes without considering the impact of those changes on the
registered callbacks' execution contexts. Thus, `defer` should always
be used. We offer the following rule to determine which form of
`defer` should be used in a given situation:

* If the callback being registered accesses the state of a process,
  then it should be registered using `defer(pid, callback)`, where
  `pid` is the PID of the process whose state is being accessed.
* If the callback doesn't access any process state, or only makes use
  of process variables that are captured _by value_ so that the
  context of the process is not directly accessed when the callback is
  executed, then it can be run in an arbitrary execution context
  chosen by libprocess, and it should be registered using
  `defer(callback)`.


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

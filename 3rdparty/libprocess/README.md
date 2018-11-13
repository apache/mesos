# Libprocess User Guide

libprocess provides general primitives and abstractions for asynchronous programming with [futures/promises](https://en.wikipedia.org/wiki/Futures_and_promises), [HTTP](https://en.wikipedia.org/wiki/Hypertext_Transfer_Protocol), and [actors](http://en.wikipedia.org/wiki/Actor_model).

> <br> **Inspired by [Erlang](http://erlang.org), libprocess gets it's name from calling an "actor" a "process" (not to be confused by an operating system process).** <br><br>

## Table of Contents

* [Presentations](#presentations)
* [Overview](#overview)
* [Futures and Promises](#futures-and-promises)
* [HTTP](#http)
* [Processes (aka Actors)](#processes)
* [Clock Management and Timeouts](#clock)
* [Miscellaneous Primitives](#miscellaneous-primitives)
* [Optimized Run Queue and Event Queue](#optimized-run-queue-event-queue)

---

## <a name="presentations"></a> Presentations

The following talks are recommended to get an overview of libprocess:

* [libprocess, a concurrent and asynchronous programming library (San Francisco C++ Meetup)](https://www.youtube.com/watch?v=KjqaZYP0T2U)


## <a name="overview"></a> Overview

This user guide is meant to help understand the constructs within the libprocess library. The main constructs are:

1. [Futures and Promises](#futures-and-promises) which are used to build ...
2. [HTTP](#http) abstractions, which make the foundation for ...
3. [Processes (aka Actors)](#processes).

For most people processes (aka actors) are the most foreign of the concepts, but they are arguably the most critical part of the library (they library is named after them!). Nevertheless, we organized this guide to walk through futures/promises and HTTP before processes because the former two are prerequisites for the latter.

## <a name="futures-and-promises"></a> Futures and Promises

The `Future` and `Promise` primitives are used to enable programmers to write asynchronous, non-blocking, and highly concurrent software.

A `Future` acts as the read-side of a result which might be computed asynchronously. A `Promise`, on the other hand, acts as the write-side "container".

Looking for a specific topic?

* [Basics](#futures-and-promises-basics)
* [States](#futures-and-promises-states)
* [Disarding a Future (aka Cancellation)](#futures-and-promises-discarding-a-future)
* [Abandoned Futures](#futures-and-promises-abandoned-futures)
* [Composition: `Future::then()`, `Future::repair()`, and `Future::recover()`](#futures-and-promises-composition)
* [Discarding and Composition](#futures-and-promises-discarding-and-composition)
* [Callback Semantics](#futures-and-promises-callback-semantics)
* [`CHECK()` Overloads](#futures-and-promises-check-overloads)

### <a name="futures-and-promises-basics"></a> Basics

A `Promise` is templated by the type that it will "contain". A `Promise` is not copyable or assignable in order to encourage strict ownership rules between processes (i.e., it's hard to reason about multiple actors concurrently trying to complete a `Promise`, even if it's safe to do so concurrently).

You can get a `Future` from a `Promise` using `Promise::future()`. Unlike `Promise`, a `Future` can be both copied and assigned.

> <br> As of this time, the templated type of the future must be the exact same as the promise: you cannot create a covariant or contravariant future. <br><br>

Here is a simple example of using `Promise` and `Future`:

```cpp
using process::Future;
using process::Promise;

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
```

### <a name="futures-and-promises-states"></a> States

A promise starts in the `PENDING` state and can then transition to any of the `READY`, `FAILED`, or `DISCARDED` states. You can check the state using `Future::isPending()`, `Future::isReady()`, `Future::isFailed()`, and `Future::isDiscarded()`.

> <br> We typically refer to transitioning to `READY` as _completing the promise/future_. <br><br>

You can also add a callback to be invoked when (or if) a transition occurs (or has occcured) by using the `Future::onReady()`, `Future::onFailed()`, and `Future::onDiscarded()`. As a _catch all_ you can use `Future::onAny()` which will invoke it's callbacks on a transition to all of `READY`, `FAILED`, and `DISCARDED`. **See [Callback Semantics](#futures-and-promises-callback-semantics) for a discussion of how/when these callbacks get invoked.**

The following table is meant to capture these transitions:

| Transition  | `Promise::*()`                      | `Future::is*()`         | `Future::on*()`            |
| ----------- | ----------------------------------- | ----------------------- | -------------------------- |
| `READY`     | `Promise::set(T)`                   | `Future::isReady()`     | `Future::onReady(F&&)`     |
| `FAILED`    | `Promise::fail(const std::string&)` | `Future::isFailed()`    | `Future::onFailed(F&&)`    |
| `DISCARDED` | `Promise::discard()`                | `Future::isDiscarded()` | `Future::onDiscarded(F&&)` |

> <br> Code Style: prefer [composition](#futures-and-promises-composition) using `Future::then()` and `Future::recover()` over `Future::onReady()`, `Future::onFailed()`, `Future::onDiscarded()`, and `Future::onAny()`. A good rule of thumb is if you find yourself creating your own instance of a `Promise` to compose an asynchronous operation you should use [composition](#futures-and-promises-composition) instead! <br><br>

We use the macros `CHECK_PENDING()`, `CHECK_READY()`, `CHECK_FAILED()`, `CHECK_DISCARDED()` throughout our examples. See [`CHECK()` Overloads](#futures-and-promises-check-overloads) for more details about these macros.

### <a name="futures-and-promises-discarding-a-future"></a> Discarding a Future (aka Cancellation)

You can "cancel" the result of some asynchronous operation by discarding a future. Unlike doing a discard on a promise, _discarding a future is a request that may or may not be be satisfiable_. You discard a future using `Future::discard()`. You can determine if a future has a discard request by using `Future::hasDiscard()` or set up a callback using `Future::onDiscard()`. Here's an example:

```cpp
using process::Future;
using process::Promise;

int main(int argc, char** argv)
{
  Promise<int> promise;

  Future<int> future = promise.future();

  CHECK_PENDING(future);

  future.discard();

  CHECK(promise.future().hasDiscard());

  CHECK_PENDING(future); // THE FUTURE IS STILL PENDING!

  return 0;
}
```

The provider of the future will often use `Future::onDiscard()` to watch for discard requests and try and act accordingly, for example:

```cpp
using process::Future;
using process::Promise;

int main(int argc, char** argv)
{
  Promise<int> promise;

  // Set up a callback to discard the future if
  // requested (this is not always possible!).
  promise.future().onDiscard([&]() {
    promise.discard();
  });

  Future<int> future = promise.future();

  CHECK_PENDING(future);

  future.discard();

  CHECK_DISCARDED(future); // NO LONGER PENDING!

  return 0;
}
```

### <a name="futures-and-promises-abandoned-futures"></a> Abandoned Futures

An instance of `Promise` that is deleted before it has transitioned out of `PENDING` is considered abandoned. The concept of abandonment was added late to the library so for backwards compatibility reasons we could not add a new state but instead needed to have it be a sub-state of `PENDING`.

You can check if a future has been abandoned by doing `Future::isAbandoned()` and set up a callback using `Future::onAbandoned()`. Here's an example:

```cpp
using process::Future;
using process::Promise;

int main(int argc, char** argv)
{
  Promise<int>* promise = new Promise<int>();

  Future<int> future = promise->future();

  CHECK(!future.isAbandoned());

  delete promise; // ABANDONMENT!

  CHECK_ABANDONED(future);

  CHECK_PENDING(future); // ALSO STILL PENDING!

  return 0;
}
```

### <a name="futures-and-promises-composition"></a> Composition: `Future::then()`, `Future::repair()`, and `Future::recover()`

You can compose together asynchronous function calls using `Future::then()`, `Future::repair()`, and `Future::recover()`. To help understand the value of composition, we'll start with an example of how you might manually do this composition:

```cpp
using process::Future;
using process::Promise;

// Returns an instance of `Person` for the specified `name`.
Future<Person> find(const std::string& name);

// Returns the mother (an instance of `Person`) of the specified `name`.
Future<Person> mother(const std::string& name)
{
  // First find the person.
  Future<Person> person = find(name);

  // Now create a `Promise` that we can use to compose the two asynchronous calls.
  Promise<Person>* promise = new Promise<Person>();

  Future<Person> mother = promise->future();

  // Here is the boiler plate that can be replaced by `Future::then()`!
  person.onAny([](const Future<Person>& person) {
    if (person.isFailed()) {
      promise->fail(person.failure());
    } else if (person.isDiscarded()) {
      promise->discard();
    } else {
      CHECK_READY(person);
      promise->set(find(person->mother));
    }
    delete promise;
  });

  return mother;
}
```

Using `Future::then()` this can be simplified to:

```cpp
using process::Future;

// Returns an instance of `Person` for the specified `name`.
Future<Person> find(const std::string& name);

// Returns the mother (an instance of `Person`) of the specified `name`.
Future<Person> mother(const std::string& name)
{
  return find(name)
    .then([](const Person& person) {
      return find(person.mother);
    });
}
```

Each of `Future::then()`, `Future::repair()`, and `Future::recover()` takes a callback that will be invoked after certain transitions, captured by this table:

| Transition                                        | `Future::*()`                                    |
| ------------------------------------------------- | ------------------------------------------------ |
| `READY`                                           | `Future::then(F&&)`                              |
| `FAILED`                                          | `Future::repair(F&&)` and `Future::recover(F&&)` |
| `DISCARDED`                                       | `Future::recover(F&&)`                           |
| Abandoned (`PENDING` and `Future::isAbandoned()`) | `Future::recover(F&&)`                           |

`Future::then()` allows you to _transform_ the type of the `Future` into a new type but both `Future::repair()` and `Future::recover()` must return the same type as `Future` because they may not get executed! Here's an example using `Future::recover()` to handle a failure:

```cpp
using process::Future;

// Returns an instance of `Person` for the specified `name`.
Future<Person> find(const std::string& name);

// Returns a parent (an instance of `Person`) of the specified `name`.
Future<Person> parent(const std::string& name)
{
  return find(name)
    .then([](const Person& person) {
      // Try to find the mother and if that fails try the father!
      return find(person.mother)
        .recover([=](const Future<Person>&) {
          return find(person.father);
        });
    });
}
```

> <br> **Be careful what you capture in your callbacks! Depending on the state of the future the callback may be executed from a different scope and what ever you captured may no longer be valid; see [Callback Semantics](#futures-and-promises-callback-semantics) for more details.** <br><br>

### <a name="futures-and-promises-discarding-and-composition"></a> Discarding and Composition

Doing a `Future::discard()` will _propagate_ through each of the futures composed with `Future::then()`, `Future::recover()`, etc. This is usually what you want, but there are two important caveats to look out for:

#### <a name="futures-and-promises-then-discards"></a> 1. `Future::then()` enforces discards

The future returned by `Future::then()` _will not execute the callback if a discard has been requested._ That is, even if the future transitions to `READY`, `Future::then()` will still enforce the request to discard and transition the future to `DISCARDED`.

> <br> These semantics are surprising to many, and, admittedly, the library may at one point in the future change the semantics and introduce a `discardable()` helper for letting people explicitly decide if/when they want a callback to be discarded. Historically, these semantics were chosen so that people could write infinite loops using `Future::then()` that could be interrupted with `Future::discard()`. The proper way to do infinte loops today is with `loop()`. <br><br>

Here's an example to clarify this caveat:

```cpp
using process::Future;
using process::Promise;

int main(int argc, char** argv)
{
  Promise<int> promise;

  Future<std::string> future = promise.future()
    .then([](int i) {
      return stringify(i);
    });

  future.discard();

  CHECK_PENDING(future);

  promise.set(42);

  CHECK_DISCARDED(future); // EVEN THOUGH THE PROMISE COMPLETED SUCCESSFULLY!

  return 0;
}
```

#### <a name="futures-and-promises-undiscardable"></a> 2. Sometimes you want something to be `undiscardable()`

You may find yourself in a circumstance when you're referencing (or composing) futures but _you don't want a disard to propagate to the referenced future!_ Consider some code that needs to complete some expensive initialization before other functions can be called. We can model that by composing each function with the initialization, for example:

```cpp
using process::Future;

Future<T> foo()
{
  return initialization
    .then([](...) {
      return ...;
    });
}
```

In the above example, if someone were to discard the future returned from `foo()` _they would also end up discarding the initialization!_

The real intention here is to compose with the initialization but not propagate discarding. This can be accomplished using `undiscardable()` which acts as a barrier to stop a discard from propagating through. Here's how the previous example would look with `undiscardable()`:

```cpp
using process::Future;
using process::undiscardable;

Future<T> foo()
{
  return undiscardable(initialization)
    .then([](...) {
      return ...;
    });
}
```

### <a name="futures-and-promises-callback-semantics"></a> Callback Semantics

There are two possible ways in which the callbacks to the `Future::onReady()` family of functions as well as the composition functions like `Future::then()` get invoked:

1. By the caller of `Future::onReady()`, `Future::then()`, etc.
2. By the caller of `Promise::set()`, `Promise::fail()`, etc.

The first case occurs if the future is already transitioned to that state when adding the callback, i.e., if the state is already `READY` for either `Future::onReady()` or `Future::then()`, or `FAILED` for `Future::onFailed()` or `Future::recover()`, etc, then the callback will be executed immediately.

The second case occurs if the future has not yet transitioned to that state. In that case the callback is stored and it is executed by the caller of `Promise::set()`, `Promise::fail()`, whoever is deleting the promise (for abandonment), etc.

_This means that it is critical to consider the synchronization that might be necessary for your code given that multiple possible callers could execute the callback!_

We call these callback semantics _synchronous_, as opposed to _asynchronous_. You can use `defer()` in order to _asynchronously_ invoke your callbacks.

> <br> Note that after the callbacks are invoked they are deleted, so any resources that you might be holding on to inside the callback will properly be released after the future transitions and it's callbacks are invoked. <br><br>

### <a name="futures-and-promises-check-overloads"></a> `CHECK()` Overloads

`CHECK()` is a macro from [Google Test](https://github.com/google/googletest) which acts like an `assert` but prints a stack trace and does better signal management. In addition to `CHECK()`, we've also created wrapper macros `CHECK_PENDING()`, `CHECK_READY()`, `CHECK_FAILED()`, `CHECK_DISCARDED()` which enables you to more concisely do things like `CHECK_READY(future)` in your tests.

<!--- TODO(benh):

* `Future::after`
* `Future::await()`

--->


## <a name="http"></a> HTTP

libprocess provides facilities for communicating between actors via HTTP
messages. With the advent of the HTTP API, HTTP is becoming the preferred mode
of communication.

### `route`

`route` installs an HTTP endpoint onto a process. Let's define a simple process
that installs an endpoint upon initialization:

```cpp
using namespace process;
using namespace process::http;

class HttpProcess : public Process<HttpProcess>
{
protected:
  void initialize() override
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
```

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

```cpp
Future<Response> future = get(upid, "testing");
```

Or let's assume our serving process has been set up on a remote server and we
want to hit its endpoint. We'll construct a `URL` for the address and then call
`get`:

```cpp
URL url = URL("http", "hostname", 1234, "/testing");

Future<Response> future = get(url);
```

### `post` and `requestDelete`

The `post` and `requestDelete` functions will similarly send POST and DELETE
requests to an HTTP endpoint. Their invocation is analogous to `get`.

### `Connection`

A `Connection` represents a connection to an HTTP server. `connect`
can be used to connect to a server, and returns a `Future` containing the
`Connection`. Let's open a connection to a server and send some requests:

```cpp
Future<Connection> connect = connect(url);

connect.await();

Connection connection = connect.get();

Request request;
request.method = "GET";
request.url = url;
request.body = "Amazing prose goes here.";
request.keepAlive = true;

Future<Response> response = connection.send(request);
```

It's also worth noting that if multiple requests are sent in succession on a
`Connection`, they will be automatically pipelined.

## <a name="processes"></a> Processes (aka Actors)

An actor in libprocess is called a *process* (not to be confused by an operating system process).

A process receives events that it processes one at a time. Because a process is only processing one event at a time there's no need for synchronization within the process.

There are a few ways to create an event for a process, the most important including:

* You can `send()` a process a message.
* You can do a function `dispatch()` on a process.
* You can send an `http::Request` to a process.

That last one is exceptionally powerful; every process is also an HTTP-based service that you can communicate with using the HTTP protocol.

### Process Lifecycle

You create a process like any other class in C++ but extending from `Process`. `Process` uses the [curiously recurring template pattern (CRTP)](http://en.wikipedia.org/wiki/Curiously_recurring_template_pattern) to simplify types for some of it's methods (you'll see this with `Process::self()` below).

Practically you can think of a process as a combination of a thread and an object, except creating/spawning a process is very cheap (no actual thread gets created, and no stack gets allocated).

Here's the simplest process you can create:

```cpp
using process::Process;

class FooProcess : public Process<FooProcess> {};
```

You start a process using `spawn()`, stop a process using `terminate()`, and wait for it to terminate by using `wait()`:

```cpp
using process::Process;
using process::spawn;
using process::terminate;
using process::wait;

class FooProcess : public Process<FooProcess> {};

int main(int argc, char** argv)
{
  FooProcess process;
  spawn(process);
  terminate(process);
  wait(process);
  return 0;
}
```

#### Memory Management

A process ***CAN NOT*** be deleted until after doing a `wait()`, otherwise you might release resources that the library is still using! To simplify memory management you can ask the library to delete the process for you after it has completely terminated. You do this by invoking `spawn()` and passing `true` as the second argument:

```cpp
using process::Process;
using process::spawn;
using process::terminate;
using process::wait;

class FooProcess : public Process<FooProcess> {};

int main(int argc, char** argv)
{
  FooProcess* process = new FooProcess();
  spawn(process, true); // <-- `process` will be automatically deleted!
  terminate(process);
  wait(process)
  return 0;
}
```

### Process Identity: `PID` and `UPID`

A process is uniquely identifiable by it's process id which can be any arbitrary string (but only one process can be spawned at a time with the same id). The `PID` and `UPID` types encapsulate both the process id as well as the network address for the process, e.g., the IP and port where the process can be reached if libprocess was initialized with an IPv4 or IPv6 network address. You can get the `PID` of a process by calling it's `self()` method:

```cpp
using process::PID;
using process::Process;
using process::spawn;

class FooProcess : public Process<FooProcess> {};

int main(int argc, char** argv)
{
  FooProcess process;
  spawn(process);

  PID<FooProcess> pid = process.self();

  return 0;
}
```

A `UPID` is the "**u**ntyped" base class of `PID`.

> If you turn on logging you might see a `PID`/`UPID` printed out as `id@ip:port`.

### Process Event Queue (aka Mailbox)

Each process has a queue of incoming `Event`'s that it processes one at a time. Other actor implementations often call this queue the "mailbox".

There are 5 different kinds of events that can be enqueued for a process:

* `MessageEvent`: a `Message` has been received.
* `DispatchEvent`: a method on the process has been "dispatched".
* `HttpEvent`: an `http::Request` has been received.
* `ExitedEvent`: another process which has been [linked](#links) has terminated.
* `TerminateEvent`: the process has been requested to terminate.

An event is serviced one at a time by invoking the process' `serve()` method which by default invokes the process' `visit()` method corresponding to the underlying event type. Most actors don't need to override the implementation of `serve()` or `visit()` but can rely on higher-level abstractions that simplify serving the event (e.g., `route()`, which make it easy to set up handlers for an `HttpEvent`, discussed below in [HTTP](#http)).

#### `MessageEvent`

A `MessageEvent` gets enqueued for a process when it gets sent a `Message`, either locally or remotely. You use `send()` to send a message from within a process and `post()` to send a message from outside a process. A `post()` sends a message without a return address because there is no process to reply to. Here's a classic ping pong example using `send()`:

```cpp
using process::MessageEvent;
using process::PID;
using process::Process;
using process::spawn;
using process::terminate;
using process::wait;

class ClientProcess : public Process<ClientProcess>
{
public:
  ClientProcess(const UPID& server) : server(server) {}

  void initialize() override
  {
    send(server, "ping");
  }

  void visit(const MessageEvent& event) override
  {
    if (event.message.from == server &&
        event.message.name == "pong") {
      terminate(self());
    }
  }
};

class ServerProcess : public Process<ServerProcess>
{
public:
protected:
  void visit(const MessageEvent& event) override
  {
    if (event.message.name == "ping") {
      send(event.message.from, "pong");
    }
    terminate(self());
  }
};

int main(int argc, char** argv)
{
  PID<ServerProcess> server = spawn(new ServerProcess(), true);
  PID<ClientProcess> client = spawn(new ClientProcess(server), true);

  wait(server);
  wait(client);

  return 0;
}
```

#### `DispatchEvent`

TODO.

#### `HttpEvent`

TODO.

#### `ExitedEvent`

TODO.

#### `TerminateEvent`

TODO.

### <a name="async-pimpl"></a> Processes and the Asynchronous Pimpl Pattern

It's tedious to require everyone to have to explicitly `spawn()`, `terminate()`, and `wait()` for a process. Having everyone call `dispatch()` when they really just want to invoke a function (albeit asynchronously) is unfortnate too! To alleviate these burdenes, a common pattern that is used is to wrap a process within another class that performs the `spawn()`, `terminate()`, `wait()`, and `dispatch()`'s for you. Here's a typical example:

```cpp
class FooProcess : public Process<FooProcess>
{
public:
  void foo(int i);
};

class Foo
{
public:
  Foo()
  {
    process::spawn(process);
  }

  ~Foo()
  {
    process::terminate(process);
    process::wait(process);
  }

  void foo(int i)
  {
    dispatch(process, &FooProcess::foo, i);
  }

private:
  FooProcess process;
};

int main(int argc, char** argv)
{
  Foo foo;

  foo.foo(42);

  return 0;
}
```

Anyone using `Foo` uses it similarly to how they would work with any other _synchronous_ object where they don't know, or need to know, the implementation details under the covers (i.e., that it's implemented using a process). This is similar to the [Pimpl](https://en.wikipedia.org/wiki/Opaque_pointer) pattern, except we need to `spawn()` and `terminate()/wait()` and rather than _synchronously_ invoking the underlying object we're _asynchronously_ invoking the underlying object using `dispatch()`.

## <a name="clock"></a> Clock Management and Timeouts

Asynchronous programs often use timeouts, e.g., because a process that initiates
an asynchronous operation wants to take action if the operation hasn't completed
within a certain time bound. To facilitate this, libprocess provides a set of
abstractions that simplify writing timeout logic. Importantly, test code has the
ability to manipulate the clock, in order to ensure that timeout logic is
exercised (without needing to block the test program until the appropriate
amount of system time has elapsed).

To invoke a function after a certain amount of time has elapsed, use `delay`:

```cpp
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
```

This invokes the `action` function after (at least) five seconds of time
have elapsed. When writing unit tests for this code, blocking the test for five
seconds is undesirable. To avoid this, we can use `Clock::advance`:

```cpp

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
```

## <a name="miscellaneous-primitives"></a> Miscellaneous Primitives

### `async`

Async defines a function template for asynchronously executing
function closures. It provides their results as
[futures](#futures-and-promises).

## <a name="optimized-run-queue-event-queue"></a> Optimized Run Queue and Event Queue

There are a handful of compile-time optimizations that can be
configured to improve the run queue and event queue performance. These
are currently not enabled by default as they are considered
***alpha***. These optimizations include:

* `--enable-lock-free-run-queue` (autotools) or
  `-DENABLE_LOCK_FREE_RUN_QUEUE` (cmake) which enables the lock-free
  run queue implementation.

* `--enable-lock-free-event-queue` (autotools) or
  `-DENABLE_LOCK_FREE_EVENT_QUEUE` (cmake) which enables the lock-free
  event queue implementation.

* `--enable-last-in-first-out-fixed-size-semaphore` (autotools) or
  `-DENABLE_LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE` (cmake) which
  enables an optimized semaphore implementation.

#### Details

Both the lock-free run queue implementation and the lock-free event
queue implementation use `moodycamel::ConcurrentQueue` which can be
found [here](https://github.com/cameron314/concurrentqueue).

For the run queue we use a semaphore to block threads when there are
not any processes to run. On Linux we found that using a semaphore
from glibc (i.e., `sem_create`, `sem_wait`, `sem_post`, etc) had some
performance issues. We discuss those performance issues and how our
optimized semaphore overcomes them in more detail in
[semaphore.hpp](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/src/semaphore.hpp#L191).

#### Benchmark

The benchmark that we've used to drive the run queue and event queue
performance improvements can be found in
[benchmarks.cpp](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/src/tests/benchmarks.cpp#L426). You
can run the benchmark yourself by invoking `./benchmarks
--gtest_filter=ProcessTest.*ThroughputPerformance`.

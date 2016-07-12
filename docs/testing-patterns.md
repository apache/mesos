---
title: Apache Mesos - Testing Patterns
layout: documentation
---

# Mesos Testing Patterns

A collection of common testing patterns used in Mesos tests. If you have found a good way to test a certain condition that you think may be useful for other cases, please document it here together with motivation and background.

## Expediting events with `Clock`
Some events in Mesos are separated by certain timeouts, for example framework registration attempts. Simple waiting for such events to fire leads to blocking the test thread for the duration of the associated timeout. This increases the duration of `make check` for no good reason.

If an event is triggered by an act of processing a message from an actor's mailbox, it can be expedited with the help of libprocess' `Clock` routines. Delayed messages are maintained in sorted order by their due time and are dispatched - i.e. pushed into destination mailboxes - when this time comes. An important bit here is that time is driven by the internal libprocess clock. We can shift this clock into the future by calling `Clock::advance(<duration>)`, rendering certain front messages in the collection due now. These messages are dispatched instantly, effectively overriding the associated event's timeout.

**NOTE**: Without calling `Clock::settle()` there is no guarantee a dispatched message has been already processed.

Below is an example of this pattern. To avoid master backlogging, Mesos frameworks usually wait for some time (backoff) before retrying registration. In the test below we simulate the loss of a registration request, but avoid blocking the test for the backoff duration.

~~~{.cpp}
TEST_F(FaultToleranceTest, FrameworkReliableRegistration)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  // As a side effect of driver instantiation, registration backoff will be set
  // to a default: internal::scheduler::REGISTRATION_BACKOFF_FACTOR.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<AuthenticateMessage> authenticateMessage =
    FUTURE_PROTOBUF(AuthenticateMessage(), _, master.get());

  // Drop the first framework registered message, allow subsequent messages.
  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    DROP_PROTOBUF(FrameworkRegisteredMessage(), master.get(), _);

  driver.start();

  // Ensure authentication occurs.
  AWAIT_READY(authenticateMessage);

  AWAIT_READY(frameworkRegisteredMessage);

  // Trigger the registration retry instantly to avoid blocking the test.
  Clock::pause();
  Clock::advance(internal::scheduler::REGISTRATION_BACKOFF_FACTOR);

  AWAIT_READY(registered); // Ensures registered message is received.

  driver.stop();
  driver.join();

  Shutdown();

  Clock::resume();
}
~~~

## Using `Clock` magic to ensure an event is processed
Scheduling a sequence of events in an asynchronous environment is not easy: a function call usually initiates an action and returns immediately, while the action runs in background. A simple, obvious, and bad solution is to use `os::sleep()` to wait for action completion. The time the action needs to finish may vary on different machines, while increasing sleep duration increases the test execution time and slows down `make check`. One of the right ways to do it is to wait for an action to finish and proceed right after. This is possible using libprocess' `Clock` routines.


Every message enqueued in a libprocess process' (or actor's, to avoid ambiguity with OS processes) mailbox is processed by `ProcessManager` (right now there is a single instance of `ProcessManager` per OS process, but this may change in the future). `ProcessManager` fetches actors from the runnable actors list and services all events from the actor's mailbox. Using `Clock::settle()` call we can block the calling thread until `ProcessManager` empties mailboxes of all actors. Here is the example of this pattern:

~~~{.cpp}
// As Master::killTask isn't doing anything, we shouldn't get a status update.
EXPECT_CALL(sched, statusUpdate(&driver, _))
  .Times(0);

// Set expectation that Master receives killTask message.
Future<KillTaskMessage> killTaskMessage =
  FUTURE_PROTOBUF(KillTaskMessage(), _, master.get());

// Attempt to kill unknown task while agent is transitioning.
TaskID unknownTaskId;
unknownTaskId.set_value("2");

// Stop the clock.
Clock::pause();

// Initiate an action.
driver.killTask(unknownTaskId);

// Make sure the event associated with the action has been queued.
AWAIT_READY(killTaskMessage);

// Wait for all messages to be dispatched and processed completely to satisfy
// the expectation that we didn't receive a status update.
Clock::settle();

Clock::resume();
~~~

## Intercepting a message sent to a different OS process
Intercepting messages sent between libprocess processes (let's call them actors to avoid ambiguity with OS processes) that live in the same OS process is easy, e.g.:

~~~{.cpp}
Future<SlaveReregisteredMessage> slaveReregisteredMessage =
  FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);
...
AWAIT_READY(slaveReregisteredMessage);
~~~

However, this won't work if we want to intercept a message sent to an actor (technically a `UPID`) that lives in another OS process. For example, `CommandExecutor` spawned by an agent will live in a separate OS process, though master and agent instances live in the same OS process together with our test (see `mesos/src/tests/cluster.hpp`). The wait in this code will fail:

~~~{.cpp}
Future<ExecutorRegisteredMessage> executorRegisteredMessage =
  FUTURE_PROTOBUF(ExecutorRegisteredMessage(), _, _);
...
AWAIT_READY(executorRegisteredMessage);
~~~

### Why messages sent outside the OS process are not intercepted?
Libprocess events may be filtered (see `libprocess/include/process/filter.hpp`). `FUTURE_PROTOBUF` uses this ability and sets an expectation on a `filter` method of `TestsFilter` class with a `MessageMatcher`, that matches the message we want to intercept. The actual filtering happens in `ProcessManager::resume()`, which fetches messages from the queue of the received events.

*No* filtering happens when sending, encoding, or transporting the message (see e.g. `ProcessManager::deliver()` or `SocketManager::send()`). Therefore in the aforementioned example, `ExecutorRegisteredMessage` leaves the agent undetected by the filter, reaches another OS process where executor lives, gets enqueued into the `CommandExecutorProcess`' mailbox and can be filtered there, but remember our expectation is set in another OS process!

### How to workaround
Consider setting expectations on corresponding incoming messages ensuring they are processed and therefore ACK message is sent.

For the aforementioned example, instead of intercepting `ExecutorRegisteredMessage`, we can intercept `RegisterExecutorMessage` and wait until its processed, which includes sending `ExecutorRegisteredMessage` (see `Slave::registerExecutor()`):

~~~{.cpp}
Future<RegisterExecutorMessage> registerExecutorMessage =
  FUTURE_PROTOBUF(RegisterExecutorMessage(), _, _);
...
AWAIT_READY(registerExecutorMessage);
Clock::pause();
Clock::settle();
Clock::resume();
~~~

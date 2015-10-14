/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <time.h>

#include <arpa/inet.h>

#include <gmock/gmock.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <string>
#include <sstream>
#include <tuple>
#include <vector>

#include <process/async.hpp>
#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>
#include <process/filter.hpp>
#include <process/future.hpp>
#include <process/gc.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/network.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/run.hpp>
#include <process/socket.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>

#include "encoder.hpp"

namespace http = process::http;
namespace inject = process::inject;

using process::async;
using process::Clock;
using process::defer;
using process::Deferred;
using process::Event;
using process::Executor;
using process::ExitedEvent;
using process::Failure;
using process::Future;
using process::Message;
using process::MessageEncoder;
using process::MessageEvent;
using process::Owned;
using process::PID;
using process::Process;
using process::ProcessBase;
using process::Promise;
using process::run;
using process::TerminateEvent;
using process::Time;
using process::UPID;

using process::firewall::DisabledEndpointsFirewallRule;
using process::firewall::FirewallRule;

using process::network::Address;
using process::network::Socket;

using std::move;
using std::string;
using std::vector;

using testing::_;
using testing::Assign;
using testing::DoAll;
using testing::Return;
using testing::ReturnArg;

// TODO(bmahler): Move tests into their own files as appropriate.

TEST(ProcessTest, Event)
{
  Event* event = new TerminateEvent(UPID());
  EXPECT_FALSE(event->is<MessageEvent>());
  EXPECT_FALSE(event->is<ExitedEvent>());
  EXPECT_TRUE(event->is<TerminateEvent>());
  delete event;
}


TEST(ProcessTest, Future)
{
  Promise<bool> promise;
  promise.set(true);
  ASSERT_TRUE(promise.future().isReady());
  EXPECT_TRUE(promise.future().get());
}


TEST(ProcessTest, Associate)
{
  Promise<bool> promise1;
  Future<bool> future1(true);
  promise1.associate(future1);
  ASSERT_TRUE(promise1.future().isReady());
  EXPECT_TRUE(promise1.future().get());

  Promise<bool> promise2;
  Promise<bool> promise2_;
  Future<bool> future2 = promise2_.future();
  promise2.associate(future2);
  promise2_.discard();
  ASSERT_TRUE(promise2.future().isDiscarded());

  Promise<bool> promise3;
  Promise<bool> promise4;
  promise3.associate(promise4.future());
  promise4.fail("associate");
  ASSERT_TRUE(promise3.future().isFailed());
  EXPECT_EQ("associate", promise3.future().failure());

  // Test 'discard' versus 'discarded' after association.
  Promise<bool> promise5;
  Future<bool> future3;
  promise5.associate(future3);
  EXPECT_FALSE(future3.isDiscarded());
  promise5.future().discard();
  EXPECT_TRUE(future3.hasDiscard());

  Promise<bool> promise6;
  Promise<bool> promise7;
  promise6.associate(promise7.future());
  promise7.discard();
  EXPECT_TRUE(promise6.future().isDiscarded());
}


void onAny(const Future<bool>& future, bool* b)
{
  ASSERT_TRUE(future.isReady());
  *b = future.get();
}


TEST(ProcessTest, OnAny)
{
  bool b = false;
  Future<bool>(true)
    .onAny(lambda::bind(&onAny, lambda::_1, &b));
  EXPECT_TRUE(b);
}


Future<string> itoa1(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


string itoa2(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


TEST(ProcessTest, Then)
{
  Promise<int*> promise;

  int i = 42;

  promise.set(&i);

  Future<string> future = promise.future()
    .then(lambda::bind(&itoa1, lambda::_1));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ("42", future.get());

  future = promise.future()
    .then(lambda::bind(&itoa2, lambda::_1));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ("42", future.get());
}


Future<int> repair(const Future<int>& future)
{
  EXPECT_TRUE(future.isFailed());
  EXPECT_EQ("Failure", future.failure());
  return 43;
}


// Checks that 'repair' callback gets executed if the future failed
// and not executed if the future is completed successfully.
TEST(ProcessTest, Repair)
{
  // Check that the 'repair' callback _does not_ get executed by
  // making sure that when we complete the promise with a value that's
  // the value that we get back.
  Promise<int> promise1;

  Future<int> future1 = promise1.future()
    .repair(lambda::bind(&repair, lambda::_1));

  EXPECT_TRUE(future1.isPending());

  promise1.set(42); // So this means 'repair' should not get executed.

  AWAIT_EXPECT_EQ(42, future1);

  // Check that the 'repair' callback gets executed by failing the
  // promise which should invoke the 'repair' callback.
  Promise<int> promise2;

  Future<int> future2 = promise2.future()
    .repair(lambda::bind(&repair, lambda::_1));

  EXPECT_TRUE(future2.isPending());

  promise2.fail("Failure"); // So 'repair' should get called returning '43'.

  AWAIT_EXPECT_EQ(43, future2);
}



Future<Nothing> after(volatile bool* executed, const Future<Nothing>& future)
{
  EXPECT_TRUE(future.hasDiscard());
  *executed = true;
  return Failure("Failure");
}


// Checks that the 'after' callback gets executed if the future is not
// completed.
TEST(ProcessTest, After1)
{
  Clock::pause();

  volatile bool executed = false;

  Future<Nothing> future = Future<Nothing>()
    .after(Hours(42), lambda::bind(&after, &executed, lambda::_1));

  // A pending future should stay pending until 'after' is executed.
  EXPECT_TRUE(future.isPending());

  // Only advanced halfway, future should remain pending.
  Clock::advance(Hours(21));

  EXPECT_TRUE(future.isPending());

  // Even doing a discard on the future should keep it pending.
  future.discard();

  EXPECT_TRUE(future.isPending());

  // After advancing all the way the future should now fail because
  // the 'after' callback gets executed.
  Clock::advance(Hours(21));

  AWAIT_FAILED(future);

  EXPECT_TRUE(executed);

  Clock::resume();
}


// Checks that completing a promise will keep the 'after' callback
// from executing.
TEST(ProcessTest, After2)
{
  Clock::pause();

  volatile bool executed = false;

  Promise<Nothing> promise;

  Future<Nothing> future = promise.future()
    .after(Hours(42), lambda::bind(&after, &executed, lambda::_1));

  EXPECT_TRUE(future.isPending());

  // Only advanced halfway, future should remain pending.
  Clock::advance(Hours(21));

  EXPECT_TRUE(future.isPending());

  // Even doing a discard on the future should keep it pending.
  future.discard();

  EXPECT_TRUE(future.isPending());

  // Now set the promise, the 'after' timer should be cancelled and
  // the pending future should be completed.
  promise.set(Nothing());

  AWAIT_READY(future);

  // Advancing time the rest of the way should not cause the 'after'
  // callback to execute.
  Clock::advance(Hours(21));

  EXPECT_FALSE(executed);

  Clock::resume();
}


Future<bool> readyFuture()
{
  return true;
}


Future<bool> failedFuture()
{
  return Failure("The value is not positive (or zero)");
}


Future<bool> pendingFuture(const Future<bool>& future)
{
  return future; // Keep it pending.
}


Future<string> second(const bool& b)
{
  return b ? string("true") : string("false");
}


Future<string> third(const string& s)
{
  return s;
}


TEST(ProcessTest, Chain)
{
  Future<string> s = readyFuture()
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  s.await();

  ASSERT_TRUE(s.isReady());
  EXPECT_EQ("true", s.get());

  s = failedFuture()
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  s.await();

  ASSERT_TRUE(s.isFailed());

  Promise<bool> promise;

  s = pendingFuture(promise.future())
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  ASSERT_TRUE(s.isPending());

  promise.discard();

  AWAIT_DISCARDED(s);
}


Future<bool> inner1(const Future<bool>& future)
{
  return future;
}


Future<int> inner2(volatile bool* executed, const Future<int>& future)
{
  *executed = true;
  return future;
}


// Tests that Future::discard does not complete the future unless
// Promise::discard is invoked.
TEST(ProcessTest, Discard1)
{
  Promise<bool> promise1;
  Promise<int> promise2;

  volatile bool executed = false;

  Future<int> future = Future<string>("hello world")
    .then(lambda::bind(&inner1, promise1.future()))
    .then(lambda::bind(&inner2, &executed, promise2.future()));

  ASSERT_TRUE(future.isPending());

  future.discard();

  // The future should remain pending, even though we discarded it.
  ASSERT_TRUE(future.hasDiscard());
  ASSERT_TRUE(future.isPending());

  // The future associated with the lambda already executed in the
  // first 'then' should have the discard propagated to it.
  ASSERT_TRUE(promise1.future().hasDiscard());

  // But the future assocaited with the lambda that hasn't yet been
  // executed should not have the discard propagated to it.
  ASSERT_FALSE(promise2.future().hasDiscard());

  // Now discarding the promise should cause the outer future to be
  // discarded also.
  ASSERT_TRUE(promise1.discard());

  AWAIT_DISCARDED(future);

  // And the final lambda should never have executed.
  ASSERT_FALSE(executed);
  ASSERT_TRUE(promise2.future().isPending());
}


// Tests that Future::discard does not complete the future and
// Promise::set can still be invoked to complete the future.
TEST(ProcessTest, Discard2)
{
  Promise<bool> promise1;
  Promise<int> promise2;

  volatile bool executed = false;

  Future<int> future = Future<string>("hello world")
    .then(lambda::bind(&inner1, promise1.future()))
    .then(lambda::bind(&inner2, &executed, promise2.future()));

  ASSERT_TRUE(future.isPending());

  future.discard();

  // The future should remain pending, even though we discarded it.
  ASSERT_TRUE(future.hasDiscard());
  ASSERT_TRUE(future.isPending());

  // The future associated with the lambda already executed in the
  // first 'then' should have the discard propagated to it.
  ASSERT_TRUE(promise1.future().hasDiscard());

  // But the future assocaited with the lambda that hasn't yet been
  // executed should not have the discard propagated to it.
  ASSERT_FALSE(promise2.future().hasDiscard());

  // Now setting the promise should cause the outer future to be
  // discarded rather than executing the last lambda because the
  // implementation of Future::then does not continue the chain once a
  // discard occurs.
  ASSERT_TRUE(promise1.set(true));

  AWAIT_DISCARDED(future);

  // And the final lambda should never have executed.
  ASSERT_FALSE(executed);
  ASSERT_TRUE(promise2.future().isPending());
}


// Tests that Future::discard does not complete the future and
// Promise::fail can still be invoked to complete the future.
TEST(ProcessTest, Discard3)
{
  Promise<bool> promise1;
  Promise<int> promise2;

  volatile bool executed = false;

  Future<int> future = Future<string>("hello world")
    .then(lambda::bind(&inner1, promise1.future()))
    .then(lambda::bind(&inner2, &executed, promise2.future()));

  ASSERT_TRUE(future.isPending());

  future.discard();

  // The future should remain pending, even though we discarded it.
  ASSERT_TRUE(future.hasDiscard());
  ASSERT_TRUE(future.isPending());

  // The future associated with the lambda already executed in the
  // first 'then' should have the discard propagated to it.
  ASSERT_TRUE(promise1.future().hasDiscard());

  // But the future assocaited with the lambda that hasn't yet been
  // executed should not have the discard propagated to it.
  ASSERT_FALSE(promise2.future().hasDiscard());

  // Now failing the promise should cause the outer future to be
  // failed also.
  ASSERT_TRUE(promise1.fail("failure message"));

  AWAIT_FAILED(future);

  // And the final lambda should never have executed.
  ASSERT_FALSE(executed);
  ASSERT_TRUE(promise2.future().isPending());
}


class SpawnProcess : public Process<SpawnProcess>
{
public:
  MOCK_METHOD0(initialize, void(void));
  MOCK_METHOD0(finalize, void(void));
};


TEST(ProcessTest, Spawn)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SpawnProcess process;

  EXPECT_CALL(process, initialize())
    .Times(1);

  EXPECT_CALL(process, finalize())
    .Times(1);

  PID<SpawnProcess> pid = spawn(process);

  ASSERT_FALSE(!pid);

  ASSERT_FALSE(wait(pid, Seconds(0)));

  terminate(pid);
  wait(pid);
}


class DispatchProcess : public Process<DispatchProcess>
{
public:
  MOCK_METHOD0(func0, void(void));
  MOCK_METHOD1(func1, bool(bool));
  MOCK_METHOD1(func2, Future<bool>(bool));
  MOCK_METHOD1(func3, int(int));
  MOCK_METHOD2(func4, Future<bool>(bool, int));
};


TEST(ProcessTest, Dispatch)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchProcess process;

  EXPECT_CALL(process, func0())
    .Times(1);

  EXPECT_CALL(process, func1(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func2(_))
    .WillOnce(ReturnArg<0>());

  PID<DispatchProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  dispatch(pid, &DispatchProcess::func0);

  Future<bool> future;

  future = dispatch(pid, &DispatchProcess::func1, true);

  EXPECT_TRUE(future.get());

  future = dispatch(pid, &DispatchProcess::func2, true);

  EXPECT_TRUE(future.get());

  terminate(pid);
  wait(pid);
}


TEST(ProcessTest, Defer1)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchProcess process;

  EXPECT_CALL(process, func0())
    .Times(1);

  EXPECT_CALL(process, func1(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func2(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func4(_, _))
    .WillRepeatedly(ReturnArg<0>());

  PID<DispatchProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  {
    Deferred<void(void)> func0 =
      defer(pid, &DispatchProcess::func0);
    func0();
  }

  Future<bool> future;

  {
    Deferred<Future<bool>(void)> func1 =
      defer(pid, &DispatchProcess::func1, true);
    future = func1();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>(void)> func2 =
      defer(pid, &DispatchProcess::func2, true);
    future = func2();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>(void)> func4 =
      defer(pid, &DispatchProcess::func4, true, 42);
    future = func4();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>(bool)> func4 =
      defer(pid, &DispatchProcess::func4, lambda::_1, 42);
    future = func4(false);
    EXPECT_FALSE(future.get());
  }

  {
    Deferred<Future<bool>(int)> func4 =
      defer(pid, &DispatchProcess::func4, true, lambda::_1);
    future = func4(42);
    EXPECT_TRUE(future.get());
  }

  // Only take const &!

  terminate(pid);
  wait(pid);
}


class DeferProcess : public Process<DeferProcess>
{
public:
  Future<string> func1(const Future<int>& f)
  {
    return f.then(defer(self(), &Self::_func1, lambda::_1));
  }

  Future<string> func2(const Future<int>& f)
  {
    return f.then(defer(self(), &Self::_func2));
  }

private:
  Future<string> _func1(int i)
  {
    return stringify(i);
  }

  Future<string> _func2()
  {
    return string("42");
  }
};


TEST(ProcessTest, Defer2)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DeferProcess process;

  PID<DeferProcess> pid = spawn(process);

  Future<string> f = dispatch(pid, &DeferProcess::func1, 41);

  f.await();

  ASSERT_TRUE(f.isReady());
  EXPECT_EQ("41", f.get());

  f = dispatch(pid, &DeferProcess::func2, 41);

  f.await();

  ASSERT_TRUE(f.isReady());
  EXPECT_EQ("42", f.get());

  terminate(pid);
  wait(pid);
}


template <typename T>
void set(T* t1, const T& t2)
{
  *t1 = t2;
}


TEST(ProcessTest, Defer3)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  volatile bool bool1 = false;
  volatile bool bool2 = false;

  Deferred<void(bool)> set1 =
    defer([&bool1](bool b) { bool1 = b; });

  set1(true);

  Deferred<void(bool)> set2 =
    defer([&bool2](bool b) { bool2 = b; });

  set2(true);

  while (!bool1);
  while (!bool2);
}


class HandlersProcess : public Process<HandlersProcess>
{
public:
  HandlersProcess()
  {
    install("func", &HandlersProcess::func);
  }

  MOCK_METHOD2(func, void(const UPID&, const string&));
};


TEST(ProcessTest, Handlers)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  EXPECT_CALL(process, func(_, _))
    .Times(1);

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  post(pid, "func");

  terminate(pid, false);
  wait(pid);
}


// Tests DROP_MESSAGE and DROP_DISPATCH and in particular that an
// event can get dropped before being processed.
TEST(ProcessTest, Expect)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  EXPECT_CALL(process, func(_, _))
    .Times(0);

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  Future<Message> message = DROP_MESSAGE("func", _, _);

  post(pid, "func");

  AWAIT_EXPECT_READY(message);

  Future<Nothing> func = DROP_DISPATCH(pid, &HandlersProcess::func);

  dispatch(pid, &HandlersProcess::func, pid, "");

  AWAIT_EXPECT_READY(func);

  terminate(pid, false);
  wait(pid);
}


// Tests the FutureArg<N> action.
TEST(ProcessTest, Action)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  Future<string> future1;
  Future<Nothing> future2;
  EXPECT_CALL(process, func(_, _))
    .WillOnce(FutureArg<1>(&future1))
    .WillOnce(FutureSatisfy(&future2));

  dispatch(pid, &HandlersProcess::func, pid, "hello world");

  AWAIT_EXPECT_EQ("hello world", future1);

  EXPECT_TRUE(future2.isPending());

  dispatch(pid, &HandlersProcess::func, pid, "hello world");

  AWAIT_EXPECT_READY(future2);

  terminate(pid, false);
  wait(pid);
}


class BaseProcess : public Process<BaseProcess>
{
public:
  virtual void func() = 0;
  MOCK_METHOD0(foo, void());
};


class DerivedProcess : public BaseProcess
{
public:
  DerivedProcess() {}
  MOCK_METHOD0(func, void());
};


TEST(ProcessTest, Inheritance)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DerivedProcess process;

  EXPECT_CALL(process, func())
    .Times(2);

  EXPECT_CALL(process, foo())
    .Times(1);

  PID<DerivedProcess> pid1 = spawn(&process);

  ASSERT_FALSE(!pid1);

  dispatch(pid1, &DerivedProcess::func);

  PID<BaseProcess> pid2(process);
  PID<BaseProcess> pid3 = pid1;

  ASSERT_EQ(pid2, pid3);

  dispatch(pid3, &BaseProcess::func);
  dispatch(pid3, &BaseProcess::foo);

  terminate(pid1, false);
  wait(pid1);
}


TEST(ProcessTest, Thunk)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  struct Thunk
  {
    static int run(int i)
    {
      return i;
    }

    static int run(int i, int j)
    {
      return run(i + j);
    }
  };

  int result = run(&Thunk::run, 21, 21).get();

  EXPECT_EQ(42, result);
}


class DelegatorProcess : public Process<DelegatorProcess>
{
public:
  explicit DelegatorProcess(const UPID& delegatee)
  {
    delegate("func", delegatee);
  }
};


class DelegateeProcess : public Process<DelegateeProcess>
{
public:
  DelegateeProcess()
  {
    install("func", &DelegateeProcess::func);
  }

  MOCK_METHOD2(func, void(const UPID&, const string&));
};


TEST(ProcessTest, Delegate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DelegateeProcess delegatee;
  DelegatorProcess delegator(delegatee.self());

  EXPECT_CALL(delegatee, func(_, _))
    .Times(1);

  spawn(&delegator);
  spawn(&delegatee);

  post(delegator.self(), "func");

  terminate(delegator, false);
  wait(delegator);

  terminate(delegatee, false);
  wait(delegatee);
}


class TimeoutProcess : public Process<TimeoutProcess>
{
public:
  MOCK_METHOD0(timeout, void());
};


TEST(ProcessTest, Delay)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  volatile bool timeoutCalled = false;

  TimeoutProcess process;

  EXPECT_CALL(process, timeout())
    .WillOnce(Assign(&timeoutCalled, true));

  spawn(process);

  delay(Seconds(5), process.self(), &TimeoutProcess::timeout);

  Clock::advance(Seconds(5));

  while (!timeoutCalled);

  terminate(process);
  wait(process);

  Clock::resume();
}


class OrderProcess : public Process<OrderProcess>
{
public:
  void order(const PID<TimeoutProcess>& pid)
  {
    // TODO(benh): Add a test which uses 'send' instead of dispatch.
    dispatch(pid, &TimeoutProcess::timeout);
  }
};


TEST(ProcessTest, Order)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  TimeoutProcess process1;

  volatile bool timeoutCalled = false;

  EXPECT_CALL(process1, timeout())
    .WillOnce(Assign(&timeoutCalled, true));

  spawn(process1);

  Time now = Clock::now(&process1);

  Seconds seconds(1);

  Clock::advance(Seconds(1));

  EXPECT_EQ(now, Clock::now(&process1));

  OrderProcess process2;
  spawn(process2);

  dispatch(process2, &OrderProcess::order, process1.self());

  while (!timeoutCalled);

  EXPECT_EQ(now + seconds, Clock::now(&process1));

  terminate(process1);
  wait(process1);

  terminate(process2);
  wait(process2);

  Clock::resume();
}


class DonateProcess : public Process<DonateProcess>
{
public:
  void donate()
  {
    DonateProcess process;
    spawn(process);
    terminate(process);
    wait(process);
  }
};


TEST(ProcessTest, Donate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DonateProcess process;
  spawn(process);

  dispatch(process, &DonateProcess::donate);

  terminate(process, false);
  wait(process);
}


class ExitedProcess : public Process<ExitedProcess>
{
public:
  explicit ExitedProcess(const UPID& pid) { link(pid); }

  MOCK_METHOD1(exited, void(const UPID&));
};


TEST(ProcessTest, Exited)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  UPID pid = spawn(new ProcessBase(), true);

  ExitedProcess process(pid);

  volatile bool exitedCalled = false;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(Assign(&exitedCalled, true));

  spawn(process);

  terminate(pid);

  while (!exitedCalled);

  terminate(process);
  wait(process);
}


TEST(ProcessTest, InjectExited)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  UPID pid = spawn(new ProcessBase(), true);

  ExitedProcess process(pid);

  volatile bool exitedCalled = false;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(Assign(&exitedCalled, true));

  spawn(process);

  inject::exited(pid, process.self());

  while (!exitedCalled);

  terminate(process);
  wait(process);
}


TEST(ProcessTest, Select)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  std::set<Future<int>> futures;
  futures.insert(promise1.future());
  futures.insert(promise2.future());
  futures.insert(promise3.future());
  futures.insert(promise4.future());

  promise1.set(42);

  Future<Future<int>> future = select(futures);

  AWAIT_READY(future);
  AWAIT_READY(future.get());
  EXPECT_EQ(42, future.get().get());

  futures.erase(promise1.future());

  future = select(futures);
  EXPECT_TRUE(future.isPending());

  future.discard();
  AWAIT_DISCARDED(future);
}


class SettleProcess : public Process<SettleProcess>
{
public:
  SettleProcess() : calledDispatch(false) {}

  virtual void initialize()
  {
    os::sleep(Milliseconds(10));
    delay(Seconds(0), self(), &SettleProcess::afterDelay);
  }

  void afterDelay()
  {
    dispatch(self(), &SettleProcess::afterDispatch);
    os::sleep(Milliseconds(10));
    TimeoutProcess timeoutProcess;
    spawn(timeoutProcess);
    terminate(timeoutProcess);
    wait(timeoutProcess);
  }

  void afterDispatch()
  {
    os::sleep(Milliseconds(10));
    calledDispatch = true;
  }

  volatile bool calledDispatch;
};


TEST(ProcessTest, Settle)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();
  SettleProcess process;
  spawn(process);
  Clock::settle();
  ASSERT_TRUE(process.calledDispatch);
  terminate(process);
  wait(process);
  Clock::resume();
}


TEST(ProcessTest, Pid)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TimeoutProcess process;

  PID<TimeoutProcess> pid = process;
}


class Listener1 : public Process<Listener1>
{
public:
  virtual void event1() = 0;
};


class Listener2 : public Process<Listener2>
{
public:
  virtual void event2() = 0;
};


class MultipleListenerProcess
  : public Process<MultipleListenerProcess>,
    public Listener1,
    public Listener2
{
public:
  MOCK_METHOD0(event1, void());
  MOCK_METHOD0(event2, void());
};


TEST(ProcessTest, Listener)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MultipleListenerProcess process;

  EXPECT_CALL(process, event1())
    .Times(1);

  EXPECT_CALL(process, event2())
    .Times(1);

  spawn(process);

  dispatch(PID<Listener1>(process), &Listener1::event1);
  dispatch(PID<Listener2>(process), &Listener2::event2);

  terminate(process, false);
  wait(process);
}


class EventReceiver
{
public:
  MOCK_METHOD1(event1, void(int));
  MOCK_METHOD1(event2, void(const string&));
};


TEST(ProcessTest, Executor)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  volatile bool event1Called = false;
  volatile bool event2Called = false;

  EventReceiver receiver;

  EXPECT_CALL(receiver, event1(42))
    .WillOnce(Assign(&event1Called, true));

  EXPECT_CALL(receiver, event2("event2"))
    .WillOnce(Assign(&event2Called, true));

  Executor executor;

  Deferred<void(int)> event1 =
    executor.defer([&receiver](int i) {
      return receiver.event1(i);
    });

  event1(42);

  Deferred<void(const string&)> event2 =
    executor.defer([&receiver](const string& s) {
      return receiver.event2(s);
    });

  event2("event2");

  while (!event1Called);
  while (!event2Called);
}


class RemoteProcess : public Process<RemoteProcess>
{
public:
  RemoteProcess()
  {
    install("handler", &RemoteProcess::handler);
  }

  MOCK_METHOD2(handler, void(const UPID&, const string&));
};


TEST(ProcessTest, Remote)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  RemoteProcess process;
  spawn(process);

  Future<Nothing> handler;
  EXPECT_CALL(process, handler(_, _))
    .WillOnce(FutureSatisfy(&handler));

  Try<Socket> create = Socket::create();
  ASSERT_SOME(create);

  Socket socket = create.get();

  AWAIT_READY(socket.connect(process.self().address));

  Message message;
  message.name = "handler";
  message.from = UPID();
  message.to = process.self();

  const string data = MessageEncoder::encode(&message);

  AWAIT_READY(socket.send(data));

  AWAIT_READY(handler);

  terminate(process);
  wait(process);
}


// Like the 'remote' test but uses http::post.
TEST(ProcessTest, Http1)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  RemoteProcess process;
  spawn(process);

  Future<UPID> pid;
  Future<string> body;
  EXPECT_CALL(process, handler(_, _))
    .WillOnce(DoAll(FutureArg<0>(&pid),
                    FutureArg<1>(&body)));

  http::Headers headers;
  headers["User-Agent"] = "libprocess/";

  Future<http::Response> response =
    http::post(process.self(), "handler", headers, "hello world");

  AWAIT_READY(body);
  ASSERT_EQ("hello world", body.get());

  AWAIT_READY(pid);
  ASSERT_EQ(UPID(), pid.get());

  terminate(process);
  wait(process);
}


// Like 'http1' but using a 'Libprocess-From' header.
TEST(ProcessTest, Http2)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  RemoteProcess process;
  spawn(process);

  // Create a receiving socket so we can get messages back.
  Try<Socket> create = Socket::create();
  ASSERT_SOME(create);

  Socket socket = create.get();

  ASSERT_SOME(socket.bind(Address()));

  // Create a UPID for 'Libprocess-From' based on the IP and port we
  // got assigned.
  Try<Address> address = socket.address();
  ASSERT_SOME(address);

  UPID from("", address.get());

  ASSERT_SOME(socket.listen(1));

  Future<UPID> pid;
  Future<string> body;
  EXPECT_CALL(process, handler(_, _))
    .WillOnce(DoAll(FutureArg<0>(&pid),
                    FutureArg<1>(&body)));

  http::Headers headers;
  headers["Libprocess-From"] = stringify(from);

  Future<http::Response> response =
    http::post(process.self(), "handler", headers, "hello world");

  AWAIT_READY(response);
  ASSERT_EQ(http::statuses[202], response.get().status);

  AWAIT_READY(body);
  ASSERT_EQ("hello world", body.get());

  AWAIT_READY(pid);
  ASSERT_EQ(from, pid.get());

  // Now post a message as though it came from the process.
  const string name = "reply";
  post(process.self(), from, name);

  // Accept the incoming connection.
  Future<Socket> accept = socket.accept();
  AWAIT_READY(accept);

  Socket client = accept.get();

  const string data = "POST /" + name + " HTTP/1.1";

  AWAIT_EXPECT_EQ(data, client.recv(data.size()));

  terminate(process);
  wait(process);
}


int foo()
{
  return 1;
}

int foo1(int a)
{
  return a;
}


int foo2(int a, int b)
{
  return a + b;
}


int foo3(int a, int b, int c)
{
  return a + b + c;
}


int foo4(int a, int b, int c, int d)
{
  return a + b + c + d;
}


void bar(int a)
{
  return;
}


TEST(ProcessTest, Async)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // Non-void functions with different no.of args.
  EXPECT_EQ(1, async(&foo).get());
  EXPECT_EQ(10, async(&foo1, 10).get());
  EXPECT_EQ(30, async(&foo2, 10, 20).get());
  EXPECT_EQ(60, async(&foo3, 10, 20, 30).get());
  EXPECT_EQ(100, async(&foo4, 10, 20, 30, 40).get());

  // Non-void function with a complex arg.
  int i = 42;
  EXPECT_EQ("42", async(&itoa2, &i).get());

  // Non-void function that returns a future.
  EXPECT_EQ("42", async(&itoa1, &i).get().get());
}


class FileServer : public Process<FileServer>
{
public:
  explicit FileServer(const string& _path)
    : path(_path) {}

  virtual void initialize()
  {
    provide("", path);
  }

  const string path;
};


TEST(ProcessTest, Provide)
{
  const Try<string> mkdtemp = os::mkdtemp();
  ASSERT_SOME(mkdtemp);

  const string LOREM_IPSUM =
      "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
      "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad "
      "minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip "
      "ex ea commodo consequat. Duis aute irure dolor in reprehenderit in "
      "voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur "
      "sint occaecat cupidatat non proident, sunt in culpa qui officia "
      "deserunt mollit anim id est laborum.";

  const string path = path::join(mkdtemp.get(), "lorem.txt");
  ASSERT_SOME(os::write(path, LOREM_IPSUM));

  FileServer server(path);
  PID<FileServer> pid = spawn(server);

  Future<http::Response> response = http::get(pid);

  AWAIT_READY(response);

  ASSERT_EQ(LOREM_IPSUM, response.get().body);

  terminate(server);
  wait(server);

  ASSERT_SOME(os::rmdir(path));
}


int baz(string s) { return 42; }

Future<int> bam(string s) { return 42; }


TEST(ProcessTest, Defers)
{
  {
    std::function<Future<int>(string)> f =
      defer(std::bind(baz, std::placeholders::_1));

    Deferred<Future<int>(string)> d =
      defer(std::bind(baz, std::placeholders::_1));

    Future<int> future = Future<string>().then(
        defer(std::bind(baz, std::placeholders::_1)));

    Future<int> future3 = Future<string>().then(
        std::bind(baz, std::placeholders::_1));

    Future<string>().then(std::function<int(string)>());
    Future<string>().then(std::function<int(void)>());

    Future<int> future11 = Future<string>().then(
        defer(std::bind(bam, std::placeholders::_1)));

    Future<int> future12 = Future<string>().then(
        std::bind(bam, std::placeholders::_1));

    std::function<Future<int>(string)> f2 =
      defer([](string s) { return baz(s); });

    Deferred<Future<int>(string)> d2 =
      defer([](string s) { return baz(s); });

    Future<int> future2 = Future<string>().then(
        defer([](string s) { return baz(s); }));

    Future<int> future4 = Future<string>().then(
        [](string s) { return baz(s); });

    Future<int> future5 = Future<string>().then(
        defer([](string s) -> Future<int> { return baz(s); }));

    Future<int> future6 = Future<string>().then(
        defer([](string s) { return Future<int>(baz(s)); }));

    Future<int> future7 = Future<string>().then(
        defer([](string s) { return bam(s); }));

    Future<int> future8 = Future<string>().then(
        [](string s) { return Future<int>(baz(s)); });

    Future<int> future9 = Future<string>().then(
        [](string s) -> Future<int> { return baz(s); });

    Future<int> future10 = Future<string>().then(
        [](string s) { return bam(s); });
  }

//   {
//     // CAN NOT DO IN CLANG!
//     std::function<void(string)> f =
//       defer(std::bind(baz, std::placeholders::_1));

//     std::function<int(string)> blah;
//     std::function<void(string)> blam = blah;

//     std::function<void(string)> f2 =
//       defer([](string s) { return baz(s); });
//   }

//   {
//     // CAN NOT DO WITH GCC OR CLANG!
//     std::function<int(int)> f =
//       defer(std::bind(baz, std::placeholders::_1));
//   }

  {
    std::function<Future<int>(void)> f =
      defer(std::bind(baz, "42"));

    std::function<Future<int>(void)> f2 =
      defer([]() { return baz("42"); });
  }

  {
    std::function<Future<int>(int)> f =
      defer(std::bind(baz, "42"));

    std::function<Future<int>(int)> f2 =
      defer([](int i) { return baz("42"); });
  }

  // Don't care about value passed from Future::then.
  {
    Future<int> future = Future<string>().then(
        defer(std::bind(baz, "42")));

    Future<int> future3 = Future<string>().then(
        std::bind(baz, "42"));

    Future<int> future11 = Future<string>().then(
        defer(std::bind(bam, "42")));

    Future<int> future12 = Future<string>().then(
        std::bind(bam, "42"));

    Future<int> future2 = Future<string>().then(
        defer([]() { return baz("42"); }));

    Future<int> future4 = Future<string>().then(
        []() { return baz("42"); });

    Future<int> future5 = Future<string>().then(
        defer([]() -> Future<int> { return baz("42"); }));

    Future<int> future6 = Future<string>().then(
        defer([]() { return Future<int>(baz("42")); }));

    Future<int> future7 = Future<string>().then(
        defer([]() { return bam("42"); }));

    Future<int> future8 = Future<string>().then(
        []() { return Future<int>(baz("42")); });

    Future<int> future9 = Future<string>().then(
        []() -> Future<int> { return baz("42"); });

    Future<int> future10 = Future<string>().then(
        []() { return bam("42"); });
  }

  struct Functor
  {
    int operator()(string) const { return 42; }
    int operator()() const { return 42; }
  } functor;

  Future<int> future13 = Future<string>().then(
      defer(functor));
}


TEST(ProcessTest, FromTry)
{
  Try<int> t = 1;
  Future<int> future = t;

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ(1, future.get());

  t = Error("error");
  future = t;

  ASSERT_TRUE(future.isFailed());
}


class PercentEncodedIDProcess : public Process<PercentEncodedIDProcess>
{
public:
  PercentEncodedIDProcess()
    : ProcessBase("id(42)") {}

  virtual void initialize()
  {
    install("handler1", &Self::handler1);
    route("/handler2", None(), &Self::handler2);
  }

  MOCK_METHOD2(handler1, void(const UPID&, const string&));
  MOCK_METHOD1(handler2, Future<http::Response>(const http::Request&));
};


TEST(ProcessTest, PercentEncodedURLs)
{
  PercentEncodedIDProcess process;
  spawn(process);

  // Construct the PID using percent-encoding.
  UPID pid("id%2842%29", process.self().address);

  // Mimic a libprocess message sent to an installed handler.
  Future<Nothing> handler1;
  EXPECT_CALL(process, handler1(_, _))
    .WillOnce(FutureSatisfy(&handler1));

  http::Headers headers;
  headers["User-Agent"] = "libprocess/";

  Future<http::Response> response = http::post(pid, "handler1", headers);

  AWAIT_READY(handler1);

  // Now an HTTP request.
  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  terminate(process);
  wait(process);
}


class HTTPEndpointProcess : public Process<HTTPEndpointProcess>
{
public:
  explicit HTTPEndpointProcess(const std::string& id)
    : ProcessBase(id) {}

  virtual void initialize()
  {
    route(
        "/handler1",
        None(),
        &HTTPEndpointProcess::handler1);
    route(
        "/handler2",
        None(),
        &HTTPEndpointProcess::handler2);
    route(
        "/handler3",
        None(),
        &HTTPEndpointProcess::handler3);
  }

  MOCK_METHOD1(handler1, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(handler2, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(handler3, Future<http::Response>(const http::Request&));
};


// Sets firewall rules which disable endpoints on a process and then
// attempts to connect to those endpoints.
TEST(ProcessTest, FirewallDisablePaths)
{
  const string id = "testprocess";

  // TODO(arojas): Add initilization list construction when available.
  hashset<string> endpoints;
  endpoints.insert(path::join("", id, "handler1"));
  endpoints.insert(path::join("", id, "handler2/nested"));
  // Patterns are not supported, so this should do nothing.
  endpoints.insert(path::join("", id, "handler3/*"));

  process::firewall::install(
      {Owned<FirewallRule>(new DisabledEndpointsFirewallRule(endpoints))});

  HTTPEndpointProcess process(id);

  PID<HTTPEndpointProcess> pid = spawn(process);

  // Test call to a disabled endpoint.
  Future<http::Response> response = http::get(pid, "handler1");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[403], response.get().status);

  // Test call to a non disabled endpoint.
  // Substrings should not match.
  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  // Test nested endpoints. Full paths needed for match.
  response = http::get(pid, "handler2/nested");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[403], response.get().status);

  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2/nested/path");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  EXPECT_CALL(process, handler3(_))
    .WillOnce(Return(http::OK()));

  // Test a wildcard rule. Since they are not supported, it must have
  // no effect at all.
  response = http::get(pid, "handler3");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  EXPECT_CALL(process, handler3(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler3/nested");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  terminate(process);
  wait(process);
}


// Test that firewall rules can be changed by changing the vector.
// An empty vector should allow all paths.
TEST(ProcessTest, FirewallUninstall)
{
  const string id = "testprocess";

  // TODO(arojas): Add initilization list construction when available.
  hashset<string> endpoints;
  endpoints.insert(path::join("", id, "handler1"));
  endpoints.insert(path::join("", id, "handler2"));

  process::firewall::install(
      {Owned<FirewallRule>(new DisabledEndpointsFirewallRule(endpoints))});

  HTTPEndpointProcess process(id);

  PID<HTTPEndpointProcess> pid = spawn(process);

  Future<http::Response> response = http::get(pid, "handler1");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[403], response.get().status);

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[403], response.get().status);

  process::firewall::install({});

  EXPECT_CALL(process, handler1(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler1");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::statuses[200], response.get().status);

  terminate(process);
  wait(process);
}

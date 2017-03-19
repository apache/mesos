// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <errno.h>
#include <time.h>

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

#include <gmock/gmock.h>

#ifndef __WINDOWS__
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif // __WINDOWS__

#include <atomic>
#include <sstream>
#include <string>
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
#include <process/reap.hpp>
#include <process/run.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stopwatch.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/killtree.hpp>
#include <stout/os/write.hpp>

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
using process::Future;
using process::Message;
using process::MessageEncoder;
using process::MessageEvent;
using process::Owned;
using process::PID;
using process::Process;
using process::ProcessBase;
using process::run;
using process::Subprocess;
using process::TerminateEvent;
using process::Time;
using process::UPID;

using process::firewall::DisabledEndpointsFirewallRule;
using process::firewall::FirewallRule;

using process::network::inet::Address;
using process::network::inet::Socket;

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
  Owned<Event> event(new TerminateEvent(UPID()));
  EXPECT_FALSE(event->is<MessageEvent>());
  EXPECT_FALSE(event->is<ExitedEvent>());
  EXPECT_TRUE(event->is<TerminateEvent>());
}


class SpawnProcess : public Process<SpawnProcess>
{
public:
  MOCK_METHOD0(initialize, void());
  MOCK_METHOD0(finalize, void());
};


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Spawn)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SpawnProcess process;

  EXPECT_CALL(process, initialize());

  EXPECT_CALL(process, finalize());

  PID<SpawnProcess> pid = spawn(process);

  ASSERT_FALSE(!pid);

  ASSERT_FALSE(wait(pid, Seconds(0)));

  terminate(pid);
  wait(pid);
}


class DispatchProcess : public Process<DispatchProcess>
{
public:
  MOCK_METHOD0(func0, void());
  MOCK_METHOD1(func1, bool(bool));
  MOCK_METHOD1(func2, Future<bool>(bool));
  MOCK_METHOD1(func3, int(int));
  MOCK_METHOD2(func4, Future<bool>(bool, int));
};


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Dispatch)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchProcess process;

  EXPECT_CALL(process, func0());

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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Defer1)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchProcess process;

  EXPECT_CALL(process, func0());

  EXPECT_CALL(process, func1(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func2(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func4(_, _))
    .WillRepeatedly(ReturnArg<0>());

  PID<DispatchProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  {
    Deferred<void()> func0 =
      defer(pid, &DispatchProcess::func0);
    func0();
  }

  Future<bool> future;

  {
    Deferred<Future<bool>()> func1 =
      defer(pid, &DispatchProcess::func1, true);
    future = func1();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>()> func2 =
      defer(pid, &DispatchProcess::func2, true);
    future = func2();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>()> func4 =
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Defer2)
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Defer3)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  std::atomic_bool bool1(false);
  std::atomic_bool bool2(false);

  Deferred<void(bool)> set1 =
    defer([&bool1](bool b) { bool1.store(b); });

  set1(true);

  Deferred<void(bool)> set2 =
    defer([&bool2](bool b) { bool2.store(b); });

  set2(true);

  while (bool1.load() == false);
  while (bool2.load() == false);
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Handlers)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  Future<Nothing> func;
  EXPECT_CALL(process, func(_, _))
    .WillOnce(FutureSatisfy(&func));

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  post(pid, "func");

  AWAIT_READY(func);

  terminate(pid, false);
  wait(pid);
}


// Tests DROP_MESSAGE and DROP_DISPATCH and in particular that an
// event can get dropped before being processed.
// NOTE: GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Expect)
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
// NOTE: GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Action)
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Inheritance)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DerivedProcess process;

  EXPECT_CALL(process, func())
    .Times(2);

  EXPECT_CALL(process, foo());

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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Thunk)
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Delegate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DelegateeProcess delegatee;
  DelegatorProcess delegator(delegatee.self());

  Future<Nothing> func;
  EXPECT_CALL(delegatee, func(_, _))
    .WillOnce(FutureSatisfy(&func));

  spawn(&delegator);
  spawn(&delegatee);

  post(delegator.self(), "func");

  AWAIT_READY(func);

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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Delay)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  std::atomic_bool timeoutCalled(false);

  TimeoutProcess process;

  EXPECT_CALL(process, timeout())
    .WillOnce(Assign(&timeoutCalled, true));

  spawn(process);

  delay(Seconds(5), process.self(), &TimeoutProcess::timeout);

  Clock::advance(Seconds(5));

  while (timeoutCalled.load() == false);

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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Order)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  TimeoutProcess process1;

  std::atomic_bool timeoutCalled(false);

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

  while (timeoutCalled.load() == false);

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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Donate)
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
  explicit ExitedProcess(const UPID& _pid) : pid(_pid) {}

  virtual void initialize()
  {
    link(pid);
  }

  MOCK_METHOD1(exited, void(const UPID&));

private:
  const UPID pid;
};


TEST(ProcessTest, Exited)
{
  UPID pid = spawn(new ProcessBase(), true);

  ExitedProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);

  terminate(pid);

  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}


TEST(ProcessTest, InjectExited)
{
  UPID pid = spawn(new ProcessBase(), true);

  ExitedProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);

  inject::exited(pid, process.self());

  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}


class MessageEventProcess : public Process<MessageEventProcess>
{
public:
  MOCK_METHOD1(visit, void(const MessageEvent&));
};


class ProcessRemoteLinkTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    // Spawn a process to coordinate with the subprocess (test-linkee).
    // The `test-linkee` will send us a message when it has finished
    // initializing and is itself ready to receive messages.
    MessageEventProcess coordinator;
    spawn(coordinator);

    Future<MessageEvent> event;
    EXPECT_CALL(coordinator, visit(_))
      .WillOnce(FutureArg<0>(&event));

    Try<Subprocess> s = process::subprocess(
        path::join(BUILD_DIR, "test-linkee") +
          " '" + stringify(coordinator.self()) + "'");
    ASSERT_SOME(s);
    linkee = s.get();

    // Wait until the subprocess sends us a message.
    AWAIT_ASSERT_READY(event);

    // Save the PID of the linkee.
    pid = event->message->from;

    terminate(coordinator);
    wait(coordinator);
  }

  // Helper method to quickly reap the `linkee`.
  // Subprocesses are reaped (via a non-blocking `waitpid` call) on
  // a regular interval. We can speed up the internal reaper by
  // advancing the clock.
  void reap_linkee()
  {
    if (linkee.isSome()) {
      bool paused = Clock::paused();

      Clock::pause();
      while (linkee->status().isPending()) {
        Clock::advance(process::MAX_REAP_INTERVAL());
        Clock::settle();
      }

      if (!paused) {
        Clock::resume();
      }
    }
  }

  virtual void TearDown()
  {
    if (linkee.isSome()) {
      os::killtree(linkee->pid(), SIGKILL);
      reap_linkee();
      linkee = None();
    }
  }

public:
  Option<Subprocess> linkee;
  UPID pid;
};


// Verifies that linking to a remote process will correctly detect
// the associated `ExitedEvent`.
// TODO(hausdorff): Test fails on Windows. Fix and enable. Linkee never sends a
// message because "no such program exists". See MESOS-5941.
TEST_F_TEMP_DISABLED_ON_WINDOWS(ProcessRemoteLinkTest, RemoteLink)
{
  // Link to the remote subprocess.
  ExitedProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);

  os::killtree(linkee->pid(), SIGKILL);
  reap_linkee();
  linkee = None();

  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}


class RemoteLinkTestProcess : public Process<RemoteLinkTestProcess>
{
public:
  explicit RemoteLinkTestProcess(const UPID& pid) : pid(pid) {}

  void linkup()
  {
    link(pid);
  }

  void relink()
  {
    link(pid, RemoteConnection::RECONNECT);
  }

  void ping_linkee()
  {
    send(pid, "whatever", "", 0);
  }

  MOCK_METHOD1(exited, void(const UPID&));

private:
  const UPID pid;
};


// Verifies that calling `link` with "relink" semantics will have the
// same behavior as `link` with "normal" semantics, when there is no
// existing persistent connection.
// TODO(hausdorff): Test fails on Windows. Fix and enable. Linkee never sends a
// message because "no such program exists". See MESOS-5941.
TEST_F_TEMP_DISABLED_ON_WINDOWS(ProcessRemoteLinkTest, RemoteRelink)
{
  RemoteLinkTestProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);
  process.relink();

  os::killtree(linkee->pid(), SIGKILL);
  reap_linkee();
  linkee = None();

  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}


// Verifies that linking and relinking a process will retain monitoring
// on the linkee.
// TODO(hausdorff): Test fails on Windows. Fix and enable. Linkee never sends a
// message because "no such program exists". See MESOS-5941.
TEST_F_TEMP_DISABLED_ON_WINDOWS(ProcessRemoteLinkTest, RemoteLinkRelink)
{
  RemoteLinkTestProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);
  process.linkup();
  process.relink();

  os::killtree(linkee->pid(), SIGKILL);
  reap_linkee();
  linkee = None();

  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}


// Verifies that relinking a remote process will not affect the
// monitoring of the process by other linkers.
// TODO(hausdorff): Test fails on Windows. Fix and enable. Linkee never sends a
// message because "no such program exists". See MESOS-5941.
TEST_F_TEMP_DISABLED_ON_WINDOWS(ProcessRemoteLinkTest, RemoteDoubleLinkRelink)
{
  ExitedProcess linker(pid);
  RemoteLinkTestProcess relinker(pid);

  Future<UPID> linkerExitedPid;
  Future<UPID> relinkerExitedPid;

  EXPECT_CALL(linker, exited(pid))
    .WillOnce(FutureArg<0>(&linkerExitedPid));
  EXPECT_CALL(relinker, exited(pid))
    .WillOnce(FutureArg<0>(&relinkerExitedPid));

  spawn(linker);
  spawn(relinker);

  relinker.linkup();
  relinker.relink();

  os::killtree(linkee->pid(), SIGKILL);
  reap_linkee();
  linkee = None();

  AWAIT_ASSERT_EQ(pid, linkerExitedPid);
  AWAIT_ASSERT_EQ(pid, relinkerExitedPid);

  terminate(linker);
  wait(linker);

  terminate(relinker);
  wait(relinker);
}


// Verifies that remote links will trigger an `ExitedEvent` if the link
// fails during socket creation. The test instigates a socket creation
// failure by hogging all available file descriptors.
TEST_F_TEMP_DISABLED_ON_WINDOWS(ProcessRemoteLinkTest, RemoteLinkLeak)
{
  RemoteLinkTestProcess relinker(pid);
  Future<UPID> relinkerExitedPid;

  EXPECT_CALL(relinker, exited(pid))
    .WillOnce(FutureArg<0>(&relinkerExitedPid));

  spawn(relinker);

  // Open enough sockets to fill up all available FDs.
  vector<Socket> fdHogs;
  while (true) {
    Try<Socket> hog = Socket::create();

    if (hog.isError()) {
      break;
    }

    fdHogs.push_back(hog.get());
  }

  relinker.linkup();

  AWAIT_ASSERT_EQ(pid, relinkerExitedPid);

  terminate(relinker);
  wait(relinker);
}


namespace process {

// Forward declare the `get_persistent_socket` function since we want
// to programatically mess with "link" FDs during tests.
Option<int> get_persistent_socket(const UPID& to);

} // namespace process {


// TODO(hausdorff): Test disabled temporarily because `SHUT_WR` does not exist
// on Windows. See MESOS-5817.
#ifndef __WINDOWS__
// Verifies that sending a message over a socket will fail if the
// link to the target is broken (i.e. closed) outside of the
// `SocketManager`s knowledge.
// Emulates the error behind MESOS-5576. In this case, the socket
// becomes "stale", but libprocess does not receive a TCP RST either.
// A `send` later will trigger a socket error and thereby discover
// the socket's staleness.
TEST_F(ProcessRemoteLinkTest, RemoteUseStaleLink)
{
  RemoteLinkTestProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);
  process.linkup();

  // Dig out the link from the `SocketManager`.
  Option<int> linkfd = get_persistent_socket(pid);
  ASSERT_SOME(linkfd);

  // Disable further writes on this socket without telling the
  // `SocketManager`!  This will cause a `send` to fail later.
  // NOTE: This is done in a loop as the `shutdown` call will fail
  // while the socket is connecting.
  Duration waited = Duration::zero();
  do {
    if (::shutdown(linkfd.get(), SHUT_WR) != 0) {
      // These errors are expected as we are racing against the code
      // responsible for setting up the persistent socket.
      ASSERT_TRUE(errno == EINPROGRESS || errno == ENOTCONN)
        << ErrnoError().message;
      continue;
    }

    break;
  } while (waited < Seconds(5));

  EXPECT_LE(waited, Seconds(5));

  ASSERT_TRUE(exitedPid.isPending());

  // Now try to send a message over the dead link.
  process.ping_linkee();

  // The dead link should be detected and trigger an `ExitedEvent`.
  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}
#endif // __WINDOWS__


// TODO(hausdorff): Test disabled temporarily because `SHUT_WR` does not exist
// on Windows. See MESOS-5817.
#ifndef __WINDOWS__
// Verifies that, in a situation where an existing remote link has become
// "stale", "relinking" prior to sending a message will lead to successful
// message passing. The existing remote link is broken in the same way as
// the test `RemoteUseStaleLink`.
TEST_F(ProcessRemoteLinkTest, RemoteStaleLinkRelink)
{
  RemoteLinkTestProcess process(pid);

  Future<UPID> exitedPid;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(FutureArg<0>(&exitedPid));

  spawn(process);
  process.linkup();

  // Dig out the link from the `SocketManager`.
  Option<int> linkfd = get_persistent_socket(pid);
  ASSERT_SOME(linkfd);

  // Disable further writes on this socket without telling the
  // `SocketManager`!  This would cause a `send` to fail later,
  // but this test will "relink" before calling `send`.
  // NOTE: This is done in a loop as the `shutdown` call will fail
  // while the socket is connecting.
  Duration waited = Duration::zero();
  do {
    if (::shutdown(linkfd.get(), SHUT_WR) != 0) {
      // These errors are expected as we are racing against the code
      // responsible for setting up the persistent socket.
      ASSERT_TRUE(errno == EINPROGRESS || errno == ENOTCONN)
        << ErrnoError().message;
      continue;
    }

    break;
  } while (waited < Seconds(5));

  EXPECT_LE(waited, Seconds(5));

  ASSERT_TRUE(exitedPid.isPending());

  // Call `link` again with the "relink" semantics.
  process.relink();

  // Now try to send a message over the new link.
  process.ping_linkee();

  // The message should trigger a suicide on the receiving end.
  // The linkee should suicide with a successful exit code.
  reap_linkee();
  AWAIT_ASSERT_READY(linkee->status());
  ASSERT_SOME_EQ(EXIT_SUCCESS, linkee->status().get());

  // We should also get the associated `ExitedEvent`.
  AWAIT_ASSERT_EQ(pid, exitedPid);

  terminate(process);
  wait(process);
}
#endif // __WINDOWS__


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
    calledDispatch.store(true);
  }

  std::atomic_bool calledDispatch;
};


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Settle)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();
  SettleProcess process;
  spawn(process);
  Clock::settle();
  ASSERT_TRUE(process.calledDispatch.load());
  terminate(process);
  wait(process);
  Clock::resume();
}


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Pid)
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Listener)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MultipleListenerProcess process;

  EXPECT_CALL(process, event1());

  EXPECT_CALL(process, event2());

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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Executor)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  std::atomic_bool event1Called(false);
  std::atomic_bool event2Called(false);

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

  while (event1Called.load() == false);
  while (event2Called.load() == false);
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


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Remote)
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


// Like the 'remote' test but uses http::connect.
// NOTE: GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Http1)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  RemoteProcess process;
  spawn(process);

  http::URL url = http::URL(
      "http",
      process.self().address.ip,
      process.self().address.port,
      process.self().id + "/handler");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection = connect.get();

  Future<UPID> pid;
  Future<string> body;
  EXPECT_CALL(process, handler(_, _))
    .WillOnce(DoAll(FutureArg<0>(&pid),
                    FutureArg<1>(&body)));

  http::Request request;
  request.method = "POST";
  request.url = url;
  request.headers["User-Agent"] = "libprocess/";
  request.body = "hello world";

  // Send the libprocess request. Note that we will not
  // receive a 202 due to the use of the `User-Agent`
  // header, therefore we need to explicitly disconnect!
  Future<http::Response> response = connection.send(request);

  AWAIT_READY(body);
  ASSERT_EQ("hello world", body.get());

  AWAIT_READY(pid);
  ASSERT_EQ(UPID(), pid.get());

  EXPECT_TRUE(response.isPending());

  AWAIT_READY(connection.disconnect());

  terminate(process);
  wait(process);
}


// Like 'http1' but uses the 'Libprocess-From' header. We can
// also use http::post here since we expect a 202 response.
// NOTE: GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Http2)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  RemoteProcess process;
  spawn(process);

  // Create a receiving socket so we can get messages back.
  Try<Socket> create = Socket::create();
  ASSERT_SOME(create);

  Socket socket = create.get();

  ASSERT_SOME(socket.bind(Address::ANY_ANY()));

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
  ASSERT_EQ(http::Status::ACCEPTED, response->code);
  ASSERT_EQ(http::Status::string(http::Status::ACCEPTED),
            response->status);

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


static int foo()
{
  return 1;
}


static int foo1(int a)
{
  return a;
}


static int foo2(int a, int b)
{
  return a + b;
}


static int foo3(int a, int b, int c)
{
  return a + b + c;
}


static int foo4(int a, int b, int c, int d)
{
  return a + b + c + d;
}


static Future<string> itoa1(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


static string itoa2(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


// GTEST_IS_THREADSAFE is not defined on Windows. See MESOS-5903.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Async)
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


// TODO(hausdorff): Enable test when `os::rmdir` is semantically equivalent to
// the POSIX version. In this case, it behaves poorly when we try to use it to
// delete a file instead of a directory. See MESOS-5942.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, Provide)
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


static int baz(string s) { return 42; }


static Future<int> bam(string s) { return 42; }


// MSVC can't compile the call to std::invoke.
#ifndef __WINDOWS__
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
    Future<string>().then(std::function<int()>());

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
//     // CANNOT DO IN CLANG!
//     std::function<void(string)> f =
//       defer(std::bind(baz, std::placeholders::_1));

//     std::function<int(string)> blah;
//     std::function<void(string)> blam = blah;

//     std::function<void(string)> f2 =
//       defer([](string s) { return baz(s); });
//   }

//   {
//     // CANNOT DO WITH GCC OR CLANG!
//     std::function<int(int)> f =
//       defer(std::bind(baz, std::placeholders::_1));
//   }

  {
    std::function<Future<int>()> f =
      defer(std::bind(baz, "42"));

    std::function<Future<int>()> f2 =
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
#endif // __WINDOWS__


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
  http::URL url = http::URL(
      "http",
      process.self().address.ip,
      process.self().address.port,
      http::encode(process.self().id) + "/handler1");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection = connect.get();

  // Mimic a libprocess message sent to an installed handler.
  Future<Nothing> handler1;
  EXPECT_CALL(process, handler1(_, _))
    .WillOnce(FutureSatisfy(&handler1));

  http::Request request;
  request.method = "POST";
  request.url = url;
  request.headers["User-Agent"] = "libprocess/";

  // Send the libprocess request. Note that we will not
  // receive a 202 due to the use of the `User-Agent`
  // header, therefore we need to explicitly disconnect!
  Future<http::Response> response = connection.send(request);

  AWAIT_READY(handler1);
  EXPECT_TRUE(response.isPending());

  AWAIT_READY(connection.disconnect());

  // Now an HTTP request.
  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  // Construct the PID using percent-encoding.
  UPID pid(http::encode(process.self().id), process.self().address);

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK), response->status);

  terminate(process);
  wait(process);
}


class HTTPEndpointProcess : public Process<HTTPEndpointProcess>
{
public:
  explicit HTTPEndpointProcess(const string& id)
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
// TODO(hausdorff): Routing logic is broken on Windows. Fix and enable test. In
// this case, we fail to set up the firewall routes. See MESOS-5904.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, FirewallDisablePaths)
{
  const string id = "testprocess";

  hashset<string> endpoints = {
    path::join("", id, "handler1"),
    path::join("", id, "handler2/nested"),
    // Patterns are not supported, so this should do nothing.
    path::join("", id, "handler3/*")
  };

  process::firewall::install(
      {Owned<FirewallRule>(new DisabledEndpointsFirewallRule(endpoints))});

  HTTPEndpointProcess process(id);

  PID<HTTPEndpointProcess> pid = spawn(process);

  // Test call to a disabled endpoint.
  Future<http::Response> response = http::get(pid, "handler1");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::FORBIDDEN, response->code);
  EXPECT_EQ(http::Status::string(http::Status::FORBIDDEN),
            response->status);

  // Test call to a non disabled endpoint.
  // Substrings should not match.
  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK),
            response->status);

  // Test nested endpoints. Full paths needed for match.
  response = http::get(pid, "handler2/nested");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::FORBIDDEN, response->code);
  EXPECT_EQ(http::Status::string(http::Status::FORBIDDEN),
            response->status);

  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2/nested/path");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK),
            response->status);

  EXPECT_CALL(process, handler3(_))
    .WillOnce(Return(http::OK()));

  // Test a wildcard rule. Since they are not supported, it must have
  // no effect at all.
  response = http::get(pid, "handler3");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK),
            response->status);

  EXPECT_CALL(process, handler3(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler3/nested");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK),
            response->status);

  terminate(process);
  wait(process);
}


// Test that firewall rules can be changed by changing the vector.
// An empty vector should allow all paths.
// TODO(hausdorff): Routing logic is broken on Windows. Fix and enable test. In
// this case, we fail to set up the firewall routes. See MESOS-5904.
TEST_TEMP_DISABLED_ON_WINDOWS(ProcessTest, FirewallUninstall)
{
  const string id = "testprocess";

  hashset<string> endpoints = {
    path::join("", id, "handler1"),
    path::join("", id, "handler2")
  };

  process::firewall::install(
      {Owned<FirewallRule>(new DisabledEndpointsFirewallRule(endpoints))});

  HTTPEndpointProcess process(id);

  PID<HTTPEndpointProcess> pid = spawn(process);

  Future<http::Response> response = http::get(pid, "handler1");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::FORBIDDEN, response->code);
  EXPECT_EQ(http::Status::string(http::Status::FORBIDDEN),
            response->status);

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::FORBIDDEN, response->code);
  EXPECT_EQ(http::Status::string(http::Status::FORBIDDEN),
            response->status);

  process::firewall::install({});

  EXPECT_CALL(process, handler1(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler1");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK),
            response->status);

  EXPECT_CALL(process, handler2(_))
    .WillOnce(Return(http::OK()));

  response = http::get(pid, "handler2");

  AWAIT_READY(response);
  EXPECT_EQ(http::Status::OK, response->code);
  EXPECT_EQ(http::Status::string(http::Status::OK),
            response->status);

  terminate(process);
  wait(process);
}

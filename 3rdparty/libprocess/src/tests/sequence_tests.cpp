#include <stdlib.h>

#include <gmock/gmock.h>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/gmock.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>

#include <stout/nothing.hpp>

using namespace process;

using testing::_;
using testing::Return;

class TestProcess : public Process<TestProcess>
{
public:
  Future<Nothing> foo()
  {
    return dispatch(self(), &Self::_foo);
  }

  Future<Nothing> _foo()
  {
    return dispatch(self(), &Self::__foo);
  }

  Future<Nothing> __foo()
  {
    return promise.future();
  }

  Nothing bar()
  {
    return Nothing();
  }

  Promise<Nothing> promise;
};


// The test verifies that callbacks are properly serialized by the
// Sequence object.
TEST(Sequence, Serialize)
{
  TestProcess process;
  spawn(process);

  Sequence sequence;

  Future<Nothing> bar = FUTURE_DISPATCH(_, &TestProcess::bar);

  lambda::function<Future<Nothing>(void)> f;

  f = defer(process, &TestProcess::foo);
  sequence.add(f);

  f = defer(process, &TestProcess::bar);
  sequence.add(f);

  // Flush the event queue to make sure that if the method 'bar' could
  // have been invoked, the future 'bar' would be satisfied before the
  // pending check below.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(bar.isPending());

  process.promise.set(Nothing());

  AWAIT_READY(bar);

  terminate(process);
  wait(process);
}


// Used to verify the discard semantics of Sequence.
class DiscardProcess : public Process<DiscardProcess>
{
public:
  Future<Nothing> func0() { return promise.future(); }

  MOCK_METHOD0(func1, Nothing(void));
  MOCK_METHOD0(func2, Nothing(void));
  MOCK_METHOD0(func3, Nothing(void));

  Promise<Nothing> promise;
};


// The tests verifies semantics of discarding one returned future.
TEST(Sequence, DiscardOne)
{
  DiscardProcess process;
  spawn(process);

  Sequence sequence;

  lambda::function<Future<Nothing>(void)> f;

  f = defer(process, &DiscardProcess::func0);
  Future<Nothing> f0 = sequence.add(f);

  f = defer(process, &DiscardProcess::func1);
  Future<Nothing> f1 = sequence.add(f);

  f = defer(process, &DiscardProcess::func2);
  Future<Nothing> f2 = sequence.add(f);

  f = defer(process, &DiscardProcess::func3);
  Future<Nothing> f3 = sequence.add(f);

  EXPECT_CALL(process, func1())
    .WillOnce(Return(Nothing()));

  EXPECT_CALL(process, func2())
    .Times(0);

  EXPECT_CALL(process, func3())
    .WillOnce(Return(Nothing()));

  // Flush the event queue to make sure that all callbacks have been
  // added to the sequence.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  f2.discard();

  // Start the sequence of calls.
  process.promise.set(Nothing());

  AWAIT_READY(f3);

  terminate(process);
  wait(process);
}


// The test verifies the semantics of deleting the Sequence object,
// which will result in all pending callbacks being discarded.
TEST(Sequence, DiscardAll)
{
  DiscardProcess process;
  spawn(process);

  Sequence* sequence = new Sequence();

  lambda::function<Future<Nothing>(void)> f;

  f = defer(process, &DiscardProcess::func0);
  Future<Nothing> f0 = sequence->add(f);

  f = defer(process, &DiscardProcess::func1);
  Future<Nothing> f1 = sequence->add(f);

  f = defer(process, &DiscardProcess::func2);
  Future<Nothing> f2 = sequence->add(f);

  f = defer(process, &DiscardProcess::func3);
  Future<Nothing> f3 = sequence->add(f);

  EXPECT_CALL(process, func1())
    .Times(0);

  EXPECT_CALL(process, func2())
    .Times(0);

  EXPECT_CALL(process, func3())
    .Times(0);

  // Flush the event queue to make sure that all callbacks have been
  // added to the sequence.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // This should cancel all pending callbacks.
  delete sequence;

  // Start the sequence of calls.
  process.promise.set(Nothing());

  AWAIT_READY(f0);
  AWAIT_DISCARDED(f1);
  AWAIT_DISCARDED(f2);
  AWAIT_DISCARDED(f3);

  terminate(process);
  wait(process);
}


class RandomProcess : public Process<RandomProcess>
{
public:
  RandomProcess() : value(0) {}

  Nothing verify()
  {
    EXPECT_EQ(0, value);
    return Nothing();
  }

  Future<Nothing> pulse()
  {
    value++;
    return dispatch(self(), &Self::_pulse);
  }

  Nothing _pulse()
  {
    value--;
    return Nothing();
  }

private:
  int value;
};


TEST(Sequence, Random)
{
  RandomProcess process;
  spawn(process);

  Sequence sequence;

  for (int i = 0; i < 100; i++) {
    lambda::function<Future<Nothing>(void)> f;

    // We randomly do 'pulse' and 'verify'. The idea here is that: if
    // sequence is not used, a 'verify' may see an intermediate
    // result of a 'pulse', in which case the value is not zero.
    if (::random() % 2 == 0) {
      f = defer(process, &RandomProcess::pulse);
    } else {
      f = defer(process, &RandomProcess::verify);
    }

    sequence.add(f);
  }

  Clock::pause();
  Clock::settle();
  Clock::resume();

  terminate(process);
  wait(process);
}

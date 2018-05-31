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

#include <gtest/gtest.h>

#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/state_machine.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

using process::Failure;
using process::Future;
using process::Promise;
using process::StateMachine;

using std::string;

enum class State
{
  INITIAL,
  STARTING,
  RUNNING,
  STOPPING,
  STOPPED,
  TERMINAL,
};


TEST(StateMachineTest, Transition)
{
  StateMachine<State> state(State::INITIAL);

  Try<int> i = state.transition<State::INITIAL, State::STARTING>(
      []() {
        return 42;
      });

  EXPECT_SOME_EQ(42, i);

  Try<Nothing> error = state.transition<State::INITIAL, State::STARTING>();

  EXPECT_ERROR(error);

  Try<Try<int>> t = state.transition<State::STARTING, State::RUNNING>(
      []() -> Try<int> {
        return 42;
      });

  ASSERT_SOME(t);
  EXPECT_SOME_EQ(42, t.get());

  error = state.transition<State::STARTING, State::RUNNING>();

  EXPECT_ERROR(error);

  t = state.transition<State::RUNNING, State::STOPPING>(
      []() -> Try<int> {
        return Error("Error");
      });

  ASSERT_SOME(t);
  ASSERT_ERROR(t.get());
  EXPECT_EQ("Error", t->error());

  error = state.transition<State::RUNNING, State::STOPPING>();

  EXPECT_ERROR(error);

  Promise<int> promise;

  Try<Future<int>> f = state.transition<State::STOPPING, State::STOPPED>(
      [&]() -> Future<int> {
        return promise.future();
      });

  ASSERT_SOME(f);
  EXPECT_TRUE(f->isPending());

  promise.set(42);

  AWAIT_EXPECT_EQ(42, f.get());

  error = state.transition<State::STOPPING, State::STOPPED>();

  EXPECT_ERROR(error);

  f = state.transition<State::STOPPED, State::TERMINAL>(
      [&]() -> Future<int> {
        return Failure("Failure");
      });

  ASSERT_SOME(f);
  ASSERT_TRUE(f->isFailed());
  EXPECT_EQ("Failure", f->failure());
}


TEST(StateMachineTest, Is)
{
  StateMachine<State> state(State::STARTING);

  EXPECT_TRUE(state.is<State::STARTING>());

  state.transition<State::STARTING, State::RUNNING>();

  EXPECT_TRUE(state.is<State::RUNNING>());
}


TEST(StateMachineTest, When)
{
  StateMachine<State> state(State::STARTING);

  AWAIT_READY(state.when<State::STARTING>());

  Future<Nothing> f = state.when<State::RUNNING>();

  EXPECT_TRUE(f.isPending());

  state.transition<State::STARTING, State::RUNNING>();

  AWAIT_READY(f);
}

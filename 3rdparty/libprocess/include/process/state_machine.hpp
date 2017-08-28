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

#ifndef __PROCESS_STATE_MACHINE_HPP__
#define __PROCESS_STATE_MACHINE_HPP__

#include <string>

#include <process/future.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/result_of.hpp>
#include <stout/traits.hpp>
#include <stout/try.hpp>

namespace process {

// An asbtraction to help build state machines within an actor.
//
// Actors are natural state machines but there is still quite a bit of
// boilerplate that one needs to end up writing. For example, let's
// say you have the following actor:
//
//   class CoinOperatedTurnstile : public Process<CoinOperatedTurnstile>
//   {
//   public:
//     // Returns nothing if you've been able to deposit a coin.
//     Future<Nothing> deposit()
//     {
//       if (state != State::LOCKED) {
//         return Failure("Coin already deposited in turnstile");
//       }
//
//       state = State::UNLOCKED;
//
//       ... perform any side effects related to depositing a coin ...;
//
//       return Nothing();
//     }
//
//     // Returns nothing if you've been able to push through the turnstile.
//     Future<Nothing> push()
//     {
//       if (state != State::UNLOCKED) {
//         return Failure("Turnstile is locked");
//       }
//
//       state = State::LOCKED;
//
//       ... perform any side effects from opening the turnstile ...;
//
//       return Nothing();
//     }
//
//   private:
//     enum class State {
//       UNLOCKED,
//       LOCKED,
//     } state = State::LOCKED;
//   };
//
// With the `StateMachine` abstraction below this becomes:
//
//   class CoinOperatedTurnstile : public Process<CoinOperatedTurnstile>
//   {
//   public:
//     // Returns nothing if you've been able to deposit a coin.
//     Future<Nothing> deposit()
//     {
//       return state.transition<State::LOCKED, State::UNLOCKED>(
//         []() -> Future<Nothing> {
//           ... perform any side effects related to depositing a coin ...;
//         },
//         "Coin already deposited in turnstile");
//     }
//
//     // Returns nothing if you've been able to push through the turnstile.
//     Future<Nothing> push()
//     {
//       return state.transition<State::UNLOCKED, State::LOCKED>(
//         []() -> Future<Nothing> {
//           ... perform any side effects from opening the turnstile ...;
//         },
//         "Turnstile is locked");
//     }
//
//   private:
//     enum class State {
//       UNLOCKED,
//       LOCKED,
//     };
//
//     StateMachine<State> state = State::LOCKED;
//   };
//
//
//  *** Transition Semantics ***
//
// The semantics of `StateMachine::transition()` are:
//
//   (1) Transition the state if the state is currently in the
//   specified "from" state. If not, return an `Error()`.
//
//   (2) Notify anyone waiting on that state transition (i.e., anyone
//   that called `StateMachine::when()`).
//
//   (3) Invoke the side effect function (if provided).
//
//
//  *** Asynchronous Transitions ***
//
// In many circumstances you'll need to perform some work, often
// asynchronously, in order to transition to a particular state. For
// example, consider some actor that needs to perform some asynchronous
// action in order to transition from a "running" state to a
// "stopped" state. To do that you'll need to introduce an
// intermediate state and _only_ complete the transition after the
// asynchronous action has completed. For example:
//
//   // Returns nothing once we have stopped.
//   Future<Nothing> stop()
//   {
//     return state.transition<State::RUNNING, State::STOPPING>(
//         []() {
//           return asynchronously_stop()
//             .then([]() -> Future<Nothing> {
//               return state.transition<State::STOPPING, State::STOPPED>();
//             });
//         });
//   }
//
// It can be tempting to skip this pattern even when you're not
// calling any asynchronous function, but be careful because you
// shouldn't be peforming ANY action to perform a transition without
// first changing the state to reflect the fact that you're doing the
// transition.
//
// The rule of thumb is this: if you need to perform _ANY_ action
// (synchronous or asynchronous) to transition to a state you _MUST_
// introduce an intermediate state to represent that you're performing
// that transition.
template <typename State>
class StateMachine
{
public:
  StateMachine(State initial) : state(initial) {}

  template <State from, State to, typename F>
  Try<typename result_of<F()>::type> transition(
      F&& f,
      Option<std::string>&& message = None())
  {
    if (state != from) {
      return Error(message.getOrElse("Invalid current state"));
    }

    state = to;

    foreach (Promise<Nothing>& promise, promises[state]) {
      promise.set(Nothing());
    }

    promises[state].clear();

    return f();
  }

  template <State from, State to>
  Try<Nothing> transition(Option<std::string>&& message = None())
  {
    return transition<from, to>([]() { return Nothing(); }, std::move(message));
  }

  template <State s>
  bool is() const
  {
    return state == s;
  }

  template <State s>
  Future<Nothing> when()
  {
    if (state != s) {
      promises[s].emplace_back();
      return promises[s].back().future();
    }

    return Nothing();
  }

private:
  State state;

  hashmap<State, std::vector<Promise<Nothing>>> promises;
};

}  // namespace process {

#endif // __PROCESS_STATE_MACHINE_HPP__

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

#ifndef __PROCESS_SEQUENCE_HPP__
#define __PROCESS_SEQUENCE_HPP__

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>

namespace process {

// Forward declaration.
class SequenceProcess;

// Provides an abstraction that serializes the execution of a sequence
// of callbacks.
class Sequence
{
public:
  Sequence(const std::string& id = "sequence");
  ~Sequence();

  // Registers a callback that will be invoked when all the futures
  // returned by the previously registered callbacks are in
  // non-pending status (i.e., ready, discarded or failed). Due to
  // these semantics, we should avoid registering a callback which
  // returns a future that could be in non-pending status for a long
  // time because it will prevent all the subsequent callbacks from
  // being invoked. This is analogous to the requirement that we want
  // to avoid invoking blocking function in a libprocess handler. A
  // user is allowed to cancel a registered callback by discarding the
  // returned future. Other callbacks in this sequence will NOT be
  // affected. The subsequent callbacks will not be invoked until the
  // future is actually DISCARDED.
  template <typename T>
  Future<T> add(const lambda::function<Future<T>()>& callback);

private:
  // Not copyable, not assignable.
  Sequence(const Sequence&);
  Sequence& operator=(const Sequence&);

  SequenceProcess* process;
};


class SequenceProcess : public Process<SequenceProcess>
{
public:
  SequenceProcess(const std::string& id)
    : ProcessBase(ID::generate(id)),
      last(Nothing()) {}

  template <typename T>
  Future<T> add(const lambda::function<Future<T>()>& callback)
  {
    // This is the future that is used to notify the next callback
    // (denoted by 'N' in the following graph).
    Owned<Promise<Nothing>> notifier(new Promise<Nothing>());

    // This is the future that will be returned to the user (denoted
    // by 'F' in the following graph).
    Owned<Promise<T>> promise(new Promise<T>());

    // We use a graph to show how we hook these futures. Each box in
    // the graph represents a future. As mentioned above, 'F' denotes
    // a future that will be returned to the user, and 'N' denotes a
    // future that is used for notifying the next callback. Each arrow
    // represents a "notification" relation. We will explain in detail
    // what "notification" means in the following.
    //
    // 'last'                 'last'                            'last'
    //   |                      |                                 |
    //   v                      v                                 v
    // +---+       +---+      +---+       +---+      +---+      +---+
    // | N |       | N |--+   | N |       | N |--+   | N |--+   | N |
    // +---+       +---+  |   +---+       +---+  |   +---+  |   +---+
    //        ==>         |     ^    ==>         |     ^    |     ^
    //                    |     |                |     |    |     |
    //                    |   +---+              |   +---+  |   +---+
    //                    +-->| F |              +-->| F |  +-->| F |
    //                        +---+                  +---+      +---+
    //
    // Initial =>  Added one callback  =>    Added two callbacks.

    // Setup the "notification" from 'F' to 'N' so that when a
    // callback is done, signal the notifier ('N').
    promise->future().onAny(lambda::bind(&completed, notifier));

    // Setup the "notification" from previous 'N' to 'F' so that when
    // a notifier ('N') is set (indicating the previous callback has
    // completed), invoke the next callback ('F') in the sequence.
    last.onAny(lambda::bind(&notified<T>, promise, callback));

    // In the following, we setup the hooks so that if this sequence
    // process is being terminated, all pending callbacks will be
    // discarded. We use weak futures here to avoid cyclic dependencies.

    // Discard the future associated with this notifier.
    //
    // NOTE: When we discard the notifier future, any `onDiscard()` callbacks
    // registered on `promise->future` will be invoked, but `onDiscard`
    // callbacks registered on the future returned by `add()` will NOT be
    // invoked. This is because currently discards do not propagate through
    // `dispatch()`. In other words, users should be careful when registering
    // `onDiscard` callbacks on the returned future.
    //
    // TODO(*): Propagate `onDiscard` through `dispatch`.
    notifier->future().onDiscard(
        lambda::bind(
            &internal::discard<T>,
            WeakFuture<T>(promise->future())));

    // Discard the notifier associated with the previous future.
    notifier->future().onDiscard(
        lambda::bind(
            &internal::discard<Nothing>,
            WeakFuture<Nothing>(last)));

    // Update the 'last'.
    last = notifier->future();

    return promise->future();
  }

protected:
  void finalize() override
  {
    last.discard();

    // TODO(jieyu): Do we need to wait for the future of the last
    // callback to be in DISCARDED state?
  }

private:
  // Invoked when a callback is done.
  static void completed(Owned<Promise<Nothing>> notifier)
  {
    notifier->set(Nothing());
  }

  // Invoked when a notifier is set.
  template <typename T>
  static void notified(
      Owned<Promise<T>> promise,
      const lambda::function<Future<T>()>& callback)
  {
    if (promise->future().hasDiscard()) {
      // The user has shown the intention to discard this callback
      // (i.e., by calling future.discard()). As a result, we will
      // just skip this callback.
      promise->discard();
    } else {
      promise->associate(callback());
    }
  }

  Future<Nothing> last;
};


inline Sequence::Sequence(const std::string& id)
{
  process = new SequenceProcess(id);
  process::spawn(process);
}


inline Sequence::~Sequence()
{
  // We set `inject` to false so that the terminate message is added to the
  // end of the sequence actor message queue. This guarantees that all `add()`
  // calls which happened before the sequence destruction are processed.
  // See MESOS-8741.
  process::terminate(process, false);
  process::wait(process);
  delete process;
}


template <typename T>
Future<T> Sequence::add(const lambda::function<Future<T>()>& callback)
{
  return dispatch(process, &SequenceProcess::add<T>, callback);
}

} // namespace process {

#endif // __PROCESS_SEQUENCE_HPP__

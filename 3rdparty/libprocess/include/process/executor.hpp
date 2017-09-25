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

#ifndef __PROCESS_EXECUTOR_HPP__
#define __PROCESS_EXECUTOR_HPP__

#include <process/deferred.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/nothing.hpp>
#include <stout/result_of.hpp>

namespace process {

// Provides an abstraction that can take a standard function object
// and defer or asynchronously execute it without needing a process.
// Each converted function object will get execute serially with respect
// to one another when invoked.
class Executor
{
public:
  Executor() : process(ID::generate("__executor__"))
  {
    spawn(process);
  }

  ~Executor()
  {
    terminate(process);
    wait(process);
  }

  void stop()
  {
    terminate(process);

    // TODO(benh): Note that this doesn't wait because that could
    // cause a deadlock ... thus, the semantics here are that no more
    // dispatches will occur after this function returns but one may
    // be occurring concurrently.
  }

  template <typename F>
  _Deferred<F> defer(F&& f)
  {
    return _Deferred<F>(process.self(), std::forward<F>(f));
  }


private:
  // Not copyable, not assignable.
  Executor(const Executor&);
  Executor& operator=(const Executor&);

  ProcessBase process;


public:
  template <
      typename F,
      typename R = typename result_of<F()>::type,
      typename std::enable_if<!std::is_void<R>::value, int>::type = 0>
  auto execute(F&& f)
    -> decltype(dispatch(process, std::function<R()>(std::forward<F>(f))))
  {
    // NOTE: Currently we cannot pass a mutable lambda into `dispatch()`
    // because it would be captured by copy, so we convert `f` into a
    // `std::function` to bypass this restriction.
    return dispatch(process, std::function<R()>(std::forward<F>(f)));
  }

  // NOTE: This overload for `void` returns `Future<Nothing>` so we can
  // chain. This follows the same behavior as `async()`.
  template <
      typename F,
      typename R = typename result_of<F()>::type,
      typename std::enable_if<std::is_void<R>::value, int>::type = 0>
  Future<Nothing> execute(F&& f)
  {
    // NOTE: Currently we cannot pass a mutable lambda into `dispatch()`
    // because it would be captured by copy, so we convert `f` into a
    // `std::function` to bypass this restriction. This wrapper also
    // avoids `f` being evaluated when it is a nested bind.
    // TODO(chhsiao): Capture `f` by forwarding once we switch to C++14.
    return dispatch(
        process,
        std::bind(
            [](const std::function<R()>& f_) { f_(); return Nothing(); },
            std::function<R()>(std::forward<F>(f))));
  }
};


// Per thread executor pointer. We use a pointer to lazily construct the
// actual executor.
extern thread_local Executor* _executor_;

#define __executor__                                                    \
  (_executor_ == nullptr ? _executor_ = new Executor() : _executor_)

} // namespace process {

#endif // __PROCESS_EXECUTOR_HPP__

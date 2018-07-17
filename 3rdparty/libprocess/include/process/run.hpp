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

#ifndef __PROCESS_RUN_HPP__
#define __PROCESS_RUN_HPP__

#include <memory> // TODO(benh): Replace shared_ptr with unique_ptr.

#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/preprocessor.hpp>

namespace process {

namespace internal {

template <typename R>
class ThunkProcess : public Process<ThunkProcess<R>>
{
public:
  ThunkProcess(std::shared_ptr<lambda::function<R()>> _thunk,
               std::shared_ptr<Promise<R>> _promise)
    : ProcessBase(ID::generate("__thunk__")),
      thunk(_thunk),
      promise(_promise) {}

  ~ThunkProcess() override {}

protected:
  void serve(Event&& event) override
  {
    promise->set((*thunk)());
  }

private:
  std::shared_ptr<lambda::function<R()>> thunk;
  std::shared_ptr<Promise<R>> promise;
};

} // namespace internal {


template <typename R>
Future<R> run(R (*method)())
{
  std::shared_ptr<lambda::function<R()>> thunk(
      new lambda::function<R()>(
          lambda::bind(method)));

  std::shared_ptr<Promise<R>> promise(new Promise<R>());
  Future<R> future = promise->future();

  terminate(spawn(new internal::ThunkProcess<R>(thunk, promise), true));

  return future;
}


#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> run(                                                        \
      R (*method)(ENUM_PARAMS(N, P)),                                   \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    std::shared_ptr<lambda::function<R()>> thunk(                       \
        new lambda::function<R()>(                                      \
            lambda::bind(method, ENUM_PARAMS(N, a))));                  \
                                                                        \
    std::shared_ptr<Promise<R>> promise(new Promise<R>());              \
    Future<R> future = promise->future();                               \
                                                                        \
    terminate(spawn(new internal::ThunkProcess<R>(thunk, promise), true)); \
                                                                        \
    return future;                                                      \
  }

  REPEAT_FROM_TO(1, 13, TEMPLATE, _) // Args A0 -> A11.
#undef TEMPLATE

} // namespace process {

#endif // __PROCESS_RUN_HPP__

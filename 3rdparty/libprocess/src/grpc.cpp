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

#include <process/dispatch.hpp>
#include <process/grpc.hpp>
#include <process/id.hpp>

namespace process {
namespace grpc {

namespace client {

void Runtime::terminate()
{
  data->terminate();
}


Future<Nothing> Runtime::wait()
{
  return data->terminated.future();
}


Runtime::Data::Data()
  : process(ID::generate("__grpc_client__"))
{
  spawn(process);

  // The looper thread can only be created here since it need to happen
  // after `queue` is initialized.
  looper.reset(new std::thread(&Runtime::Data::loop, this));
}


Runtime::Data::~Data()
{
  terminate();
  process::wait(process);
}


void Runtime::Data::loop()
{
  void* tag;
  bool ok;

  while (queue.Next(&tag, &ok)) {
    // The returned callback object is managed by the `callback` shared
    // pointer, so if we get a regular event from the `CompletionQueue`,
    // then the object would be captured by the following lambda
    // dispatched to `process`; otherwise it would be reclaimed here.
    std::shared_ptr<lambda::function<void()>> callback(
        reinterpret_cast<lambda::function<void()>*>(tag));
    if (ok) {
      dispatch(process, [=] { (*callback)(); });
    }
  }

  dispatch(process, [this] {
    // NOTE: This is a blocking call. However, the thread is guaranteed
    // to be exiting, therefore the amount of blocking time should be
    // short (just like other syscalls we invoke).
    looper->join();
    // Terminate `process` after all events are drained.
    process::terminate(process, false);
    terminated.set(Nothing());
  });
}


void Runtime::Data::terminate()
{
  synchronized (lock) {
    if (!terminating) {
      terminating = true;
      queue.Shutdown();
    }
  }
}

} // namespace client {

} // namespace grpc {
} // namespace process {

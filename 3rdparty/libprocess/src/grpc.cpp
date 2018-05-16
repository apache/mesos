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

#include <process/grpc.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

namespace process {
namespace grpc {
namespace client {

void Runtime::terminate()
{
  dispatch(data->pid, &RuntimeProcess::terminate);
}


Future<Nothing> Runtime::wait()
{
  return data->terminated;
}


Runtime::RuntimeProcess::RuntimeProcess()
  : ProcessBase(ID::generate("__grpc_client__")), terminating(false) {}


Runtime::RuntimeProcess::~RuntimeProcess()
{
  CHECK(!looper);
}


void Runtime::RuntimeProcess::send(SendCallback callback)
{
  std::move(callback)(terminating, &queue);
}


void Runtime::RuntimeProcess::receive(ReceiveCallback callback)
{
  std::move(callback)();
}


void Runtime::RuntimeProcess::terminate()
{
  if (!terminating) {
    terminating = true;
    queue.Shutdown();
  }
}


Future<Nothing> Runtime::RuntimeProcess::wait()
{
  return terminated.future();
}


void Runtime::RuntimeProcess::initialize()
{
  // The looper thread can only be created here since it need to happen
  // after `queue` is initialized.
  CHECK(!looper);
  looper.reset(new std::thread(&RuntimeProcess::loop, this));
}


void Runtime::RuntimeProcess::finalize()
{
  CHECK(terminating) << "Runtime has not yet been terminated";

  // NOTE: This is a blocking call. However, the thread is guaranteed
  // to be exiting, therefore the amount of blocking time should be
  // short (just like other syscalls we invoke).
  looper->join();
  looper.reset();
  terminated.set(Nothing());
}


void Runtime::RuntimeProcess::loop()
{
  void* tag;
  bool ok;

  while (queue.Next(&tag, &ok)) {
    // Currently only unary RPCs are supported, so `ok` should always be true.
    // See: https://grpc.io/grpc/cpp/classgrpc_1_1_completion_queue.html#a86d9810ced694e50f7987ac90b9f8c1a // NOLINT
    CHECK(ok);

    // Obtain the tag as a `ReceiveCallback` and dispatch it to the runtime
    // process. The tag is then reclaimed here.
    ReceiveCallback* callback = reinterpret_cast<ReceiveCallback*>(tag);
    dispatch(self(), &RuntimeProcess::receive, std::move(*callback));
    delete callback;
  }

  // Terminate self after all events are drained.
  process::terminate(self(), false);
}


Runtime::Data::Data()
{
  RuntimeProcess* process = new RuntimeProcess();
  terminated = process->wait();
  pid = spawn(process, true);
}


Runtime::Data::~Data()
{
  dispatch(pid, &RuntimeProcess::terminate);
}

} // namespace client {
} // namespace grpc {
} // namespace process {

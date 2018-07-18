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

#include <event_loop.hpp>

#include <process/logging.hpp>
#include <process/once.hpp>

#include <stout/windows.hpp>

#include <stout/windows/os.hpp>

#include "windows/event_loop.hpp"
#include "windows/libwinio.hpp"

namespace process {

windows::EventLoop* libwinio_loop;

void EventLoop::initialize()
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return;
  }

  Try<windows::EventLoop*> try_loop = windows::EventLoop::create();
  if (try_loop.isError()) {
    LOG(FATAL) << "Failed to initialize Windows IOCP event loop";
  }

  libwinio_loop = try_loop.get();

  initialized->done();
}


void EventLoop::delay(
    const Duration& duration, const std::function<void()>& function)
{
  if (!libwinio_loop) {
    // TODO(andschwa): Remove this check, see MESOS-9097.
    LOG(FATAL) << "Windows IOCP event loop is not initialized";
  }

  libwinio_loop->launchTimer(duration, function);
}


double EventLoop::time()
{
  FILETIME filetime;
  ::GetSystemTimeAsFileTime(&filetime);
  return os::internal::windows_to_unix_epoch(filetime);
}


void EventLoop::run()
{
  if (!libwinio_loop) {
    // TODO(andschwa): Remove this check, see MESOS-9097.
    LOG(FATAL) << "Windows IOCP event loop is not initialized";
  }

  libwinio_loop->run();
}


void EventLoop::stop()
{
  if (!libwinio_loop) {
    // TODO(andschwa): Remove this check, see MESOS-9097.
    LOG(FATAL) << "Windows IOCP event loop is not initialized";
  }

  libwinio_loop->stop();
}

} // namespace process {

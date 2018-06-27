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

#ifndef __LIBWINIO_HPP__
#define __LIBWINIO_HPP__

#include <stout/lambda.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>

#include <process/address.hpp>
#include <process/future.hpp>

namespace process {
namespace windows {

class EventLoop
{
public:
  static Try<EventLoop*> create();

  // Run the event loop forever.
  Try<Nothing> run();

  // Signal the event loop to stop.
  Try<Nothing> stop();

  // Timer event that calls the callback after the timer has fired.
  Try<Nothing> launchTimer(
      const Duration& duration, const lambda::function<void()>& callback);

  // Register a handle for async IO.
  Try<Nothing> registerHandle(const int_fd& fd);

private:
  EventLoop(HANDLE iocp_handle);

  // Custom Deleter for a RAII handle.
  struct HandleDeleter
  {
    // `HANDLE` satisfies the C++ `NullablePointer` interface, since it's
    // a literal pointer. So we can use the `HANDLE` directly instead of
    // using a pointer to the `HANDLE`.
    // See http://en.cppreference.com/w/cpp/memory/unique_ptr for more info.
    typedef HANDLE pointer;
    void operator()(HANDLE h) { ::CloseHandle(h); }
  };

  std::unique_ptr<HANDLE, HandleDeleter> iocp_handle_;
};


// All of these functions do an asynchronous IO operation. The returned future
// can be discarded to cancel the operation.
Future<size_t> read(const int_fd& fd, void* buf, size_t size);

Future<size_t> write(const int_fd& fd, const void* buf, size_t size);

// Socket only functions.
Future<size_t> recv(const int_fd& fd, void* buf, size_t size);

Future<size_t> send(const int_fd& fd, const void* buf, size_t size);

Future<Nothing> accept(const int_fd& fd, const int_fd& accepted_socket);

Future<Nothing> connect(const int_fd& fd, const network::Address& address);

Future<size_t> sendfile(
    const int_fd& fd, const int_fd& file_fd, off_t offset, size_t size);

} // namespace windows {
} // namespace process {

#endif // __LIBWINIO_HPP__

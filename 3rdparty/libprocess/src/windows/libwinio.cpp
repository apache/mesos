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

#include <memory>

#include <stout/lambda.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/read.hpp>
#include <stout/os/sendfile.hpp>
#include <stout/os/write.hpp>

#include <stout/windows/error.hpp>

#include <process/address.hpp>
#include <process/network.hpp>
#include <process/process.hpp> // For process::initialize.

#include "windows/libwinio.hpp"

namespace process {
namespace windows {

// Key constants for IOCP `GetQueuedCompletionStatusEx`.
constexpr DWORD KEY_QUIT = 0;
constexpr DWORD KEY_IO = 1;
constexpr DWORD KEY_TIMER = 2;


// Definitions for handling for event loop timers.
struct TimerOverlapped
{
  HANDLE timer;
  LARGE_INTEGER time;
  lambda::function<void()> callback;
};


// This ia a `TimerAPCProc` callback function that is called when the timer
// set by `SetWaitableTimer` fires [1].  Note that `CALLBACK` is defined to be
// `__stdcall` in the Windows SDK, since the Win32 APIs use the stdcall
// calling convention.
//
// [1]: https://msdn.microsoft.com/en-us/library/windows/desktop/ms686786(v=vs.85).aspx // NOLINT(whitespace/line_length)
static void CALLBACK timer_apc(void* arg, DWORD timer_low, DWORD timer_high)
{
  TimerOverlapped* timer = reinterpret_cast<TimerOverlapped*>(arg);
  timer->callback();
  ::CloseHandle(timer->timer);
  delete timer;
}


// Definitions for handling event loop IO.
enum class IOType
{
  READ,
  WRITE,
  RECV,
  SEND,
  ACCEPT,
  CONNECT,
  SENDFILE
};

// Base overlapped struct that contains the required Win32 overlapped object,
// the `HANDLE` where the IO was performed and the type of IO performed.
struct IOOverlappedBase
{
  OVERLAPPED overlapped;
  HANDLE handle;
  IOType type;
};

// We keep `Promise<T>*` instead of `Promises<T>` so that we decouple the
// Promise from the overlapped object. This is because to support cancellation,
// we keep `std::shared_ptr` and `std::weak_ptr` copies of the overlapped
// object pointers in the `Promise` callbacks, so when the callbacks are
// cleaned up, we would also end up cleaning up the Promise itself, which can
// cause memory corruption.
template <typename T>
struct IOOverlapped
{
  IOOverlapped(const IOOverlappedBase& _base, Promise<T>* _promise)
    : base(_base), promise(_promise)
  {}

  IOOverlappedBase base;
  Promise<T>* promise;
};

using IOOverlappedReadWrite = IOOverlapped<size_t>;
using IOOverlappedConnect = IOOverlapped<Nothing>;

// The async `Accept` is like `Connect` but with some additional buffers.
// We can't use inheritance here because `IOOverlappedAccept` will not
// be a C++ standard layout type, meaning that treating it like a C
// struct in the IOCP code can lead to undefined behavior.
struct IOOverlappedAccept
{
  IOOverlappedAccept(const IOOverlappedBase& _base, Promise<Nothing>* _promise)
    : base(_base), promise(_promise)
  {}

  IOOverlappedBase base;
  Promise<Nothing>* promise;

  // `AcceptEx` needs a buffer size of atleast "16 bytes more than the size of
  // the sockaddr structure for the transport protocol" [1] for the remote and
  // local addresses.
  // [1]: https://msdn.microsoft.com/en-us/library/windows/desktop/ms737524(v=vs.85).aspx // NOLINT(whitespace/line_length)
  unsigned char buf[2 * sizeof(SOCKADDR_STORAGE) + 32];
};

// The IOCP Win32 APIs use C structs and involve a lot of type unsafe casting,
// so we ensure that these C++ structs can be safely used as C structs.
static_assert(
    std::is_standard_layout<IOOverlappedReadWrite>::value,
    "IOOverlappedReadWrite must be a standard layout type");

static_assert(
    std::is_standard_layout<IOOverlappedConnect>::value,
    "IOOverlappedConnect must be a standard layout type");

static_assert(
    std::is_standard_layout<IOOverlappedAccept>::value,
    "IOOverlappedAccept must be a standard layout type");


template <typename T>
static void set_io_promise(Promise<T>* promise, const T& data, DWORD error)
{
  // If our discard induced CancelIoEx call succeeded, then we
  // will see ERROR_OPERATION_ABORTED. Otherwise, the discard
  // lost the race against the operation completing and we
  // should just surface the result.
  if (promise->future().hasDiscard() && error == ERROR_OPERATION_ABORTED) {
    promise->discard();
  } else if (error == ERROR_SUCCESS) {
    promise->set(data);
  } else {
    promise->fail("IO failed with error code: " + WindowsError(error).message);
  }
}


static void handle_io(OVERLAPPED* overlapped_, DWORD bytes_transferred)
{
  // Overlapped objects to passed to this function should have been contained
  // inside a `IOOverlappedBase`. So, we can use the `CONTAINING_RECORD` macro
  // to get the pointer to the `IOOverlappedBase` struct from the `OVERLAPPED`
  // pointer.
  IOOverlappedBase* overlapped_base =
    CONTAINING_RECORD(overlapped_, IOOverlappedBase, overlapped);

  // Get the Win32 error code of the overlapped operation. The status code
  // is actually in overlapped->overlapped->Internal, but it's the NT
  // status code instead of the Win32 error.
  DWORD error = ERROR_SUCCESS;
  DWORD bytes;
  const BOOL success = ::GetOverlappedResult(
      overlapped_base->handle, &overlapped_base->overlapped, &bytes, FALSE);

  if (!success) {
    error = ::GetLastError();
  } else {
    // If the IO succeeded, these should be the same for sure.
    CHECK_EQ(bytes, bytes_transferred);
  }

  switch (overlapped_base->type) {
    case IOType::READ:
    case IOType::RECV: {
      IOOverlappedReadWrite* io_read =
        CONTAINING_RECORD(overlapped_base, IOOverlappedReadWrite, base);

      std::unique_ptr<Promise<size_t>> promise(io_read->promise);

      // For reads, we need to make sure we ignore the EOF errors.
      if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
        set_io_promise(promise.get(), static_cast<size_t>(0), ERROR_SUCCESS);
      } else {
        set_io_promise(
            promise.get(), static_cast<size_t>(bytes_transferred), error);
      }
      return;
    }
    case IOType::WRITE:
    case IOType::SEND:
    case IOType::SENDFILE: {
      IOOverlappedReadWrite* io_write =
        CONTAINING_RECORD(overlapped_base, IOOverlappedReadWrite, base);

      std::unique_ptr<Promise<size_t>> promise(io_write->promise);
      set_io_promise(
          promise.get(), static_cast<size_t>(bytes_transferred), error);
      return;
    }
    case IOType::CONNECT: {
      IOOverlappedConnect* io_connect =
        CONTAINING_RECORD(overlapped_base, IOOverlappedConnect, base);

      std::unique_ptr<Promise<Nothing>> promise(io_connect->promise);
      set_io_promise(promise.get(), Nothing(), error);
      return;
    }
    case IOType::ACCEPT: {
      IOOverlappedAccept* io_accept =
        CONTAINING_RECORD(overlapped_base, IOOverlappedAccept, base);

      std::unique_ptr<Promise<Nothing>> promise(io_accept->promise);
      set_io_promise(promise.get(), Nothing(), error);
      return;
    }
  }

  UNREACHABLE();
}


// Function to handle all IOCP notifications. Returns true if the IOCP loop
// should continue and false if it should stop.
static bool check_and_handle_completion(const OVERLAPPED_ENTRY& entry)
{
  switch (entry.lpCompletionKey) {
    case KEY_QUIT: {
      return false;
    }
    case KEY_TIMER: {
      // In the IOCP, we just set the timer. When the timer completes, it will
      // queue up an APC that will interrupt the IOCP loop in order to execute
      // the APC callback.
      TimerOverlapped* timer =
        reinterpret_cast<TimerOverlapped*>(entry.lpOverlapped);

      const BOOL success = ::SetWaitableTimer(
          timer->timer, &timer->time, 0, &timer_apc, timer, TRUE);

      if (!success) {
        // Nothing we can really do here aside from log the event.
        std::string errorMsg = WindowsError().message;
        LOG(FATAL)
          << "process::windows::handle_completion failed to set timer: "
          << errorMsg;
      }
      return true;
    }
    case KEY_IO: {
      if (entry.lpOverlapped == nullptr) {
        // Don't know if this is possible, but just log it in case.
        LOG(FATAL) << "process::windows::handle_completion returned with a null"
                   << "overlapped object";
      } else {
        handle_io(entry.lpOverlapped, entry.dwNumberOfBytesTransferred);
      }
      return true;
    }
  }

  UNREACHABLE();
}


// Windows Event loop implementation.
Try<EventLoop*> EventLoop::create()
{
  HANDLE iocp_handle =
    ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);

  if (iocp_handle == nullptr) {
    return WindowsError();
  }

  return new EventLoop(iocp_handle);
}

EventLoop::EventLoop(HANDLE iocp_handle)
  : iocp_handle_(std::unique_ptr<HANDLE, HandleDeleter>(iocp_handle))
{}

Try<Nothing> EventLoop::run()
{
  const Try<long> maxEntries = os::cpus();
  if (maxEntries.isError()) {
    return Error(maxEntries.error());
  }

  bool loop = true;
  std::vector<OVERLAPPED_ENTRY> entries(maxEntries.get());
  while (loop) {
    ULONG dequeued_entries;

    // This function can return in three ways:
    //   1) We get some IO completion events and the function will return true.
    //   2) A timer APC interrupts the function in its alertable wait status,
    //      so the thread will execute the APC. The function will return false
    //      and set the error to `WAIT_IO_COMPLETION`.
    //   3) We get a legitimate error, so we exit early.
    BOOL success = ::GetQueuedCompletionStatusEx(
        iocp_handle_.get(),
        entries.data(),
        maxEntries.get(),
        &dequeued_entries,
        INFINITE,
        TRUE);

    if (!success) {
      // Case 2: Got APC interrupt. We simply continue the loop.
      if (::GetLastError() == WAIT_IO_COMPLETION) {
        continue;
      }

      // We hit case 3, which means we got a serious error.
      return WindowsError();
    }

    // Case 1: Dequeue completion packets and process them. If we get a quit
    // notification, then we will finish the current queue and then exit.
    for (ULONG i = 0; i < dequeued_entries; i++) {
      const bool continue_loop = check_and_handle_completion(entries[i]);
      loop = loop && continue_loop;
    }
  }
  return Nothing();
}


Try<Nothing> EventLoop::stop()
{
  const BOOL success =
    ::PostQueuedCompletionStatus(iocp_handle_.get(), 0, KEY_QUIT, nullptr);

  if (success) {
    return Nothing();
  }

  return WindowsError();
}


Try<Nothing> EventLoop::launchTimer(
    const Duration& duration, const lambda::function<void()>& callback)
{
  // Create a non-inheritable, manual reset, unnamed timer.
  HANDLE timer = ::CreateWaitableTimerW(nullptr, true, nullptr);
  if (timer == nullptr) {
    return WindowsError();
  }

  // If you give a positive value, then the timer call interprets it as
  // absolute time. A negative value is interpretted as relative time.
  // 0 is run immediately. The resolution is in 100ns.
  LARGE_INTEGER time_elapsed;
  time_elapsed.QuadPart = -duration.ns() / 100;

  TimerOverlapped* overlapped =
    new TimerOverlapped{timer, time_elapsed, callback};

  // We don't actually set the timer here since APCs only execute in the same
  // thread that called the async function. So, we queue the function call to
  // the IOCP so the event loop thread can queue the APC.
  //
  // NOTE: `::PostQueuedCompletionStatus` does not process the second to fourth
  // arguments, so you can give anything for them. Specifically, the overlapped
  // parameter doesn't need to point to a `OVERLAPPED` structure. See
  // https://msdn.microsoft.com/en-us/library/windows/desktop/aa365458(v=vs.85).aspx // NOLINT(whitespace/line_length)
  const BOOL success = ::PostQueuedCompletionStatus(
      iocp_handle_.get(),
      0,
      KEY_TIMER,
      reinterpret_cast<OVERLAPPED*>(overlapped));

  if (!success) {
    // Failing `PostQueuedCompletionStatus` means we have to clean the
    // memory here, since the APC won't execute.
    WindowsError error;
    delete overlapped;
    ::CloseHandle(timer);
    return error;
  }

  // The APC callback will clean up the memory and handle, so we can return.
  return Nothing();
}


// NOTE: The following functions use `int_fd` instead of the native Win32
// `HANDLE`, because they are the more "public" libwinio APIs that interface
// directly with the libprocess IO, socket and event loop code.
Try<Nothing> EventLoop::registerHandle(const int_fd& fd)
{
  Try<HANDLE> assigned_handle = fd.assign_iocp(iocp_handle_.get(), KEY_IO);
  if (assigned_handle.isError()) {
    return Error(assigned_handle.error());
  }

  // In this case, the IOCP handle was already assigned. So, we check if the
  // right one is assigned.
  if (assigned_handle.get() != nullptr) {
    if (assigned_handle.get() != iocp_handle_.get()) {
      return Error(
          "fd is already registered to a different Windows Event Loop");
    }
    return Nothing();
  }

  // This is the first time the handle was assigned. We continue the
  // initialization. These are some optimizations that prevent IOCP
  // notifications on success and event notifications, so that we have less
  // context switches.
  BOOL success = ::SetFileCompletionNotificationModes(
      fd, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);

  if (!success) {
    return WindowsError();
  }

  return Nothing();
}


template <typename T, typename O>
static void enable_cancellation(
    const int_fd& fd,
    const Future<T>& future,
    const std::shared_ptr<O>& overlapped)
{
  // We capture a `std::weak_ptr` in the `.onDiscard` callback, so that
  // we can cancel the IO operation with the overlapped object if it still
  // exists. The captured `shared_ptr` in the `.onAny` callback ensures that
  // we keep the overlapped object alive until the IOCP callbacks are done.
  auto overlapped_weak = std::weak_ptr<O>(overlapped);

  future.onDiscard([fd, overlapped_weak]() {
    std::shared_ptr<O> cancel = overlapped_weak.lock();
    if (static_cast<bool>(cancel)) {
      // We were able to get a reference to the overlapped object, so let's
      // try to cancel the IO operation. Note that there is technically a
      // race here. We could be between the IO completion event and deleting
      // the overlapped object. In that case, this function will just no-op.
      ::CancelIoEx(fd, &cancel->base.overlapped);
    }
  });

  future.onAny([overlapped]() {});
}


static Future<size_t> read_internal(
    const int_fd& fd, void* buf, size_t size, IOType type)
{
  process::initialize();

  Promise<size_t>* promise = new Promise<size_t>();
  Future<size_t> future = promise->future();

  // We use a `std::shared_ptr`, so we can safely support canceling.
  auto overlapped = std::make_shared<IOOverlappedReadWrite>(
      IOOverlappedBase{OVERLAPPED{}, fd, type}, promise);

  enable_cancellation(fd, future, overlapped);

  // Start the asynchronous operation.
  const Result<size_t> result =
    os::read_async(fd, buf, size, &overlapped->base.overlapped);

  // If the request is pending, then we return immediately and have the
  // callback free the promise and overlapped.
  if (result.isNone()) {
    return future;
  }

  // In an error or immediate success, we have to manually set the promise
  // and free it.
  if (result.isError()) {
    promise->fail("os::read_async failed: " + result.error());
  } else if (result.isSome()) {
    promise->set(result.get());
  }
  delete promise;
  return future;
}


static Future<size_t> write_internal(
    const int_fd& fd, const void* buf, size_t size, IOType type)
{
  process::initialize();

  Promise<size_t>* promise = new Promise<size_t>();
  Future<size_t> future = promise->future();

  // We use a `std::shared_ptr`, so we can safely support canceling.
  auto overlapped = std::make_shared<IOOverlappedReadWrite>(
      IOOverlappedBase{OVERLAPPED{}, fd, type}, promise);

  enable_cancellation(fd, future, overlapped);

  // Start the asynchronous operation.
  const Result<size_t> result =
    os::write_async(fd, buf, size, &overlapped->base.overlapped);

  // If the request is pending, then we return immediately and have the
  // callback free the promise and overlapped.
  if (result.isNone()) {
    return future;
  }

  // In an error or immediate success, we have to manually set the promise
  // and free it.
  if (result.isError()) {
    promise->fail("os::write_async failed: " + result.error());
  } else if (result.isSome()) {
    promise->set(result.get());
  }
  delete promise;
  return future;
}


Future<size_t> read(const int_fd& fd, void* buf, size_t size)
{
  return read_internal(fd, buf, size, IOType::READ);
}


Future<size_t> write(const int_fd& fd, const void* buf, size_t size)
{
  return write_internal(fd, buf, size, IOType::WRITE);
}


Future<size_t> recv(const int_fd& fd, void* buf, size_t size)
{
  return read_internal(fd, buf, size, IOType::RECV);
}


Future<size_t> send(const int_fd& fd, const void* buf, size_t size)
{
  return write_internal(fd, buf, size, IOType::SEND);
}


Future<Nothing> accept(const int_fd& fd, const int_fd& accepted_socket)
{
  process::initialize();

  Promise<Nothing>* promise = new Promise<Nothing>();
  Future<Nothing> future = promise->future();

  // We use a `std::shared_ptr`, so we can safely support canceling.
  auto overlapped = std::make_shared<IOOverlappedAccept>(
      IOOverlappedBase{OVERLAPPED{}, fd, IOType::ACCEPT}, promise);

  enable_cancellation(fd, future, overlapped);

  // The `overlapped->buf` passed into `::AcceptEx` will receive the first
  // data block sent, the local address of the server and the remote address
  // of the client. The (4th, 5th, 6th) arguments are
  // (0, sizeof(buf)/2 , sizeof(buf) / 2), since we ignore the first block,
  // and simply store the local and remote addresses. For more details, see
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms737524(v=vs.85).aspx // NOLINT(whitespace/line_length)
  DWORD bytes;
  const BOOL success = ::AcceptEx(
      fd,
      accepted_socket,
      overlapped->buf,
      0,
      sizeof(overlapped->buf) / 2,
      sizeof(overlapped->buf) / 2,
      &bytes,
      &overlapped->base.overlapped);

  const DWORD error = ::WSAGetLastError();

  // If the request is pending, then we return immediately and have the
  // callback free the promise and overlapped
  if (!success && error == WSA_IO_PENDING) {
    return future;
  }

  // In an error or immediate success, we have to manually set the promise
  // and free it.
  if (success) {
    promise->set(Nothing());
  } else {
    promise->fail("AcceptEx failed: " + WindowsError(error).message);
  }
  delete promise;
  return future;
}

// The MSDN docs state that `::ConnectEx` must be retrieved through
// `::WSAIoctl`. See the remarks section of the docs:
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms737606(v=vs.85).aspx // NOLINT(whitespace/line_length)
static LPFN_CONNECTEX init_connect_ex(const int_fd& fd)
{
  LPFN_CONNECTEX connect_ex;
  GUID connect_ex_guid = WSAID_CONNECTEX;
  DWORD bytes;

  int res = ::WSAIoctl(
      fd,
      SIO_GET_EXTENSION_FUNCTION_POINTER,
      &connect_ex_guid,
      sizeof(connect_ex_guid),
      &connect_ex,
      sizeof(connect_ex),
      &bytes,
      nullptr,
      nullptr);

  // We can't use connect if we failed to get the function, so we just force
  // an abort.
  CHECK_EQ(res, 0);
  return connect_ex;
}


static LPFN_CONNECTEX& get_connect_ex_ptr(const int_fd& fd)
{
  // C++11 magic static initialization.
  static LPFN_CONNECTEX ptr = init_connect_ex(fd);
  return ptr;
}


Future<Nothing> connect(const int_fd& fd, const network::Address& address)
{
  process::initialize();

  // `::ConnectEx` needs the socket to be bound first.
  Try<Nothing> bind_result = Nothing();
  if (address.family() == network::Address::Family::INET4) {
    const network::inet4::Address addr = network::inet4::Address::ANY_ANY();
    bind_result = network::bind(fd, addr);
  } else if (address.family() == network::Address::Family::INET6) {
    const network::inet6::Address addr = network::inet6::Address::ANY_ANY();
    bind_result = network::bind(fd, addr);
  } else {
    return Failure("Async connect only supports IPv6 and IPv4");
  }

  if (bind_result.isError()) {
    // `WSAEINVAL` means socket is already bound, so we can continue. If it was
    // bound incorrectly, then we can get an error later on.
    if (::WSAGetLastError() != WSAEINVAL) {
      return Failure("Failed to bind connect socket: " + bind_result.error());
    }
  }

  // Load `::ConnectEx` function pointer, since it's not normally available.
  const sockaddr_storage storage = address;
  const int address_size = static_cast<int>(address.size());
  LPFN_CONNECTEX connect_ex = get_connect_ex_ptr(fd);

  Promise<Nothing>* promise = new Promise<Nothing>();
  Future<Nothing> future = promise->future();

  auto overlapped = std::make_shared<IOOverlappedConnect>(
      IOOverlappedBase{OVERLAPPED{}, fd, IOType::CONNECT}, promise);

  enable_cancellation(fd, future, overlapped);

  const BOOL success = connect_ex(
      fd,
      reinterpret_cast<const sockaddr*>(&storage),
      address_size,
      nullptr,
      0,
      nullptr,
      &overlapped->base.overlapped);

  const DWORD error = ::WSAGetLastError();

  // If the request is pending, then we return immediately and have the
  // callback free the promise and overlapped
  if (!success && error == WSA_IO_PENDING) {
    return future;
  }

  // In an error or immediate success, we have to manually set the promise
  // and free it.
  if (success) {
    promise->set(Nothing());
  } else {
    promise->fail("AcceptEx failed: " + stringify(error));
  }
  delete promise;
  return future;
}


Future<size_t> sendfile(
    const int_fd& socket, const int_fd& file, off_t offset, size_t size)
{
  process::initialize();

  if (offset < 0) {
    return Failure("process::windows::sendfile got negative offset");
  }

  Promise<size_t>* promise = new Promise<size_t>();
  Future<size_t> future = promise->future();

  auto overlapped = std::make_shared<IOOverlappedReadWrite>(
      IOOverlappedBase{OVERLAPPED{}, socket, IOType::SENDFILE}, promise);

  uint64_t offset64 = static_cast<uint64_t>(offset);
  overlapped->base.overlapped.Offset = static_cast<DWORD>(offset64);
  overlapped->base.overlapped.OffsetHigh = static_cast<DWORD>(offset64 >> 32);

  enable_cancellation(socket, future, overlapped);

  const Result<size_t> result =
    os::sendfile_async(socket, file, size, &overlapped->base.overlapped);

  // If the request is pending, then we return immediately and have the
  // callback free the promise and overlapped.
  if (result.isNone()) {
    return future;
  }

  // In an error or immediate success, we have to manually set the promise
  // and free it.
  if (result.isError()) {
    promise->fail("os::sendfile_async failed: " + result.error());
  } else if (result.isSome()) {
    promise->set(result.get());
  }
  delete promise;
  return future;
}

} // namespace windows {
} // namespace process {

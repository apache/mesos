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

#ifdef __WINDOWS__
// NOTE: This must be included before the OpenSSL headers as it includes
// `WinSock2.h` and `Windows.h` in the correct order.
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <atomic>
#include <queue>

#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <process/ssl/flags.hpp>

#include <stout/synchronized.hpp>
#include <stout/unimplemented.hpp>

#include "openssl.hpp"

#include "ssl/openssl_socket.hpp"

namespace process {
namespace network {
namespace internal {

// Contains the state of a source/sink BIO wrapper for an ordinary socket.
struct SocketBIOData
{
  // Socket associated with this BIO.
  // OpenSSLSocketImpl retains ownership of the socket.
  int_fd socket;

  // Stores the latest call to `BIO_write`.
  struct SendRequest
  {
    SendRequest(Future<size_t> _future)
      : future(_future) {}

    Future<size_t> future;
  };

  // Stores the latest call to `BIO_read`.
  struct RecvRequest
  {
    RecvRequest(
        char* _data,
        size_t _size,
        Future<size_t> _future)
      : data(_data),
        size(_size),
        future(_future) {}

    char* data; // NOT owned by this object.
    size_t size;
    Future<size_t> future;
  };

  // Protects the following instance variables.
  std::atomic_flag lock = ATOMIC_FLAG_INIT;
  Owned<SendRequest> send_request;
  Owned<RecvRequest> recv_request;
  bool reached_eof;
};


// Called in response to `BIO_new()`.
// We will need to perform some additional initialization to link
// the OpenSSLSocketImpl to the BIO, outside of this function.
//
// See: https://www.openssl.org/docs/man1.1.1/man3/BIO_get_data.html
int bio_libprocess_create(BIO* bio)
{
  // Indicate that initialization has not completed.
  BIO_set_init(bio, 0);

  // The caller will need to fill in the data with a copy of the `int_fd`
  // associated with the OpenSSLSocketImpl, after BIO creation.
  BIO_set_data(bio, new SocketBIOData());

  return 1;
}


// Called in response to `BIO_free()`.
int bio_libprocess_destroy(BIO* bio)
{
  SocketBIOData* data = reinterpret_cast<SocketBIOData*>(BIO_get_data(bio));
  CHECK_NOTNULL(data);
  delete data;

  return 1;
}


// Called in response to `BIO_write()`.
// This function will maintain a single pending write at any time
// by swapping the contents of `send_request`.
int bio_libprocess_write(BIO* bio, const char* input, int length)
{
  SocketBIOData* data = reinterpret_cast<SocketBIOData*>(BIO_get_data(bio));
  CHECK_NOTNULL(data);

  synchronized (data->lock) {
    // Only a single write should be pending at any time.
    if (data->send_request.get() == nullptr ||
        data->send_request->future.isReady()) {
      Owned<SocketBIOData::SendRequest> request(
          new SocketBIOData::SendRequest(
              io::write(data->socket, input, length)));

      std::swap(request, data->send_request);
      return length;
    }

    BIO_set_retry_write(bio);
    return 0;
  }
}


// Called in response to `BIO_read()`.
// This function will maintain a single pending read at any time
// by swapping the contents of `recv_request`.
int bio_libprocess_read(BIO* bio, char* output, int length)
{
  SocketBIOData* data = reinterpret_cast<SocketBIOData*>(BIO_get_data(bio));
  CHECK_NOTNULL(data);

  synchronized (data->lock) {
    // Only a single read should be pending at any time.
    if (data->recv_request.get() == nullptr) {
      Owned<SocketBIOData::RecvRequest> request(
          new SocketBIOData::RecvRequest(
              output, length, io::read(data->socket, output, length)));

      std::swap(request, data->recv_request);
    } else if (data->recv_request->future.isReady()) {
      Owned<SocketBIOData::RecvRequest> completed_request;
      std::swap(completed_request, data->recv_request);

      // When retrying a read, the arguments passed in must be identical
      // to the previous attempt. This is an API requirement of `SSL_read`.
      // See: https://www.openssl.org/docs/man1.1.1/man3/SSL_read.html
      //   "The calling process then must repeat the call after taking
      //   appropriate action to satisfy the needs of the read function."
      //
      // This guarantee means we can read onto the same buffer between retries,
      // confident that the same output buffer will be allocated and available
      // each time.
      CHECK_EQ(completed_request->data, output);
      CHECK_EQ(completed_request->size, length);

      if (completed_request->future.get() == 0u) {
        data->reached_eof = true;
      }

      return completed_request->future.get();
    }

    BIO_set_retry_read(bio);
    return 0;
  }
}


// Called in response to `BIO_ctrl()`, which is usually wrapped by
// different macros, i.e. `BIO_reset()`, `BIO_eof()`, `BIO_flush()`, etc.
//
// The enums implemented below were based on Libevent's BIO implementation
// and OpenSSL's BSS (BIO Source Sink) Socket, found here:
// https://github.com/openssl/openssl/blob/OpenSSL_1_1_1/crypto/bio/bss_sock.c
//
// See: https://www.openssl.org/docs/man1.1.1/man3/BIO_ctrl.html
long bio_libprocess_ctrl(BIO* bio, int command, long, void*)
{
  SocketBIOData* data = reinterpret_cast<SocketBIOData*>(BIO_get_data(bio));
  CHECK_NOTNULL(data);

  switch (command) {
    // Returns 1 when a read request has returned 0 bytes,
    // and otherwise returns 0.
    case BIO_CTRL_EOF: {
      synchronized (data->lock) {
        if (data->reached_eof) {
          return 1;
        }

        return 0;
      }
    }

    // NOTE: We choose not to implement BIO_CTRL_FLUSH because this call
    // expects blocking behavior, and is sometimes called from within OpenSSL's
    // library functions. When necessary, the retry-write behavior should be
    // sufficient to make sure writes succeed.
    case BIO_CTRL_FLUSH: {
      // NOTE: We must return a successful result here, even though we
      // have not flushed any data, because OpenSSL considers a failure
      // here unrecoverable and will try to close the connection.
      return 1;
    }

    // NOTE: Libevent implements BIO_CTRL_GET_CLOSE and BIO_CTRL_SET_CLOSE,
    // which indicates that the underlying I/O stream should be closed when
    // the BIO is freed. We opt to always close/free the socket/BIO.

    // NOTE: We choose not to implement BIO_CTRL_PENDING and BIO_CTRL_WPENDING
    // because this implementation only keeps a single read/write buffered
    // at once. Also, these methods are not used by OpenSSL or by callers
    // of our implementation.

    default:
      return 0; // Not implemented.
  }
}


// Constructs a new BIO_METHOD wrapping the libprocess event loop.
//
// See: https://www.openssl.org/docs/man1.1.1/man3/BIO_meth_new.html
static BIO_METHOD* libprocess_bio = nullptr;
static BIO_METHOD* get_libprocess_BIO_METHOD()
{
  if (libprocess_bio != nullptr) {
    return libprocess_bio;
  }

  // Get a unique index for our new type, and annotate the index
  // to say this BIO is a source/sink with a file descriptor.
  int type = BIO_get_new_index();
  CHECK(type > 0) << "Failed to create a new BIO type";
  type = type|BIO_TYPE_SOURCE_SINK|BIO_TYPE_DESCRIPTOR;

  libprocess_bio = BIO_meth_new(type, "libprocess");

  BIO_meth_set_create(libprocess_bio, bio_libprocess_create);
  BIO_meth_set_destroy(libprocess_bio, bio_libprocess_destroy);

  BIO_meth_set_write(libprocess_bio, bio_libprocess_write);
  BIO_meth_set_read(libprocess_bio, bio_libprocess_read);

  BIO_meth_set_ctrl(libprocess_bio, bio_libprocess_ctrl);

  return libprocess_bio;
}


// Constructs a new source/sink BIO around the given socket.
static BIO* BIO_new_libprocess(int_fd socket)
{
  BIO* bio = BIO_new(get_libprocess_BIO_METHOD());
  CHECK_NOTNULL(bio);

  SocketBIOData* data = reinterpret_cast<SocketBIOData*>(BIO_get_data(bio));
  CHECK_NOTNULL(data);

  // Fill in the socket field of the new BIO.
  data->socket = socket;

  BIO_set_init(bio, 1);

  return bio;
}


Try<std::shared_ptr<SocketImpl>> OpenSSLSocketImpl::create(int_fd s)
{
  UNIMPLEMENTED;
}


OpenSSLSocketImpl::OpenSSLSocketImpl(int_fd _s)
  : PollSocketImpl(_s) {}


OpenSSLSocketImpl::~OpenSSLSocketImpl()
{
  UNIMPLEMENTED;
}


Future<Nothing> OpenSSLSocketImpl::connect(
    const Address& address)
{
  LOG(FATAL) << "No TLS config was passed to a SSL socket.";
}


Future<Nothing> OpenSSLSocketImpl::connect(
    const Address& address,
    const openssl::TLSClientConfig& config)
{
  UNIMPLEMENTED;
}


Future<size_t> OpenSSLSocketImpl::recv(char* data, size_t size)
{
  UNIMPLEMENTED;
}


Future<size_t> OpenSSLSocketImpl::send(const char* data, size_t size)
{
  UNIMPLEMENTED;
}


Future<size_t> OpenSSLSocketImpl::sendfile(
    int_fd fd, off_t offset, size_t size)
{
  UNIMPLEMENTED;
}


Future<std::shared_ptr<SocketImpl>> OpenSSLSocketImpl::accept()
{
  UNIMPLEMENTED;
}


Try<Nothing, SocketError> OpenSSLSocketImpl::shutdown(int how)
{
  UNIMPLEMENTED;
}

} // namespace internal {
} // namespace network {
} // namespace process {

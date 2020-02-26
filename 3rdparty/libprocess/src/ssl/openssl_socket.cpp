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

#include <algorithm>
#include <atomic>
#include <queue>

#include <boost/shared_array.hpp>

#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <process/ssl/flags.hpp>

#include <stout/net.hpp>
#include <stout/synchronized.hpp>
#include <stout/unimplemented.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/lseek.hpp>

#include "openssl.hpp"

#include "ssl/openssl_socket.hpp"

using process::network::openssl::Mode;

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


Try<std::shared_ptr<SocketImpl>> OpenSSLSocketImpl::create(int_fd socket)
{
  openssl::initialize();

  if (!openssl::flags().enabled) {
    return Error("SSL is disabled");
  }

  return std::make_shared<OpenSSLSocketImpl>(socket);
}


OpenSSLSocketImpl::OpenSSLSocketImpl(int_fd socket)
  : PollSocketImpl(socket),
    ssl(nullptr),
    dirty_shutdown(false) {}


OpenSSLSocketImpl::~OpenSSLSocketImpl()
{
  if (ssl != nullptr) {
    SSL_free(ssl);
    ssl = nullptr;
  }

  if (compute_thread.isSome()) {
    process::terminate(compute_thread.get());
    compute_thread = None();
  }
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
  if (client_config.isSome()) {
    return Failure("Socket is already connecting or connected");
  }

  if (config.ctx == nullptr) {
    return Failure("Invalid SSL context");
  }

  // NOTE: The OpenSSLSocketImpl destructor is responsible for calling
  // `SSL_free` on this SSL object.
  ssl = SSL_new(config.ctx);
  if (ssl == nullptr) {
    return Failure("Failed to connect: SSL_new");
  }

  client_config = config;

  if (config.configure_socket) {
    Try<Nothing> configured = config.configure_socket(
        ssl, address, config.servername);

    if (configured.isError()) {
      return Failure("Failed to configure socket: " + configured.error());
    }
  }

  // Set the SSL context in client mode.
  SSL_set_connect_state(ssl);

  if (address.family() == Address::Family::INET4 ||
      address.family() == Address::Family::INET6) {
    Try<inet::Address> inet_address = network::convert<inet::Address>(address);

    if (inet_address.isError()) {
      return Failure("Failed to convert address: " + inet_address.error());
    }

    // Determine the 'peer_ip' from the address we're connecting to in
    // order to properly verify the certificate later.
    peer_ip = inet_address->ip;
  }

  if (config.servername.isSome()) {
    VLOG(2) << "Connecting to " << config.servername.get() << " at " << address;
  } else {
    VLOG(2) << "Connecting to " << address << " with no hostname specified";
  }

  // Hold a weak pointer since the connection (plus handshaking)
  // might never complete.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  // Connect like a normal socket, then setup the I/O abstraction with OpenSSL
  // and perform the TLS handshake.
  return PollSocketImpl::connect(address)
    .then([weak_self]() -> Future<size_t> {
      std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
      if (self == nullptr) {
        return Failure("Socket destroyed while connecting");
      }

      return self->set_ssl_and_do_handshake(self->ssl);
    })
    .then([weak_self]() -> Future<Nothing> {
      std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
      if (self == nullptr) {
        return Failure("Socket destroyed while connecting");
      }

      // Time to do post-verification.
      CHECK(self->client_config.isSome());

      if (self->client_config->verify) {
        Try<Nothing> verify = self->client_config->verify(
            self->ssl, self->client_config->servername, self->peer_ip);

        if (verify.isError()) {
          VLOG(1) << "Failed connect, verification error: " << verify.error();

          return Failure(verify.error());
        }
      }

      return Nothing();
    });
}


Future<size_t> OpenSSLSocketImpl::recv(char* output, size_t size)
{
  if (dirty_shutdown) {
    return Failure("Socket is shutdown");
  }

  // Hold a weak pointer since the incoming socket is not guaranteed
  // to terminate before the receiving end does.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  return process::loop(
      compute_thread,
      [weak_self, output, size]() -> Future<int> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while receiving");
        }

        ERR_clear_error();
        return SSL_read(self->ssl, output, size);
      },
      [weak_self](int result) -> Future<ControlFlow<size_t>> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while receiving");
        }

        if (result == 0) {
          // Check if EOF has been reached.
          BIO* bio = SSL_get_rbio(self->ssl);
          if (BIO_eof(bio) == 1) {
            return Break(0u);
          }
        }

        return self->handle_ssl_return_result(result, true);
      });
}


Future<size_t> OpenSSLSocketImpl::send(const char* input, size_t size)
{
  if (dirty_shutdown) {
    return Failure("Socket is shutdown");
  }

  // Hold a weak pointer since a write may become backlogged indefinitely.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  return process::loop(
      compute_thread,
      [weak_self, input, size]() -> Future<int> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while sending");
        }

        ERR_clear_error();
        return SSL_write(self->ssl, input, size);
      },
      [weak_self](int result) -> Future<ControlFlow<size_t>> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while sending");
        }

        return self->handle_ssl_return_result(result, false);
      });
}


Future<size_t> OpenSSLSocketImpl::sendfile(
    int_fd fd, off_t offset, size_t size)
{
  if (dirty_shutdown) {
    return Failure("Socket is shutdown");
  }

  // Hold a weak pointer since both read and write are not guaranteed to finish.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  Try<off_t> seek = os::lseek(fd, offset, SEEK_SET);
  if (seek.isError()) {
    return Failure("Failed to seek: " + seek.error());
  }

  Try<Nothing> async = io::prepare_async(fd);
  if (async.isError()) {
    return Failure("Failed to make FD asynchronous: " + async.error());
  }

  size_t remaining_size = size;
  boost::shared_array<char> data(new char[io::BUFFERED_READ_SIZE]);

  return process::loop(
      compute_thread,
      [weak_self, fd, remaining_size, data]() -> Future<size_t> {
        return io::read(
            fd, data.get(), std::min(io::BUFFERED_READ_SIZE, remaining_size))
          .then([weak_self, data](size_t read_bytes) -> Future<size_t> {
            std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
            if (self == nullptr) {
              return Failure("Socket destroyed while sending file");
            }

            return self->send(data.get(), read_bytes);
          });
      },
      [size, &remaining_size](size_t written) mutable
          -> Future<ControlFlow<size_t>> {
        remaining_size -= written;

        if (remaining_size > 0) {
          return Continue();
        }

        return Break(size);
      });
}


Future<std::shared_ptr<SocketImpl>> OpenSSLSocketImpl::accept()
{
  if (!accept_loop_started.once()) {
    // Hold a weak pointer since we do not want this accept loop to extend
    // the lifetime of `this` unnecessarily.
    std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

    // We start accepting incoming connections in a loop here because a socket
    // must complete the SSL handshake (or be downgraded) before the socket is
    // considered ready. In case the incoming socket never writes any data,
    // we do not wait for the accept logic to complete before accepting a
    // new socket.
    process::loop(
        compute_thread,
        [weak_self]() -> Future<std::shared_ptr<SocketImpl>> {
          std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());

          if (self != nullptr) {
            return self->PollSocketImpl::accept();
          }

          return Failure("Socket destructed");
        },
        [weak_self](const std::shared_ptr<SocketImpl>& socket)
            -> Future<ControlFlow<Nothing>> {
          std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());

          if (self == nullptr) {
            return Break();
          }

          return self->handle_accept_callback(socket);
        });

    accept_loop_started.done();
  }

  // NOTE: In order to not deadlock the libprocess socket manager, we must
  // defer accepted sockets regardless of success or failure. This prevents
  // the socket manager from recursively calling `on_accept` and deadlocking.
  return accept_queue.get()
    .repair(defer(
        [](const Future<Future<std::shared_ptr<SocketImpl>>>& failure) {
          return failure;
        }))
    .then(defer(
        [](const Future<std::shared_ptr<SocketImpl>>& impl)
            -> Future<std::shared_ptr<SocketImpl>> {
          CHECK(!impl.isPending());
          return impl;
        }));
}


Try<Nothing, SocketError> OpenSSLSocketImpl::shutdown(int how)
{
  if (dirty_shutdown) {
    return Nothing();
  }

  // Treat this as a dirty shutdown (i.e. closing the socket before sending
  // the SSL close notification) because we are not guaranteed to properly
  // shutdown synchronously.
  dirty_shutdown = true;

  // Hold a weak pointer since we are ok with closing the socket before
  // shutdown is completed gracefully.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  // The shutdown itself will happen asynchronously.
  process::loop(
      compute_thread,
      [weak_self]() -> Future<int> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while doing shutdown");
        }

        ERR_clear_error();
        return SSL_shutdown(self->ssl);
      },
      [weak_self](int result) -> Future<ControlFlow<size_t>> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while doing shutdown");
        }

        // A successful shutdown will return 0 if the close notification
        // was sent, or 1 if both sides of the connection have closed.
        // Either case is sufficient for a clean shutdown.
        if (result >= 0) {
          return Break(0u);
        }

        // Check if EOF has been reached.
        BIO* bio = SSL_get_rbio(self->ssl);
        if (BIO_eof(bio) == 1) {
          return Break(0u);
        }

        return self->handle_ssl_return_result(result, false);
      });

  return Nothing();
}


Future<ControlFlow<Nothing>> OpenSSLSocketImpl::handle_accept_callback(
    const std::shared_ptr<SocketImpl>& socket)
{
  // Wrap this new socket up into our SSL wrapper class by releasing
  // the FD and creating a new OpenSSLSocketImpl object with the FD.
  const std::shared_ptr<OpenSSLSocketImpl> ssl_socket =
    std::make_shared<OpenSSLSocketImpl>(socket->release());

  // Set up SSL object.
  SSL* accept_ssl = SSL_new(openssl::context());
  if (accept_ssl == nullptr) {
    accept_queue.put(Failure("Accept failed, SSL_new"));
    return Continue();
  }

  Try<Address> peer_address = network::peer(ssl_socket->get());
  if (!peer_address.isSome()) {
    SSL_free(accept_ssl);
    accept_queue.put(
        Failure("Could not determine peer IP for connection"));
    return Continue();
  }

  // NOTE: Right now, `openssl::configure_socket` does not do anything
  // in server mode, but we still pass the correct peer address to
  // enable modules to implement application-level logic in the future.
  Try<Nothing> configured = openssl::configure_socket(
      accept_ssl, Mode::SERVER, peer_address.get(), None());

  if (configured.isError()) {
    SSL_free(accept_ssl);
    accept_queue.put(
        Failure("Could not configure socket: " + configured.error()));
    return Continue();
  }

  // Set the SSL context in server mode.
  SSL_set_accept_state(accept_ssl);

  // Hold a weak pointer since we do not want this accept function to extend
  // the lifetime of `this` unnecessarily.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  // Pass ownership of `accept_ssl` to the newly accepted socket,
  // and start the SSL handshake. When the SSL handshake completes,
  // the listening socket will place the result (failure or success)
  // onto the listening socket's `accept_queue`.
  //
  // TODO(josephw): Add a timeout to catch/close incoming sockets which
  // never finish the SSL handshake.
  ssl_socket->set_ssl_and_do_handshake(accept_ssl)
    .onAny([weak_self, ssl_socket](Future<size_t> result) {
      std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());

      if (self == nullptr) {
        return;
      }

      if (result.isFailed()) {
        self->accept_queue.put(Failure(result.failure()));
        return;
      }

      // For verification purposes, we need to grab the address (again).
      Try<Address> address = network::address(ssl_socket->get());
      if (address.isError()) {
        self->accept_queue.put(
            Failure("Failed to get address: " + address.error()));
        return;
      }

      Try<inet::Address> inet_address =
        network::convert<inet::Address>(address.get());

      Try<Nothing> verify = openssl::verify(
          ssl_socket->ssl,
          Mode::SERVER,
          None(),
          inet_address.isSome()
            ? Some(inet_address->ip)
            : Option<net::IP>::none());

      if (verify.isError()) {
        VLOG(1) << "Failed accept, verification error: "
                << verify.error();

        self->accept_queue.put(Failure(verify.error()));
        return;
      }

      self->accept_queue.put(ssl_socket);
    });

  return Continue();
}


Future<size_t> OpenSSLSocketImpl::set_ssl_and_do_handshake(SSL* _ssl)
{
  // NOTE: We would normally create this UPID in the socket's constructor.
  // However, during libprocess initialization, the libprocess listening socket
  // is constructed before spawning processes is allowed.
  // This function is guaranteed to be called for any SSL socket that may
  // transmit encrypted data. Listening sockets will not create a UPID.
  if (compute_thread.isNone()) {
    compute_thread = spawn(new ProcessBase(), true);
  }

  // Save a reference to the SSL object.
  ssl = _ssl;

  // Construct the BIO wrapper for the underlying socket.
  //
  // NOTE: This transfers ownership of the BIO to the SSL object.
  // The BIO will be freed upon calling `SSL_free(ssl)`.
  BIO* bio = BIO_new_libprocess(get());
  SSL_set_bio(ssl, bio, bio);

  // Hold a weak pointer since the handshake may potentially never complete.
  std::weak_ptr<OpenSSLSocketImpl> weak_self(shared(this));

  return process::loop(
      compute_thread,
      [weak_self]() -> Future<int> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while doing handshake");
        }

        ERR_clear_error();
        return SSL_do_handshake(self->ssl);
      },
      [weak_self](int result) -> Future<ControlFlow<size_t>> {
        std::shared_ptr<OpenSSLSocketImpl> self(weak_self.lock());
        if (self == nullptr) {
          return Failure("Socket destroyed while doing handshake");
        }

        // Check if EOF has been reached.
        BIO* bio = SSL_get_rbio(self->ssl);
        if (BIO_eof(bio) == 1) {
          return Failure("EOF while doing handshake");
        }

        return self->handle_ssl_return_result(result, false);
      });
}


Future<ControlFlow<size_t>> OpenSSLSocketImpl::handle_ssl_return_result(
    int result,
    bool handle_as_read)
{
  if (result > 0) {
    // Successful result, potentially meaning a connected/accepted socket
    // or a completed send/recv request.
    return Break(result);
  }

  // Not a success, so we'll need to have the BIO and associated data handy.
  BIO* bio = SSL_get_rbio(ssl);
  SocketBIOData* data = reinterpret_cast<SocketBIOData*>(BIO_get_data(bio));
  CHECK_NOTNULL(data);

  int error = SSL_get_error(ssl, result);
  switch (error) {
    case SSL_ERROR_WANT_READ: {
      synchronized (data->lock) {
        if (data->recv_request.get() != nullptr) {
          return data->recv_request->future
            .then([]() -> Future<ControlFlow<size_t>> {
              return Continue();
            });
        }
      }

      return Continue();
    }
    case SSL_ERROR_WANT_WRITE: {
      synchronized (data->lock) {
        if (data->send_request.get() != nullptr) {
          return data->send_request->future
            .then([]() -> Future<ControlFlow<size_t>> {
              return Continue();
            });
        }
      }

      return Continue();
    }
    case SSL_ERROR_WANT_CLIENT_HELLO_CB:
    case SSL_ERROR_WANT_X509_LOOKUP:
      return Failure("Not implemented");
    case SSL_ERROR_ZERO_RETURN:
      if (handle_as_read) {
        return Break(0u);
      } else {
        return Failure("TLS connection has been closed");
      }
    case SSL_ERROR_WANT_ASYNC:
    case SSL_ERROR_WANT_ASYNC_JOB:
      // We do not use `SSL_MODE_ASYNC`.
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
      // We make sure the underlying socket is connected prior
      // to any interaction with the OpenSSL library.
      UNREACHABLE();
    case SSL_ERROR_SSL:
      dirty_shutdown = true;
      return Failure("Protocol error");
    case SSL_ERROR_SYSCALL:
      dirty_shutdown = true;

      // NOTE: If there is an error (`ERR_peek_error() != 0`),
      // we fall through to the default error handling case.
      if (ERR_peek_error() == 0) {
        return Failure("TCP connection closed before SSL termination");
      }
    default: {
      char buffer[1024] = {};
      std::string error_strings;
      while ((error = ERR_get_error())) {
        ERR_error_string_n(error, buffer, sizeof(buffer));
        error_strings += "\n" + stringify(buffer);
      }
      return Failure("Failed with error:" + error_strings);
    }
  };
}

} // namespace internal {
} // namespace network {
} // namespace process {

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

#include <event2/buffer.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <process/queue.hpp>
#include <process/socket.hpp>

#include <process/ssl/flags.hpp>

#include <stout/net.hpp>
#include <stout/stopwatch.hpp>
#include <stout/synchronized.hpp>

#include <stout/os/close.hpp>
#include <stout/os/dup.hpp>
#include <stout/os/fcntl.hpp>

#include "libevent.hpp"
#include "libevent_ssl_socket.hpp"
#include "openssl.hpp"
#include "poll_socket.hpp"

// Locking:
//
// We use the BEV_OPT_THREADSAFE flag when constructing bufferevents
// so that all **functions that are called from the event loop that
// take a bufferevent as a parameter will automatically have the
// lock acquired**.
//
// This means that everywhere that the libevent library does not
// already lock the bev, we need to manually 'synchronize (bev) {'.
// To further complicate matters, due to a deadlock scneario in
// libevent-openssl (v 2.0.21) we currently modify bufferevents using
// continuations in the event loop, but these functions, while run
// from within the event loop, are not passed the 'bev' as a parameter
// and thus MUST use 'synchronized (bev)'. See 'Continuation' comment
// below for more details on why we need to invoke these continuations
// from within the event loop.

// Continuations via 'run_in_event_loop(...)':
//
// There is a deadlock scenario in libevent-openssl (v 2.0.21) when
// modifying the bufferevent (bev) from another thread (not the event
// loop). To avoid this we run all bufferevent manipulation logic in
// continuations that are executed within the event loop.

// DISALLOW_SHORT_CIRCUIT:
//
// We disallow short-circuiting in 'run_in_event_loop' due to a bug in
// libevent_openssl with deferred callbacks still being called (still
// in the run queue) even though a bev has been disabled.

using std::queue;
using std::string;

using process::network::openssl::Mode;

// Specialization of 'synchronize' to use bufferevent with the
// 'synchronized' macro.
static Synchronized<bufferevent> synchronize(bufferevent* bev)
{
  return Synchronized<bufferevent>(
      bev,
      [](bufferevent* bev) { bufferevent_lock(bev); },
      [](bufferevent* bev) { bufferevent_unlock(bev); });
}

namespace process {
namespace network {
namespace internal {

Try<std::shared_ptr<SocketImpl>> LibeventSSLSocketImpl::create(int_fd s)
{
  openssl::initialize();

  if (!openssl::flags().enabled) {
    return Error("SSL is disabled");
  }

  auto socket = std::make_shared<LibeventSSLSocketImpl>(s);
  // See comment at 'initialize' declaration for why we call this.
  socket->initialize();
  return socket;
}


LibeventSSLSocketImpl::~LibeventSSLSocketImpl()
{
  // We defer termination and destruction of all event loop specific
  // calls and structures. This is a safety against the socket being
  // destroyed before existing event loop calls have completed since
  // they require valid data structures (the weak pointer).
  //
  // Release ownership of the file descriptor so that
  // we can defer closing the socket.
  int_fd fd = release();
  CHECK(fd >= 0);

  evconnlistener* _listener = listener;
  bufferevent* _bev = bev;
  std::weak_ptr<LibeventSSLSocketImpl>* _event_loop_handle = event_loop_handle;

  run_in_event_loop(
      [_listener, _bev, _event_loop_handle, fd]() {
        // Once this lambda is called, it should not be possible for
        // more event loop callbacks to be triggered with 'this->bev'.
        // This is important because we delete event_loop_handle which
        // is the callback argument for any event loop callbacks.
        // This lambda is responsible for ensuring 'this->bev' is
        // disabled, and cleaning up any remaining state associated
        // with the event loop.

        CHECK(__in_event_loop__);

        if (_listener != nullptr) {
          evconnlistener_free(_listener);
        }

        if (_bev != nullptr) {
          // NOTE: Removes all future callbacks using 'this->bev'.
          bufferevent_disable(_bev, EV_READ | EV_WRITE);

          SSL* ssl = bufferevent_openssl_get_ssl(_bev);
          SSL_free(ssl);
          bufferevent_free(_bev);
        }

        CHECK_SOME(os::close(fd)) << "Failed to close socket";

        delete _event_loop_handle;
      },
      DISALLOW_SHORT_CIRCUIT);
}


void LibeventSSLSocketImpl::initialize()
{
  event_loop_handle = new std::weak_ptr<LibeventSSLSocketImpl>(shared(this));
}


Try<Nothing, SocketError> LibeventSSLSocketImpl::shutdown(int how)
{
  // Nothing to do if this socket was never initialized.
  synchronized (lock) {
    if (bev == nullptr) {
      // If it was not initialized, then there should also be no
      // requests.
      CHECK(connect_request.get() == nullptr);
      CHECK(recv_request.get() == nullptr);
      CHECK(send_request.get() == nullptr);

      // We expect this to fail and generate an 'ENOTCONN' failure as
      // no connection should exist at this point.
      if (::shutdown(s, how) < 0) {
        return SocketError();
      }

      return Nothing();
    }
  }

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self]() {
        CHECK(__in_event_loop__);
        CHECK(self);

        if (self->bev == nullptr) {
          return;
        }

        synchronized (self->bev) {
          Owned<RecvRequest> request;

          // Swap the 'recv_request' under the object lock.
          synchronized (self->lock) {
            std::swap(request, self->recv_request);
          }

          // If there is still a pending receive request then close it.
          if (request.get() != nullptr) {
            request->promise
              .set(bufferevent_read(self->bev, request->data, request->size));
          }

          // Workaround for SSL shutdown, see http://www.wangafu.net/~nickm/libevent-book/Ref6a_advanced_bufferevents.html // NOLINT
          SSL* ssl = bufferevent_openssl_get_ssl(self->bev);
          SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
          SSL_shutdown(ssl);
        }
      },
      DISALLOW_SHORT_CIRCUIT);

  return Nothing();
}


// Only runs in event loop. No locks required. See 'Locking' note at
// top of file.
void LibeventSSLSocketImpl::recv_callback(bufferevent* /*bev*/, void* arg)
{
  CHECK(__in_event_loop__);

  std::weak_ptr<LibeventSSLSocketImpl>* handle =
    reinterpret_cast<std::weak_ptr<LibeventSSLSocketImpl>*>(CHECK_NOTNULL(arg));

  std::shared_ptr<LibeventSSLSocketImpl> impl(handle->lock());

  // Don't call the 'recv_callback' unless the socket is still valid.
  if (impl != nullptr) {
    impl->recv_callback();
  }
}


// Only runs in event loop. Member function continuation of static
// 'recv_callback'. This function can be called from two places -
// a) `LibeventSSLSocketImpl::recv` when a new Socket::recv is called and there
//    is buffer available to read.
// b) `LibeventSSLSocketImpl::recv_callback when libevent calls the deferred
//    read callback.
void LibeventSSLSocketImpl::recv_callback()
{
  CHECK(__in_event_loop__);

  Owned<RecvRequest> request;

  const size_t buffer_length = evbuffer_get_length(bufferevent_get_input(bev));

  // Swap out the request object IFF there is buffer available to read. We check
  // this here because it is possible that when the libevent deferred callback
  // was called, a Socket::recv context already read the buffer from the event.
  // Following sequence is possible:
  // a. libevent finds a buffer ready to be read.
  // b. libevent queues buffer event to be dispatched.
  // c. Socket::recv is called that creates a new request.
  // d. Socket::recv finds buffer length > 0.
  // e. Socket::recv reads the buffer.
  // f. A new request Socket::recv is called which creates a new request.
  // g. libevent callback is called for the event queued at step b.
  // h. libevent callback finds the length of the buffer as 0 but the request is
  //    a non-nullptr due to step f.
  if (buffer_length > 0 || received_eof) {
    synchronized (lock) {
      std::swap(request, recv_request);
    }
  }

  if (request.get() != nullptr) {
    if (buffer_length > 0) {
      size_t length = bufferevent_read(bev, request->data, request->size);
      CHECK(length > 0);

      request->promise.set(length);
    } else {
      CHECK(received_eof);
      request->promise.set(0);
    }
  }
}


// Only runs in event loop. No locks required. See 'Locking' note at
// top of file.
void LibeventSSLSocketImpl::send_callback(bufferevent* /*bev*/, void* arg)
{
  CHECK(__in_event_loop__);

  std::weak_ptr<LibeventSSLSocketImpl>* handle =
    reinterpret_cast<std::weak_ptr<LibeventSSLSocketImpl>*>(CHECK_NOTNULL(arg));

  std::shared_ptr<LibeventSSLSocketImpl> impl(handle->lock());

  // Don't call the 'send_callback' unless the socket is still valid.
  if (impl != nullptr) {
    impl->send_callback();
  }
}


// Only runs in event loop. Member function continuation of static
// 'recv_callback'.
void LibeventSSLSocketImpl::send_callback()
{
  CHECK(__in_event_loop__);

  Owned<SendRequest> request;

  synchronized (lock) {
    std::swap(request, send_request);
  }

  if (request.get() != nullptr) {
    request->promise.set(request->size);
  }
}


// Only runs in event loop. No locks required. See 'Locking' note at
// top of file.
void LibeventSSLSocketImpl::event_callback(
    bufferevent* /*bev*/,
    short events,
    void* arg)
{
  CHECK(__in_event_loop__);

  std::weak_ptr<LibeventSSLSocketImpl>* handle =
    reinterpret_cast<std::weak_ptr<LibeventSSLSocketImpl>*>(CHECK_NOTNULL(arg));

  std::shared_ptr<LibeventSSLSocketImpl> impl(handle->lock());

  // Don't call the 'event_callback' unless the socket is still valid.
  if (impl != nullptr) {
    impl->event_callback(events);
  }
}


// Only runs in event loop. Member function continuation of static
// 'recv_callback'.
void LibeventSSLSocketImpl::event_callback(short events)
{
  CHECK(__in_event_loop__);

  // TODO(bmahler): Libevent's invariant is that `events` contains:
  //
  //   (1) one of BEV_EVENT_READING or BEV_EVENT_WRITING to
  //       indicate whether the event was on the read or write path.
  //
  //   (2) one of BEV_EVENT_EOF, BEV_EVENT_ERROR, BEV_EVENT_TIMEOUT,
  //       BEV_EVENT_CONNECTED.
  //
  // (1) allows us to handle read and write errors separately.
  // HOWEVER, for SSL bufferevents in 2.0.x, libevent never seems
  // to tell us about BEV_EVENT_READING or BEV_EVENT_WRITING,
  // which forces us to write incorrect logic here by treating all
  // events as affecting both reads and writes.
  //
  // This has been fixed in 2.1.x:
  //   2.1 "What's New":
  //     https://github.com/libevent/libevent/blob/release-2.1.8-stable/whatsnew-2.1.txt#L333-L335 // NOLINT
  //   Commit:
  //     https://github.com/libevent/libevent/commit/f7eb69ace
  //
  // We should require 2.1.x so that we can correctly distinguish
  // between the read and write errors, and not have two code paths
  // depending on the libevent version, see MESOS-5999, MESOS-6770.

  Owned<RecvRequest> current_recv_request;
  Owned<SendRequest> current_send_request;
  Owned<ConnectRequest> current_connect_request;

  if (events & BEV_EVENT_EOF ||
      events & BEV_EVENT_CONNECTED ||
      events & BEV_EVENT_ERROR) {
    synchronized (lock) {
      std::swap(current_recv_request, recv_request);
      std::swap(current_send_request, send_request);
      std::swap(current_connect_request, connect_request);
    }
  }

  // First handle EOF, we also look for `BEV_EVENT_ERROR` with
  // `EVUTIL_SOCKET_ERROR() == 0` since this occurs as a result
  // of a "dirty" SSL shutdown (i.e. TCP close before SSL close)
  // or when this socket has been shut down and further sends
  // are performed.
  //
  // TODO(bmahler): We don't expose "dirty" SSL shutdowns as
  // recv errors, but perhaps we should?
  if (events & BEV_EVENT_EOF ||
     (events & BEV_EVENT_ERROR && EVUTIL_SOCKET_ERROR() == 0)) {
    received_eof = true;

    if (current_recv_request.get() != nullptr) {
      // Drain any remaining data from the bufferevent or complete the
      // promise with 0 to signify EOF. Because we set `received_eof`,
      // subsequent calls to `recv` will return 0 if there is no data
      // remaining on the buffer.
      if (evbuffer_get_length(bufferevent_get_input(bev)) > 0) {
        size_t length =
          bufferevent_read(
              bev,
              current_recv_request->data,
              current_recv_request->size);
        CHECK(length > 0);

        current_recv_request->promise.set(length);
      } else {
        current_recv_request->promise.set(0);
      }
    }

    if (current_send_request.get() != nullptr) {
      current_send_request->promise.fail("Failed send: connection closed");
    }

    if (current_connect_request.get() != nullptr) {
      SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(bev));
      SSL_free(ssl);
      bufferevent_free(CHECK_NOTNULL(bev));
      bev = nullptr;
      current_connect_request->promise.fail(
          "Failed connect: connection closed");
    }
  } else if (events & BEV_EVENT_CONNECTED) {
    // We should not have receiving or sending request while still
    // connecting.
    CHECK(current_recv_request.get() == nullptr);
    CHECK(current_send_request.get() == nullptr);
    CHECK_NOTNULL(current_connect_request.get());

    // If we're connecting, then we've succeeded. Time to do
    // post-verification.
    CHECK_NOTNULL(bev);
    CHECK(client_config.isSome());

    if (client_config->verify) {
      // Do post-validation of connection.
      SSL* ssl = bufferevent_openssl_get_ssl(bev);

      Try<Nothing> verify = client_config->verify(
          ssl, client_config->servername, peer_ip);

      if (verify.isError()) {
        VLOG(1) << "Failed connect, verification error: " << verify.error();
        SSL_free(ssl);
        bufferevent_free(bev);
        bev = nullptr;
        current_connect_request->promise.fail(verify.error());
        return;
      }
    }

    current_connect_request->promise.set(Nothing());
  } else if (events & BEV_EVENT_ERROR) {
    CHECK(EVUTIL_SOCKET_ERROR() != 0);
    std::ostringstream error_stream;
    error_stream << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());

    // If there is a valid error, fail any requests and log the error.
    VLOG(1) << "Socket error: " << error_stream.str();

    if (current_recv_request.get() != nullptr) {
      current_recv_request->promise.fail(
          "Failed recv, connection error: " +
          error_stream.str());
    }

    if (current_send_request.get() != nullptr) {
      current_send_request->promise.fail(
          "Failed send, connection error: " +
          error_stream.str());
    }

    if (current_connect_request.get() != nullptr) {
      SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(bev));
      SSL_free(ssl);
      bufferevent_free(CHECK_NOTNULL(bev));
      bev = nullptr;
      current_connect_request->promise.fail(
          "Failed connect, connection error: " +
          error_stream.str());
    }
  }
}


LibeventSSLSocketImpl::LibeventSSLSocketImpl(int_fd _s)
  : SocketImpl(_s),
    bev(nullptr),
    listener(nullptr),
    recv_request(nullptr),
    send_request(nullptr),
    connect_request(nullptr),
    event_loop_handle(nullptr) {}


LibeventSSLSocketImpl::LibeventSSLSocketImpl(
    int_fd _s,
    bufferevent* _bev)
  : SocketImpl(_s),
    bev(_bev),
    listener(nullptr),
    recv_request(nullptr),
    send_request(nullptr),
    connect_request(nullptr),
    event_loop_handle(nullptr) {}


Future<Nothing> LibeventSSLSocketImpl::connect(
    const Address& address)
{
  LOG(FATAL) << "No TLS config was passed to a SSL socket.";
}


Future<Nothing> LibeventSSLSocketImpl::connect(
    const Address& address,
    const openssl::TLSClientConfig& config)
{
  if (bev != nullptr) {
    return Failure("Socket is already connected");
  }

  if (connect_request.get() != nullptr) {
    return Failure("Socket is already connecting");
  }

  if (config.ctx == nullptr) {
    return Failure("Invalid SSL context");
  }

  SSL* ssl = SSL_new(config.ctx);
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

  // Construct the bufferevent in the connecting state.
  // We set 'BEV_OPT_DEFER_CALLBACKS' to avoid calling the
  // 'event_callback' before 'bufferevent_socket_connect' returns.
  CHECK(bev == nullptr);
  bev = bufferevent_openssl_socket_new(
      base,
      s,
      ssl,
      BUFFEREVENT_SSL_CONNECTING,
      BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS);

  if (bev == nullptr) {
    // We need to free 'ssl' here because the bev won't clean it up
    // for us.
    SSL_free(ssl);
    return Failure("Failed to connect: bufferevent_openssl_socket_new");
  }

  if (address.family() == Address::Family::INET4 ||
      address.family() == Address::Family::INET6) {
    inet::Address inetAddress =
      CHECK_NOTERROR(network::convert<inet::Address>(address));

    // Determine the 'peer_ip' from the address we're connecting to in
    // order to properly verify the certificate later.
    peer_ip = inetAddress.ip;
  }

  if (config.servername.isSome()) {
    VLOG(2) << "Connecting to " << config.servername.get() << " at " << address;
  } else {
    VLOG(2) << "Connecting to " << address << " with no hostname specified";
  }

  // Optimistically construct a 'ConnectRequest' and future.
  Owned<ConnectRequest> request(new ConnectRequest());
  Future<Nothing> future = request->promise.future();

  // Assign 'connect_request' under lock, fail on error.
  synchronized (lock) {
    if (connect_request.get() != nullptr) {
      SSL_free(ssl);
      bufferevent_free(bev);
      bev = nullptr;
      return Failure("Socket is already connecting");
    }
    std::swap(request, connect_request);
  }

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self, address]() {
        sockaddr_storage addr = address;

          // Assign the callbacks for the bufferevent. We do this
          // before the 'bufferevent_socket_connect()' call to avoid
          // any race on the underlying buffer events becoming ready.
          bufferevent_setcb(
              self->bev,
              &LibeventSSLSocketImpl::recv_callback,
              &LibeventSSLSocketImpl::send_callback,
              &LibeventSSLSocketImpl::event_callback,
              CHECK_NOTNULL(self->event_loop_handle));

          // Explicitly enable read and write bufferevents as needed
          // for libevent >= 2.1.6. See MESOS-9265.
          bufferevent_enable(self->bev, EV_READ | EV_WRITE);

          if (bufferevent_socket_connect(
                  self->bev,
                  reinterpret_cast<sockaddr*>(&addr),
                  address.size()) < 0) {
            SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(self->bev));
            SSL_free(ssl);
            bufferevent_free(self->bev);

            self->bev = nullptr;

            Owned<ConnectRequest> request;

            // Swap out the 'connect_request' so we can destroy it
            // outside of the lock.
            synchronized (self->lock) {
              std::swap(request, self->connect_request);
            }

            CHECK_NOTNULL(request.get());

            // Fail the promise since we failed to connect.
            request->promise.fail(
                "Failed to connect: bufferevent_socket_connect");
          }
      },
      DISALLOW_SHORT_CIRCUIT);

  return future;
}


Future<size_t> LibeventSSLSocketImpl::recv(char* data, size_t size)
{
  // Optimistically construct a 'RecvRequest' and future.
  Owned<RecvRequest> request(new RecvRequest(data, size));
  std::weak_ptr<LibeventSSLSocketImpl> weak_self(shared(this));

  // If the user of the future decides to 'discard', then we want to
  // test whether the request was already satisfied.
  // We capture a 'weak_ptr' to 'this' (as opposed to a 'shared_ptr')
  // because the socket could be destroyed before this lambda is
  // executed. If we used a 'shared_ptr' then this lambda could extend
  // the life-time of 'this' unnecessarily.
  Future<size_t> future = request->promise.future()
    .onDiscard([weak_self]() {
      // Extend the life-time of 'this' through the execution of the
      // lambda in the event loop. Note: The 'self' needs to be
      // explicitly captured because we're not using it in the body of
      // the lambda. We can use a 'shared_ptr' because
      // run_in_event_loop is guaranteed to execute.
      std::shared_ptr<LibeventSSLSocketImpl> self(weak_self.lock());

      if (self != nullptr) {
        run_in_event_loop(
            [self]() {
              CHECK(__in_event_loop__);
              CHECK(self);

              Owned<RecvRequest> request;

              synchronized (self->lock) {
                std::swap(request, self->recv_request);
              }

              // Only discard if the request hasn't already been
              // satisfied.
              if (request.get() != nullptr) {
                // Discard the promise outside of the object lock as
                // the callbacks can be expensive.
                request->promise.discard();
              }
            },
            DISALLOW_SHORT_CIRCUIT);
      }
    });

  // Assign 'recv_request' under lock, fail on error.
  synchronized (lock) {
    if (recv_request.get() != nullptr) {
      return Failure("Socket is already receiving");
    }
    std::swap(request, recv_request);
  }

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self]() {
        CHECK(__in_event_loop__);
        CHECK(self);

        bool recv = false;

        // We check to see if 'recv_request' is null. It would be null
        // if a 'discard' happened before this lambda was executed.
        synchronized (self->lock) {
          recv = self->recv_request.get() != nullptr;
        }

        // Only try to read existing data from the bufferevent if the
        // request has not already been discarded.
        if (recv) {
          synchronized (self->bev) {
            evbuffer* input = bufferevent_get_input(self->bev);
            size_t length = evbuffer_get_length(input);

            // If there is already data in the buffer or an EOF has
            // been received, fulfill the 'recv_request' by calling
            // 'recv_callback()'. Otherwise do nothing and wait for
            // the 'recv_callback' to run when we receive data over
            // the network.
            if (length > 0 || self->received_eof) {
              self->recv_callback();
            }
          }
        }
      },
      DISALLOW_SHORT_CIRCUIT);

  return future;
}


Future<size_t> LibeventSSLSocketImpl::send(const char* data, size_t size)
{
  // Optimistically construct a 'SendRequest' and future.
  Owned<SendRequest> request(new SendRequest(size));
  Future<size_t> future = request->promise.future();

  // We don't add an 'onDiscard' continuation to send because we can
  // not accurately detect how many bytes have been sent. Once we pass
  // the data to the bufferevent, there is the possibility that parts
  // of it have been sent. Another reason is that if we send partial
  // messages (discard only a part of the data), then it is likely
  // that the receiving end will fail parsing the message.

  // Assign 'send_request' under lock, fail on error.
  synchronized (lock) {
    if (send_request.get() != nullptr) {
      return Failure("Socket is already sending");
    }
    std::swap(request, send_request);
  }

  evbuffer* buffer = CHECK_NOTNULL(evbuffer_new());

  int result = evbuffer_add(buffer, data, size);
  CHECK_EQ(0, result);

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self, buffer]() {
        CHECK(__in_event_loop__);
        CHECK(self);

        // Check if the socket is closed or the write end has
        // encountered an error in the interim (i.e. we received
        // a BEV_EVENT_ERROR with BEV_EVENT_WRITING).
        bool write = false;

        synchronized (self->lock) {
          if (self->send_request.get() != nullptr) {
            write = true;
          }
        }

        if (write) {
          int result = bufferevent_write_buffer(self->bev, buffer);
          CHECK_EQ(0, result);
        }

        evbuffer_free(buffer);
      },
      DISALLOW_SHORT_CIRCUIT);

  return future;
}


Future<size_t> LibeventSSLSocketImpl::sendfile(
    int_fd fd,
    off_t offset,
    size_t size)
{
  // Optimistically construct a 'SendRequest' and future.
  Owned<SendRequest> request(new SendRequest(size));
  Future<size_t> future = request->promise.future();

  // Assign 'send_request' under lock, fail on error.
  synchronized (lock) {
    if (send_request.get() != nullptr) {
      return Failure("Socket is already sending");
    }
    std::swap(request, send_request);
  }

  // Duplicate the file descriptor because Libevent will take ownership
  // and control the lifecycle separately.
  //
  // TODO(josephw): We can avoid duplicating the file descriptor in
  // future versions of Libevent. In Libevent versions 2.1.2 and later,
  // we may use `evbuffer_file_segment_new` and `evbuffer_add_file_segment`
  // instead of `evbuffer_add_file`.
  Try<int_fd> dup = os::dup(fd);
  if (dup.isError()) {
    return Failure(dup.error());
  }

  // NOTE: This is *not* an `int_fd` because `libevent` requires a CRT
  // integer file descriptor, which we allocate and then use
  // exclusively here.
#ifdef __WINDOWS__
  int owned_fd = dup->crt();
  // The `os::cloexec` and `os::nonblock` functions do nothing on
  // Windows, and cannot be called because they take `int_fd`.
#else
  int owned_fd = dup.get();

  // Set the close-on-exec flag.
  Try<Nothing> cloexec = os::cloexec(owned_fd);
  if (cloexec.isError()) {
    os::close(owned_fd);
    return Failure(
        "Failed to set close-on-exec on duplicated file descriptor: " +
        cloexec.error());
  }

  // Make the file descriptor non-blocking.
  Try<Nothing> nonblock = os::nonblock(owned_fd);
  if (nonblock.isError()) {
    os::close(owned_fd);
    return Failure(
        "Failed to make duplicated file descriptor non-blocking: " +
        nonblock.error());
  }
#endif // __WINDOWS__

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self, owned_fd, offset, size]() {
        CHECK(__in_event_loop__);
        CHECK(self);

        // Check if the socket is closed or the write end has
        // encountered an error in the interim (i.e. we received
        // a BEV_EVENT_ERROR with BEV_EVENT_WRITING).
        bool write = false;

        synchronized (self->lock) {
          if (self->send_request.get() != nullptr) {
            write = true;
          }
        }

        if (write) {
          // NOTE: `evbuffer_add_file` will take ownership of the file
          // descriptor and close it after it has finished reading it.
          int result = evbuffer_add_file(
              bufferevent_get_output(self->bev),
              owned_fd,
              offset,
              size);
          CHECK_EQ(0, result);
        } else {
#ifdef __WINDOWS__
          // NOTE: `os::close()` on Windows is not compatible with CRT
          // file descriptors, only `HANDLE` and `SOCKET` types.
          ::_close(owned_fd);
#else
          os::close(owned_fd);
#endif // __WINDOWS__
        }
      },
      DISALLOW_SHORT_CIRCUIT);

  return future;
}


Try<Nothing> LibeventSSLSocketImpl::listen(int backlog)
{
  if (listener != nullptr) {
    return Error("Socket is already listening");
  }

  CHECK(bev == nullptr);

  // NOTE: Accepted sockets are nonblocking by default in libevent, but
  // can be set to block via the `LEV_OPT_LEAVE_SOCKETS_BLOCKING`
  // flag for `evconnlistener_new`.
  listener = evconnlistener_new(
      base,
      [](evconnlistener* listener,
         evutil_socket_t socket,
         sockaddr* addr,
         int addr_length,
         void* arg) {
        CHECK(__in_event_loop__);

        std::weak_ptr<LibeventSSLSocketImpl>* handle =
          reinterpret_cast<std::weak_ptr<LibeventSSLSocketImpl>*>(
              CHECK_NOTNULL(arg));

        std::shared_ptr<LibeventSSLSocketImpl> impl(handle->lock());

#ifndef __WINDOWS__
        // NOTE: Passing the flag `LEV_OPT_CLOSE_ON_EXEC` into
        // `evconnlistener_new` would atomically set `SOCK_CLOEXEC`
        // on the accepted socket. However, this flag is not supported
        // in the minimum recommended version of libevent (2.0.22).
        Try<Nothing> cloexec = os::cloexec(socket);
        if (cloexec.isError()) {
          VLOG(2) << "Failed to accept, cloexec: " << cloexec.error();

          // Propagate the error through the listener's `accept_queue`.
          if (impl != nullptr) {
            impl->accept_queue.put(
                Failure("Failed to accept, cloexec: " + cloexec.error()));
          }

          os::close(socket);
          return;
        }
#endif // __WINDOWS__

        if (impl != nullptr) {
          // We pass the 'listener' into the 'AcceptRequest' because
          // this function could be executed before 'this->listener'
          // is set.
          AcceptRequest* request =
            new AcceptRequest(
                  // NOTE: The `int_fd` must be explicitly constructed
                  // to avoid the `intptr_t` being casted to an `int`,
                  // resulting in a `HANDLE` instead of a `SOCKET` on
                  // Windows.
                  int_fd(socket),
                  listener,
                  CHECK_NOTERROR(network::Address::create(addr, addr_length)));

          impl->accept_callback(request);
        }
      },
      event_loop_handle,
      LEV_OPT_REUSEABLE,
      backlog,
      s);

  if (listener == nullptr) {
    return Error("Failed to listen on socket");
  }

  // TODO(jmlvanre): attach an error callback.

  return Nothing();
}


Future<std::shared_ptr<SocketImpl>> LibeventSSLSocketImpl::accept()
{
  // Note that due to MESOS-8448, when the caller discards, it's
  // possible that we pull an accepted socket out of the queue but
  // drop it when `.then` transitions to discarded rather than
  // executing the continuation. This is currently acceptable since
  // callers only discard when they're breaking their accept loop.
  // However, from an API perspective, we shouldn't be dropping
  // the socket on the floor.
  //
  // We explicitly specify the return type to avoid a type deduction
  // issue in some versions of clang. See MESOS-2943.
  return accept_queue.get()
    .then([](const Future<std::shared_ptr<SocketImpl>>& impl)
      -> Future<std::shared_ptr<SocketImpl>> {
      CHECK(!impl.isPending());
      return impl;
    });
}


void LibeventSSLSocketImpl::peek_callback(
    evutil_socket_t fd,
    short what,
    void* arg)
{
  CHECK(__in_event_loop__);

  CHECK(what & EV_READ);
  char data[6];

  // Try to peek the first 6 bytes of the message.
  ssize_t size = ::recv(fd, data, 6, MSG_PEEK);

  // Based on the function 'ssl23_get_client_hello' in openssl, we
  // test whether to dispatch to the SSL or non-SSL based accept based
  // on the following rules:
  //   1. If there are fewer than 3 bytes: non-SSL.
  //   2. If the 1st bit of the 1st byte is set AND the 3rd byte is
  //          equal to SSL2_MT_CLIENT_HELLO: SSL.
  //   3. If the 1st byte is equal to SSL3_RT_HANDSHAKE AND the 2nd
  //      byte is equal to SSL3_VERSION_MAJOR and the 6th byte is
  //      equal to SSL3_MT_CLIENT_HELLO: SSL.
  //   4. Otherwise: non-SSL.

  // For an ascii based protocol to falsely get dispatched to SSL it
  // needs to:
  //   1. Start with an invalid ascii character (0x80).
  //   2. OR have the first 2 characters be a SYN followed by ETX, and
  //          then the 6th character be SOH.
  // These conditions clearly do not constitute valid HTTP requests,
  // and are unlikely to collide with other existing protocols.

  bool ssl = false; // Default to rule 4.

  if (size < 2) { // Rule 1.
    ssl = false;
  } else if ((data[0] & 0x80) && data[2] == SSL2_MT_CLIENT_HELLO) { // Rule 2.
    ssl = true;
  } else if (data[0] == SSL3_RT_HANDSHAKE &&
             data[1] == SSL3_VERSION_MAJOR &&
             data[5] == SSL3_MT_CLIENT_HELLO) { // Rule 3.
    ssl = true;
  }

  AcceptRequest* request = reinterpret_cast<AcceptRequest*>(arg);

  // We call 'event_free()' here because it ensures the event is made
  // non-pending and inactive before it gets deallocated.
  event_free(request->peek_event);
  request->peek_event = nullptr;

  if (ssl) {
    accept_SSL_callback(request);
  } else {
    // Downgrade to a non-SSL socket implementation.
    //
    // NOTE: The `int_fd` must be explicitly constructed to avoid the
    // `intptr_t` being casted to an `int`, resulting in a `HANDLE`
    // instead of a `SOCKET` on Windows.
    Try<std::shared_ptr<SocketImpl>> impl = PollSocketImpl::create(int_fd(fd));
    if (impl.isError()) {
      request->promise.fail(impl.error());
    } else {
      request->promise.set(impl.get());
    }

    delete request;
  }
}


void LibeventSSLSocketImpl::accept_callback(AcceptRequest* request)
{
  CHECK(__in_event_loop__);

  Queue<Future<std::shared_ptr<SocketImpl>>> accept_queue_ = accept_queue;

  // After the socket is accepted, it must complete the SSL
  // handshake (or be downgraded to a regular socket) before
  // we put it in the queue of connected sockets.
  request->promise.future()
    .onAny([accept_queue_](Future<std::shared_ptr<SocketImpl>> impl) mutable {
      accept_queue_.put(impl);
    });

  // If we support downgrading the connection, first wait for this
  // socket to become readable. We will then MSG_PEEK it to test
  // whether we want to dispatch as SSL or non-SSL.
  if (openssl::flags().support_downgrade) {
    request->peek_event = event_new(
        base,
        request->socket,
        EV_READ,
        &LibeventSSLSocketImpl::peek_callback,
        request);
    event_add(request->peek_event, nullptr);
  } else {
    accept_SSL_callback(request);
  }
}


void LibeventSSLSocketImpl::accept_SSL_callback(AcceptRequest* request)
{
  CHECK(__in_event_loop__);

  // Set up SSL object.
  SSL* ssl = SSL_new(openssl::context());
  if (ssl == nullptr) {
    // TODO(bmahler): Log the error reason.
    request->promise.fail("Failed to SSL_new");
    delete request;
    return;
  }

  // NOTE: Right now, the configure callback does not do anything in server
  // mode, but we still pass the correct peer address to enable modules to
  // implement application-level logic in the future.
  Try<Nothing> configured = openssl::configure_socket(
      ssl, openssl::Mode::SERVER, request->address, None());

  if (configured.isError()) {
    request->promise.fail(
        "Failed to openssl::configure_socket"
        " for " + stringify(request->address) + ": " + configured.error());
    delete request;
    return;
  }

  // We use 'request->listener' because 'this->listener' may not have
  // been set by the time this function is executed. See comment in
  // the lambda for evconnlistener_new in
  // 'LibeventSSLSocketImpl::listen'.
  event_base* ev_base = evconnlistener_get_base(request->listener);

  // Construct the bufferevent in the accepting state.
  bufferevent* bev = bufferevent_openssl_socket_new(
      ev_base,
      request->socket,
      ssl,
      BUFFEREVENT_SSL_ACCEPTING,
      BEV_OPT_THREADSAFE);

  if (bev == nullptr) {
    // TODO(bmahler): Log the error reason.
    request->promise.fail("Failed to bufferevent_openssl_socket_new"
                          " for " + stringify(request->address));
    SSL_free(ssl);
    delete request;
    return;
  }

  bufferevent_setcb(
      bev,
      nullptr,
      nullptr,
      [](bufferevent* bev, short events, void* arg) {
        // This handles error states or 'BEV_EVENT_CONNECTED' events
        // and satisfies the promise by constructing a new socket if
        // the connection was successfuly established.
        CHECK(__in_event_loop__);

        AcceptRequest* request =
          reinterpret_cast<AcceptRequest*>(CHECK_NOTNULL(arg));

        if (events & BEV_EVENT_EOF) {
          request->promise.fail(
              "Connection closed for " + stringify(request->address));
        } else if (events & BEV_EVENT_CONNECTED) {
          SSL* ssl = bufferevent_openssl_get_ssl(bev);
          CHECK_NOTNULL(ssl);

          Try<net::IP> ip = net::IP::create(request->address);

          Try<Nothing> verify = openssl::verify(
              ssl,
              Mode::SERVER,
              None(),
              ip.isSome() ? Option<net::IP>(*ip) : None());

          if (verify.isError()) {
            VLOG(1) << "Failed accept for " << request->address
                    << ", verification error: " << verify.error();
            request->promise.fail(verify.error());
            SSL_free(ssl);
            bufferevent_free(bev);
            // TODO(jmlvanre): Clean up for readability. Consider RAII
            // or constructing the impl earlier.
            CHECK(request->socket >= 0);
            Try<Nothing> close = os::close(request->socket);
            if (close.isError()) {
              LOG(FATAL)
                << "Failed to close socket " << request->socket
                << ": " << close.error();
            }
            delete request;
            return;
          }

          auto impl = std::shared_ptr<LibeventSSLSocketImpl>(
              new LibeventSSLSocketImpl(
                  request->socket,
                  bev));

          // See comment at 'initialize' declaration for why we call
          // this.
          impl->initialize();

          // We have to wait till after 'initialize()' is invoked for
          // event_loop_handle to be valid as a callback argument for
          // the callbacks.
          bufferevent_setcb(
              CHECK_NOTNULL(impl->bev),
              &LibeventSSLSocketImpl::recv_callback,
              &LibeventSSLSocketImpl::send_callback,
              &LibeventSSLSocketImpl::event_callback,
              CHECK_NOTNULL(impl->event_loop_handle));

          // Explicitly enable read and write bufferevents as needed
          // for libevent >= 2.1.6. See MESOS-9265.
          bufferevent_enable(bev, EV_READ | EV_WRITE);

          request->promise.set(std::dynamic_pointer_cast<SocketImpl>(impl));
        } else if (events & BEV_EVENT_ERROR) {
          std::ostringstream stream;
          if (EVUTIL_SOCKET_ERROR() != 0) {
            stream << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
          } else {
            char buffer[1024] = {};
            unsigned long error = bufferevent_get_openssl_error(bev);
            ERR_error_string_n(error, buffer, sizeof(buffer));
            stream << buffer;
          }

          // Fail the accept request and log the error.
          VLOG(1) << "Failed accept for " << request->address
                  << ": " << stream.str();

          SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(bev));
          SSL_free(ssl);
          bufferevent_free(bev);

          // TODO(jmlvanre): Clean up for readability. Consider RAII
          // or constructing the impl earlier.
          CHECK(request->socket >= 0);
          Try<Nothing> close = os::close(request->socket);
          if (close.isError()) {
            LOG(FATAL)
              << "Failed to close socket " << stringify(request->socket)
              << ": " << close.error();
          }
          request->promise.fail(
              "Failed to complete SSL connection for " +
              stringify(request->address) + ": " + stream.str());
        }

        delete request;
      },
      request);
}

} // namespace internal {
} // namespace network {
} // namespace process {

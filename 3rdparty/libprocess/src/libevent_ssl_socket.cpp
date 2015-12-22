/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

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

#include <stout/net.hpp>
#include <stout/synchronized.hpp>

#include "libevent.hpp"
#include "libevent_ssl_socket.hpp"
#include "openssl.hpp"

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

Try<std::shared_ptr<Socket::Impl>> LibeventSSLSocketImpl::create(int s)
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

  // Copy the members that we are interested in. This is necessary
  // because 'this' points to memory that may be re-allocated and
  // invalidate any reference to 'this->XXX'. We want to manipulate
  // or use these data structures within the finalization lambda
  // below.
  evconnlistener* _listener = listener;
  bufferevent* _bev = bev;
  std::weak_ptr<LibeventSSLSocketImpl>* _event_loop_handle = event_loop_handle;
  int ssl_socket_fd = ssl_connect_fd;

  run_in_event_loop(
      [_listener, _bev, _event_loop_handle, ssl_socket_fd]() {
        // Once this lambda is called, it should not be possible for
        // more event loop callbacks to be triggered with 'this->bev'.
        // This is important because we delete event_loop_handle which
        // is the callback argument for any event loop callbacks.
        // This lambda is responsible for ensuring 'this->bev' is
        // disabled, and cleaning up any remaining state associated
        // with the event loop.

        CHECK(__in_event_loop__);

        if (_listener != NULL) {
          evconnlistener_free(_listener);
        }

        if (_bev != NULL) {
          SSL* ssl = bufferevent_openssl_get_ssl(_bev);
          // Workaround for SSL shutdown, see http://www.wangafu.net/~nickm/libevent-book/Ref6a_advanced_bufferevents.html // NOLINT
          SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
          SSL_shutdown(ssl);

          // NOTE: Removes all future callbacks using 'this->bev'.
          bufferevent_disable(_bev, EV_READ | EV_WRITE);

          // Clean up the ssl object.
          SSL_free(ssl);

          // Clean up the buffer event. Since we don't set
          // 'BEV_OPT_CLOSE_ON_FREE' we rely on the base class
          // 'Socket::Impl' to clean up the fd.
          bufferevent_free(_bev);
        }

        if (ssl_socket_fd >= 0) {
          Try<Nothing> close = os::close(ssl_socket_fd);
          if (close.isError()) {
            LOG(WARNING) << "Failed to close socket "
                         << stringify(ssl_socket_fd) << ": " << close.error();
          }
        }

        delete _event_loop_handle;
      },
      DISALLOW_SHORT_CIRCUIT);
}


void LibeventSSLSocketImpl::initialize()
{
  event_loop_handle = new std::weak_ptr<LibeventSSLSocketImpl>(shared(this));
}


Try<Nothing> LibeventSSLSocketImpl::shutdown()
{
  // Nothing to do if this socket was never initialized.
  synchronized (lock) {
    if (bev == NULL) {
      // If it was not initialized, then there should also be no
      // requests.
      CHECK(connect_request.get() == NULL);
      CHECK(recv_request.get() == NULL);
      CHECK(send_request.get() == NULL);

      errno = ENOTCONN;
      return ErrnoError();
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

        CHECK_NOTNULL(self->bev);

        synchronized (self->bev) {
          Owned<RecvRequest> request;

          // Swap the 'recv_request' under the object lock.
          synchronized (self->lock) {
            std::swap(request, self->recv_request);
          }

          // If there is still a pending receive request then close it.
          if (request.get() != NULL) {
            request->promise
              .set(bufferevent_read(self->bev, request->data, request->size));
          }
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
  if (impl != NULL) {
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
  //    a non-NULL due to step f.
  if (buffer_length > 0) {
    synchronized (lock) {
      std::swap(request, recv_request);
    }
  }

  if (request.get() != NULL) {
    // There is an invariant that if we are executing a
    // 'recv_callback' and we have a request there must be data here
    // because we should not be getting a spurrious receive callback
    // invocation. Even if we discarded a request, the manual
    // invocation of 'recv_callback' guarantees that there is a
    // non-zero amount of data available in the bufferevent.
    size_t length = bufferevent_read(bev, request->data, request->size);
    CHECK(length > 0);

    request->promise.set(length);
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
  if (impl != NULL) {
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

  if (request.get() != NULL) {
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
  if (impl != NULL) {
    impl->event_callback(events);
  }
}


// Only runs in event loop. Member function continuation of static
// 'recv_callback'.
void LibeventSSLSocketImpl::event_callback(short events)
{
  CHECK(__in_event_loop__);

  Owned<RecvRequest> current_recv_request;
  Owned<SendRequest> current_send_request;
  Owned<ConnectRequest> current_connect_request;

  // In all of the following conditions, we're interested in swapping
  // the value of the requests with null (if they are already null,
  // then there's no harm).
  if (events & BEV_EVENT_EOF ||
      events & BEV_EVENT_CONNECTED ||
      events & BEV_EVENT_ERROR) {
    synchronized (lock) {
      std::swap(current_recv_request, recv_request);
      std::swap(current_send_request, send_request);
      std::swap(current_connect_request, connect_request);
    }
  }

  // If a request below is null, then no such request is in progress,
  // either because it was never created, it has already been
  // completed, or it has been discarded.

  if (events & BEV_EVENT_EOF ||
     (events & BEV_EVENT_ERROR && EVUTIL_SOCKET_ERROR() == 0)) {
    // At end of file, close the connection.
    if (current_recv_request.get() != NULL) {
      current_recv_request->promise.set(0);
    }

    if (current_send_request.get() != NULL) {
      current_send_request->promise.set(0);
    }

    if (current_connect_request.get() != NULL) {
      SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(bev));
      SSL_free(ssl);
      bufferevent_free(CHECK_NOTNULL(bev));
      bev = NULL;
      current_connect_request->promise.fail(
          "Failed connect: connection closed");
    }
  } else if (events & BEV_EVENT_CONNECTED) {
    // We should not have receiving or sending request while still
    // connecting.
    CHECK(current_recv_request.get() == NULL);
    CHECK(current_send_request.get() == NULL);
    CHECK_NOTNULL(current_connect_request.get());

    // If we're connecting, then we've succeeded. Time to do
    // post-verification.
    CHECK_NOTNULL(bev);

    // Do post-validation of connection.
    SSL* ssl = bufferevent_openssl_get_ssl(bev);

    Try<Nothing> verify = openssl::verify(ssl, peer_hostname);
    if (verify.isError()) {
      VLOG(1) << "Failed connect, verification error: " << verify.error();
      SSL_free(ssl);
      bufferevent_free(bev);
      bev = NULL;
      current_connect_request->promise.fail(verify.error());
      return;
    }

    current_connect_request->promise.set(Nothing());
  } else if (events & BEV_EVENT_ERROR) {
    CHECK(EVUTIL_SOCKET_ERROR() != 0);
    std::ostringstream error_stream;
    error_stream << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());

    // If there is a valid error, fail any requests and log the error.
    VLOG(1) << "Socket error: " << error_stream.str();

    if (current_recv_request.get() != NULL) {
      current_recv_request->promise.fail(
          "Failed recv, connection error: " +
          error_stream.str());
    }

    if (current_send_request.get() != NULL) {
      current_send_request->promise.fail(
          "Failed send, connection error: " +
          error_stream.str());
    }

    if (current_connect_request.get() != NULL) {
      SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(bev));
      SSL_free(ssl);
      bufferevent_free(CHECK_NOTNULL(bev));
      bev = NULL;
      current_connect_request->promise.fail(
          "Failed connect, connection error: " +
          error_stream.str());
    }
  }
}


LibeventSSLSocketImpl::LibeventSSLSocketImpl(int _s)
  : Socket::Impl(_s),
    bev(NULL),
    listener(NULL),
    lock(ATOMIC_FLAG_INIT),
    recv_request(NULL),
    send_request(NULL),
    connect_request(NULL),
    event_loop_handle(NULL),
    ssl_connect_fd(-1) {}


LibeventSSLSocketImpl::LibeventSSLSocketImpl(
    int _s,
    bufferevent* _bev,
    Option<std::string>&& _peer_hostname)
  : Socket::Impl(_s),
    bev(_bev),
    listener(NULL),
    lock(ATOMIC_FLAG_INIT),
    recv_request(NULL),
    send_request(NULL),
    connect_request(NULL),
    event_loop_handle(NULL),
    peer_hostname(std::move(_peer_hostname)),
    ssl_connect_fd(-1) {}


Future<Nothing> LibeventSSLSocketImpl::connect(const Address& address)
{
  if (bev != NULL) {
    return Failure("Socket is already connected");
  }

  if (connect_request.get() != NULL) {
    return Failure("Socket is already connecting");
  }

  SSL* ssl = SSL_new(openssl::context());
  if (ssl == NULL) {
    return Failure("Failed to connect: SSL_new");
  }

  ssl_connect_fd = ::dup(get());
  if (ssl_connect_fd < 0) {
    return Failure("Failed to 'dup' socket for new openssl socket handle");
  }

  // Reapply FD_CLOEXEC on the duplicate file descriptor.
  Try<Nothing> closeexec = os::cloexec(ssl_connect_fd);
  if (closeexec.isError()) {
    return Failure(
        "Failed to set FD_CLOEXEC flag for the dup'ed openssl socket handle: " +
        closeexec.error());
  }

  // Construct the bufferevent in the connecting state.
  // We set 'BEV_OPT_DEFER_CALLBACKS' to avoid calling the
  // 'event_callback' before 'bufferevent_socket_connect' returns.
  CHECK(bev == NULL);
  bev = bufferevent_openssl_socket_new(
      base,
      ssl_connect_fd,
      ssl,
      BUFFEREVENT_SSL_CONNECTING,
      BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS);

  if (bev == NULL) {
    // We need to free 'ssl' here because the bev won't clean it up
    // for us.
    SSL_free(ssl);
    return Failure("Failed to connect: bufferevent_openssl_socket_new");
  }

  // Try and determine the 'peer_hostname' from the address we're
  // connecting to in order to properly verify the SSL connection later.
  const Try<string> hostname = address.hostname();

  if (hostname.isError()) {
    VLOG(2) << "Could not determine hostname of peer: " << hostname.error();
  } else {
    VLOG(2) << "Connecting to " << hostname.get();
    peer_hostname = hostname.get();
  }

  // Optimistically construct a 'ConnectRequest' and future.
  Owned<ConnectRequest> request(new ConnectRequest());
  Future<Nothing> future = request->promise.future();

  // Assign 'connect_request' under lock, fail on error.
  synchronized (lock) {
    if (connect_request.get() != NULL) {
      SSL_free(ssl);
      bufferevent_free(bev);
      bev = NULL;
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
        sockaddr_storage addr =
          net::createSockaddrStorage(address.ip, address.port);

          // Assign the callbacks for the bufferevent. We do this
          // before the 'bufferevent_socket_connect()' call to avoid
          // any race on the underlying buffer events becoming ready.
          bufferevent_setcb(
              self->bev,
              &LibeventSSLSocketImpl::recv_callback,
              &LibeventSSLSocketImpl::send_callback,
              &LibeventSSLSocketImpl::event_callback,
              CHECK_NOTNULL(self->event_loop_handle));

          if (bufferevent_socket_connect(
                  self->bev,
                  reinterpret_cast<sockaddr*>(&addr),
                  address.size()) < 0) {
            SSL* ssl = bufferevent_openssl_get_ssl(CHECK_NOTNULL(self->bev));
            SSL_free(ssl);
            bufferevent_free(self->bev);
            self->bev = NULL;

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

      if (self != NULL) {
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
              if (request.get() != NULL) {
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
    if (recv_request.get() != NULL) {
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
          recv = self->recv_request.get() != NULL;
        }

        // Only try to read existing data from the bufferevent if the
        // request has not already been discarded.
        if (recv) {
          synchronized (self->bev) {
            evbuffer* input = bufferevent_get_input(self->bev);
            size_t length = evbuffer_get_length(input);

            // If there is already data in the buffer, fulfill the
            // 'recv_request' by calling 'recv_callback()'. Otherwise
            // do nothing and wait for the 'recv_callback' to run when
            // we receive data over the network.
            if (length > 0) {
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
    if (send_request.get() != NULL) {
      return Failure("Socket is already sending");
    }
    std::swap(request, send_request);
  }

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self, data, size]() {
        CHECK(__in_event_loop__);
        CHECK(self);

        // We check that send_request is valid, because we do not
        // allow discards. This means there is no race between the
        // entry of 'send' and the execution of this lambda.
        synchronized (self->lock) {
          CHECK_NOTNULL(self->send_request.get());
        }

        bufferevent_write(self->bev, data, size);
      },
      DISALLOW_SHORT_CIRCUIT);

  return future;
}


Future<size_t> LibeventSSLSocketImpl::sendfile(
    int fd,
    off_t offset,
    size_t size)
{
  // Optimistically construct a 'SendRequest' and future.
  Owned<SendRequest> request(new SendRequest(size));
  Future<size_t> future = request->promise.future();

  // Assign 'send_request' under lock, fail on error.
  synchronized (lock) {
    if (send_request.get() != NULL) {
      return Failure("Socket is already sending");
    }
    std::swap(request, send_request);
  }

  // Extend the life-time of 'this' through the execution of the
  // lambda in the event loop. Note: The 'self' needs to be explicitly
  // captured because we're not using it in the body of the lambda. We
  // can use a 'shared_ptr' because run_in_event_loop is guaranteed to
  // execute.
  auto self = shared(this);

  run_in_event_loop(
      [self, fd, offset, size]() {
        CHECK(__in_event_loop__);
        CHECK(self);

        // We check that send_request is valid, because we do not
        // allow discards. This means there is no race between the
        // entry of 'sendfile' and the execution of this lambda.
        synchronized (self->lock) {
          CHECK_NOTNULL(self->send_request.get());
        }

        evbuffer_add_file(
            bufferevent_get_output(self->bev),
            fd,
            offset,
            size);
      },
      DISALLOW_SHORT_CIRCUIT);

  return future;
}


Try<Nothing> LibeventSSLSocketImpl::listen(int backlog)
{
  if (listener != NULL) {
    return Error("Socket is already listening");
  }

  CHECK(bev == NULL);

  listener = evconnlistener_new(
      base,
      [](evconnlistener* listener,
         int socket,
         sockaddr* addr,
         int addr_length,
         void* arg) {
        CHECK(__in_event_loop__);

        std::weak_ptr<LibeventSSLSocketImpl>* handle =
          reinterpret_cast<std::weak_ptr<LibeventSSLSocketImpl>*>(
              CHECK_NOTNULL(arg));

        std::shared_ptr<LibeventSSLSocketImpl> impl(handle->lock());

        if (impl != NULL) {
          Try<net::IP> ip = net::IP::create(*addr);
          if (ip.isError()) {
            VLOG(2) << "Could not convert sockaddr to net::IP: " << ip.error();
          }

          // We pass the 'listener' into the 'AcceptRequest' because
          // this function could be executed before 'this->listener'
          // is set.
          AcceptRequest* request =
            new AcceptRequest(
                  socket,
                  listener,
                  ip.isSome() ? Option<net::IP>(ip.get()) : None());

          impl->accept_callback(request);
        }
      },
      event_loop_handle,
      LEV_OPT_REUSEABLE,
      backlog,
      s);

  if (listener == NULL) {
    return Error("Failed to listen on socket");
  }

  // TODO(jmlvanre): attach an error callback.

  return Nothing();
}


Future<Socket> LibeventSSLSocketImpl::accept()
{
  // We explicitly specify the return type to avoid a type deduction
  // issue in some versions of clang. See MESOS-2943.
  return accept_queue.get()
    .then([](const Future<Socket>& future) -> Future<Socket> {
      return future;
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
  request->peek_event = NULL;

  if (ssl) {
    accept_SSL_callback(request);
  } else {
    // Downgrade to a non-SSL socket.
    Try<Socket> create = Socket::create(Socket::POLL, fd);
    if (create.isError()) {
      request->promise.fail(create.error());
    } else {
      request->promise.set(create.get());
    }

    delete request;
  }
}


void LibeventSSLSocketImpl::accept_callback(AcceptRequest* request)
{
  CHECK(__in_event_loop__);

  // Enqueue a potential socket that we will set up SSL state for and
  // verify.
  accept_queue.put(request->promise.future());

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
    event_add(request->peek_event, NULL);
  } else {
    accept_SSL_callback(request);
  }
}


void LibeventSSLSocketImpl::accept_SSL_callback(AcceptRequest* request)
{
  CHECK(__in_event_loop__);

  // Set up SSL object.
  SSL* ssl = SSL_new(openssl::context());
  if (ssl == NULL) {
    request->promise.fail("Accept failed, SSL_new");
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

  if (bev == NULL) {
    request->promise.fail("Accept failed: bufferevent_openssl_socket_new");
    SSL_free(ssl);
    delete request;
    return;
  }

  bufferevent_setcb(
      bev,
      NULL,
      NULL,
      [](bufferevent* bev, short events, void* arg) {
        // This handles error states or 'BEV_EVENT_CONNECTED' events
        // and satisfies the promise by constructing a new socket if
        // the connection was successfuly established.
        CHECK(__in_event_loop__);

        AcceptRequest* request =
          reinterpret_cast<AcceptRequest*>(CHECK_NOTNULL(arg));

        if (events & BEV_EVENT_EOF) {
          request->promise.fail("Failed accept: connection closed");
        } else if (events & BEV_EVENT_CONNECTED) {
          // We will receive a 'CONNECTED' state on an accepting socket
          // once the connection is established. Time to do
          // post-verification. First, we need to determine the peer
          // hostname.
          Option<string> peer_hostname = None();
          if (request->ip.isSome()) {
            Try<string> hostname = net::getHostname(request->ip.get());
            if (hostname.isError()) {
              VLOG(2) << "Could not determine hostname of peer: "
                      << hostname.error();
            } else {
              VLOG(2) << "Accepting from " << hostname.get();
              peer_hostname = hostname.get();
            }
          }

          SSL* ssl = bufferevent_openssl_get_ssl(bev);
          CHECK_NOTNULL(ssl);

          Try<Nothing> verify = openssl::verify(ssl, peer_hostname);
          if (verify.isError()) {
            VLOG(1) << "Failed accept, verification error: " << verify.error();
            request->promise.fail(verify.error());
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
            delete request;
            return;
          }

          auto impl = std::shared_ptr<LibeventSSLSocketImpl>(
              new LibeventSSLSocketImpl(
                  request->socket,
                  bev,
                  std::move(peer_hostname)));

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

          Socket socket = Socket::Impl::socket(std::move(impl));

          request->promise.set(socket);
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
          VLOG(1) << "Socket error: " << stream.str();

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
              "Failed accept: connection error: " + stream.str());
        }

        delete request;
      },
      request);
}

} // namespace network {
} // namespace process {

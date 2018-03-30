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

#include <process/id.hpp>
#include <process/defer.hpp>

#include "encoder.hpp"
#include "http_proxy.hpp"
#include "socket_manager.hpp"

using process::http::InternalServerError;
using process::http::NotFound;

using process::network::inet::Socket;

using process::http::Response;
using process::http::Request;

using std::string;
using std::stringstream;

namespace process {

HttpProxy::HttpProxy(const Socket& _socket)
  : ProcessBase(ID::generate("__http__")),
    socket(_socket) {}


void HttpProxy::finalize()
{
  // Need to make sure response producers know not to continue to
  // create a response (streaming or otherwise).
  if (pipe.isSome()) {
    http::Pipe::Reader reader = pipe.get();
    reader.close();
  }
  pipe = None();

  while (!items.empty()) {
    Item* item = items.front();

    // Attempt to discard the future.
    item->future.discard();

    // But it might have already been ready. In general, we need to
    // wait until this future is potentially ready in order to attempt
    // to close a pipe if one exists.
    item->future.onReady([](const Response& response) {
      // Cleaning up a response (i.e., closing any open Pipes in the
      // event Response::type is PIPE).
      if (response.type == Response::PIPE) {
        CHECK_SOME(response.reader);
        http::Pipe::Reader reader = response.reader.get(); // Remove const.
        reader.close();
      }
    });

    items.pop();
    delete item;
  }

  // Just in case this process gets killed outside of `SocketManager::close`,
  // remove the proxy from the socket.
  socket_manager->unproxy(socket);
}


void HttpProxy::enqueue(const Response& response, const Request& request)
{
  handle(Future<Response>(response), request);
}


void HttpProxy::handle(const Future<Response>& future, const Request& request)
{
  items.push(new Item(request, future));

  if (items.size() == 1) {
    next();
  }
}


void HttpProxy::next()
{
  if (items.size() > 0) {
    // Wait for any transition of the future.
    items.front()->future.onAny(
        defer(self(), &HttpProxy::waited, lambda::_1));
  }
}


void HttpProxy::waited(const Future<Response>& future)
{
  CHECK(items.size() > 0);
  Item* item = items.front();

  CHECK(future == item->future);

  // Process the item and determine if we're done or not (so we know
  // whether to start waiting on the next responses).
  bool processed = process(item->future, item->request);

  items.pop();
  delete item;

  if (processed) {
    next();
  }
}


bool HttpProxy::process(const Future<Response>& future, const Request& request)
{
  if (!future.isReady()) {
    // TODO(benh): Consider handling other "states" of future
    // (discarded, failed, etc) with different HTTP statuses.
    Response response = future.isFailed()
      ? InternalServerError(future.failure())
      : InternalServerError("discarded future");

    VLOG(1) << "Returning '" << response.status << "'"
            << " for '" << request.url.path << "'"
            << " ("
            << (future.isFailed()
                  ? future.failure()
                  : "discarded") << ")";

    socket_manager->send(response, request, socket);

    return true; // All done, can process next response.
  }

  Response response = future.get();

  // If the response specifies a path, try and perform a sendfile.
  if (response.type == Response::PATH) {
    // Make sure no body is sent (this is really an error and
    // should be reported and no response sent.
    response.body.clear();

    const string& path = response.path;
    Try<int_fd> fd = os::open(path, O_RDONLY);
    if (fd.isError()) {
#ifdef __WINDOWS__
      const int error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
#else
      const int error = errno;
      if (error == ENOENT || error == ENOTDIR) {
#endif // __WINDOWS__
          VLOG(1) << "Returning '404 Not Found' for path '" << path << "'";
          socket_manager->send(NotFound(), request, socket);
      } else {
        VLOG(1) << "Failed to send file at '" << path << "': " << fd.error();
        socket_manager->send(InternalServerError(), request, socket);
      }
    } else {
      const Try<Bytes> size = os::stat::size(fd.get());
      if (size.isError()) {
        VLOG(1) << "Failed to send file at '" << path << "': " << size.error();
        socket_manager->send(InternalServerError(), request, socket);
      } else if (os::stat::isdir(fd.get())) {
        VLOG(1) << "Returning '404 Not Found' for directory '" << path << "'";
        socket_manager->send(NotFound(), request, socket);
      } else {
        // While the user is expected to properly set a 'Content-Type'
        // header, we fill in (or overwrite) 'Content-Length' header.
        response.headers["Content-Length"] = stringify(size->bytes());

        if (size.get() == 0) {
          socket_manager->send(response, request, socket);
          return true; // All done, can process next request.
        }

        VLOG(1) << "Sending file at '" << path << "' with length "
                << size.get();

        // TODO(benh): Consider a way to have the socket manager turn
        // on TCP_CORK for both sends and then turn it off.
        socket_manager->send(
            new HttpResponseEncoder(response, request),
            true,
            socket);

        // Note the file descriptor gets closed by FileEncoder.
        socket_manager->send(
            new FileEncoder(fd.get(), size->bytes()),
            request.keepAlive,
            socket);
      }
    }
  } else if (response.type == Response::PIPE) {
    // Make sure no body is sent (this is really an error and
    // should be reported and no response sent.
    response.body.clear();

    // While the user is expected to properly set a 'Content-Type'
    // header, we fill in (or overwrite) 'Transfer-Encoding' header.
    response.headers["Transfer-Encoding"] = "chunked";

    VLOG(3) << "Starting \"chunked\" streaming";

    socket_manager->send(
        new HttpResponseEncoder(response, request),
        true,
        socket);

    CHECK_SOME(response.reader);
    http::Pipe::Reader reader = response.reader.get();

    pipe = reader;

    // Avoid copying the request for each chunk read on the pipe.
    //
    // TODO(bmahler): Make request a process::Owned or
    // process::Shared from the point where it is decoded.
    Owned<Request> request_(new Request(request));

    reader.read()
      .onAny(defer(self(), &Self::stream, request_, lambda::_1));

    return false; // Streaming, don't process next response (yet)!
  } else {
    socket_manager->send(response, request, socket);
  }

  return true; // All done, can process next response.
}


void HttpProxy::stream(
    const Owned<Request>& request,
    const Future<string>& chunk)
{
  CHECK_SOME(pipe);
  CHECK_NOTNULL(request.get());

  http::Pipe::Reader reader = pipe.get();

  bool finished = false; // Whether we're done streaming.

  if (chunk.isReady()) {
    std::ostringstream out;

    if (chunk->empty()) {
      // Finished reading.
      out << "0\r\n" << "\r\n";
      finished = true;
    } else {
      out << std::hex << chunk->size() << "\r\n";
      out << chunk.get();
      out << "\r\n";

      // Keep reading.
      reader.read()
        .onAny(defer(self(), &Self::stream, request, lambda::_1));
    }

    // Always persist the connection when streaming is not finished.
    socket_manager->send(
        new DataEncoder(out.str()),
        finished ? request->keepAlive : true,
        socket);
  } else if (chunk.isFailed()) {
    VLOG(1) << "Failed to read from stream: " << chunk.failure();
    // TODO(bmahler): Have to close connection if headers were sent!
    socket_manager->send(InternalServerError(), *request, socket);
    finished = true;
  } else {
    VLOG(1) << "Failed to read from stream: discarded";
    // TODO(bmahler): Have to close connection if headers were sent!
    socket_manager->send(InternalServerError(), *request, socket);
    finished = true;
  }

  if (finished) {
    reader.close();
    pipe = None();
    next();
  }
}

} // namespace process {

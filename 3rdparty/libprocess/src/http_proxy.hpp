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

#ifndef __PROCESS_HTTP_PROXY_HPP__
#define __PROCESS_HTTP_PROXY_HPP__

#include <queue>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/socket.hpp>

#include <stout/option.hpp>

namespace process {

// Provides a process that manages sending HTTP responses so as to
// satisfy HTTP/1.1 pipelining. Each request should either enqueue a
// response, or ask the proxy to handle a future response. The process
// is responsible for making sure the responses are sent in the same
// order as the requests. Note that we use a 'Socket' in order to keep
// the underlying file descriptor from getting closed while there
// might still be outstanding responses even though the client might
// have closed the connection (see more discussion in
// SocketManager::close and SocketManager::proxy).
class HttpProxy : public Process<HttpProxy>
{
public:
  explicit HttpProxy(const network::inet::Socket& _socket);

  ~HttpProxy() override {}

  // Enqueues the response to be sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void enqueue(
      const http::Response& response,
      const http::Request& request);

  // Enqueues a future to a response that will get waited on (up to
  // some timeout) and then sent once all previously enqueued
  // responses have been processed (e.g., waited for and sent).
  void handle(
      const Future<http::Response>& future,
      const http::Request& request);

protected:
  void finalize() override;

private:
  // Starts "waiting" on the next available future response.
  void next();

  // Invoked once a future response has been satisfied.
  void waited(const Future<http::Response>& future);

  // Demuxes and handles a response.
  bool process(
      const Future<http::Response>& future,
      const http::Request& request);

  // Handles stream based responses.
  void stream(
      const Owned<http::Request>& request,
      const Future<std::string>& chunk);

  network::inet::Socket socket; // Store the socket to keep it open.

  // Describes a queue "item" that wraps the future to the response
  // and the original request.
  // The original request contains needed information such as what encodings
  // are acceptable and whether to persist the connection.
  struct Item
  {
    Item(const http::Request& _request, const Future<http::Response>& _future)
      : request(_request), future(_future) {}

    const http::Request request; // Make a copy.
    Future<http::Response> future; // Make a copy.
  };

  std::queue<Item*> items;

  Option<http::Pipe::Reader> pipe; // Current pipe, if streaming.
};

} // namespace process {

#endif // __PROCESS_HTTP_PROXY_HPP__

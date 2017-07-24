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

#ifndef __PROCESS_GRPC_HPP__
#define __PROCESS_GRPC_HPP__

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>

#include <google/protobuf/message.h>

#include <grpc++/grpc++.h>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/synchronized.hpp>
#include <stout/try.hpp>


// This file provides libprocess "support" for using gRPC. In
// particular, it defines two wrapper classes: `Channel` (representing a
// connection to a gRPC server) and `client::Runtime`, which integrates
// an event loop waiting for gRPC responses, and provides the `call`
// interface to create an asynchrous gRPC call and return a `Future`.


#define GRPC_RPC(service, rpc) \
  (&service::Stub::Async##rpc)

namespace process {
namespace grpc {

// Forward declarations.
namespace client { class Runtime; }


/**
 * A copyable interface to manage a connection to a gRPC server.
 * All `Channel` copies share the same connection. Note that the
 * connection is established lazily by the gRPC runtime library: the
 * actual connection is delayed till an RPC call is made.
 */
class Channel
{
public:
  Channel(const std::string& uri,
          const std::shared_ptr<::grpc::ChannelCredentials>& credentials =
            ::grpc::InsecureChannelCredentials())
    : channel(::grpc::CreateChannel(uri, credentials)) {}

private:
  std::shared_ptr<::grpc::Channel> channel;

  friend class client::Runtime;
};


namespace client {

/**
 * A copyable interface to manage an internal gRPC runtime instance for
 * asynchronous gRPC calls. A gRPC runtime instance includes a gRPC
 * `CompletionQueue` to manage outstanding requests, a looper thread to
 * wait for any incoming responses from the `CompletionQueue`, and a
 * process to handle the responses. All `Runtime` copies share the same
 * gRPC runtime instance. Usually we only need a single gRPC runtime
 * instance to handle all gRPC calls, but multiple instances can be
 * instantiated for more parallelism or isolation.
 * NOTE: The destruction of the internal gRPC runtime instance is a
 * blocking operation: it waits for the managed process to terminate.
 * The user should ensure that this only happens at shutdown.
 */
class Runtime
{
public:
  Runtime() : data(new Data()) {}

  /**
   * Sends an asynchronous gRPC call.
   *
   * @param channel A connection to a gRPC server.
   * @param rpc The asynchronous gRPC call to make. This can be obtained
   *     by the `GRPC_RPC(Service, RPC)` macro.
   * @param request The request protobuf for the gRPC call.
   * @return a `Future` waiting for a response protobuf.
   */
  template <typename Stub, typename Request, typename Response>
  Future<Response> call(
      const Channel& channel,
      std::unique_ptr<::grpc::ClientAsyncResponseReader<Response>>(Stub::*rpc)(
          ::grpc::ClientContext*,
          const Request&,
          ::grpc::CompletionQueue*),
      const Request& request)
  {
    static_assert(
        std::is_convertible<Request*, google::protobuf::Message*>::value,
        "Request must be a protobuf message");

    synchronized (data->lock) {
      if (data->terminating) {
        return Failure("Runtime has been terminated.");
      }

      std::shared_ptr<::grpc::ClientContext> context(
          new ::grpc::ClientContext());

      // TODO(chhsiao): Allow the caller to specify a timeout.
      context->set_deadline(
          std::chrono::system_clock::now() + std::chrono::seconds(5));

      // Create a `Promise` and a callback lambda as a tag and invokes
      // an asynchronous gRPC call through the `CompletionQueue`
      // managed by `data`. The `Promise` will be set by the callback
      // upon server response.
      std::shared_ptr<Promise<Response>> promise(new Promise<Response>);
      promise->future().onDiscard([=] { context->TryCancel(); });

      std::shared_ptr<Response> response(new Response());
      std::shared_ptr<::grpc::Status> status(new ::grpc::Status());

      std::shared_ptr<::grpc::ClientAsyncResponseReader<Response>> reader(
          (Stub(channel.channel).*rpc)(context.get(), request, &data->queue));

      reader->Finish(
          response.get(),
          status.get(),
          new lambda::function<void()>(
              // NOTE: `context` and `reader` need to be held on in
              // order to get updates for the ongoing RPC, and thus
              // are captured here. The lambda itself will later be
              // retrieved and managed in `Data::loop()`.
              [context, reader, response, status, promise]() {
                CHECK(promise->future().isPending());
                if (promise->future().hasDiscard()) {
                  promise->discard();
                } else if (status->ok()) {
                  promise->set(*response);
                } else {
                  promise->fail(status->error_message());
                }
              }));

      return promise->future();
    }
  }

  /**
   * Asks the internal gRPC runtime instance to shut down the
   * `CompletionQueue`, which would stop its looper thread, drain and
   * fail all pending gRPC calls in the `CompletionQueue`, then
   * asynchronously join the looper thread.
   */
  void terminate();

  /**
   * @return A `Future` waiting for all pending gRPC calls in the
   *     `CompletionQueue` of the internal gRPC runtime instance to be
   *     drained and the looper thread to be joined.
   */
  Future<Nothing> wait();

private:
  struct Data
  {
    Data();
    ~Data();

    void loop();
    void terminate();

    std::unique_ptr<std::thread> looper;
    ::grpc::CompletionQueue queue;
    ProcessBase process;
    std::atomic_flag lock;
    bool terminating;
    Promise<Nothing> terminated;
  };

  std::shared_ptr<Data> data;
};

} // namespace client {

} // namespace grpc {
} // namespace process {

#endif // __PROCESS_GRPC_HPP__

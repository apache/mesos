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
#include <utility>

#include <google/protobuf/message.h>

#include <grpcpp/grpcpp.h>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/synchronized.hpp>
#include <stout/try.hpp>


// This file provides libprocess "support" for using gRPC. In particular, it
// defines two wrapper classes: `client::Connection` which represents a
// connection to a gRPC server, and `client::Runtime` which integrates an event
// loop waiting for gRPC responses and provides the `call` interface to create
// an asynchronous gRPC call and return a `Future`.


#define GRPC_CLIENT_METHOD(service, rpc) \
  (&service::Stub::PrepareAsync##rpc)

namespace process {
namespace grpc {

/**
 * Represents errors caused by non-OK gRPC statuses. See:
 * https://grpc.io/grpc/cpp/classgrpc_1_1_status.html
 */
class StatusError : public Error
{
public:
  StatusError(::grpc::Status _status)
    : Error(_status.error_message()), status(std::move(_status))
  {
    CHECK(!status.ok());
  }

  const ::grpc::Status status;
};


namespace client {

// Internal helper utilities.
namespace internal {

template <typename T>
struct MethodTraits; // Undefined.


template <typename Stub, typename Request, typename Response>
struct MethodTraits<
    std::unique_ptr<::grpc::ClientAsyncResponseReader<Response>>(Stub::*)(
        ::grpc::ClientContext*,
        const Request&,
        ::grpc::CompletionQueue*)>
{
  typedef Stub stub_type;
  typedef Request request_type;
  typedef Response response_type;
};

} // namespace internal {


/**
 * A copyable interface to manage a connection to a gRPC server. All
 * `Connection` copies share the same gRPC channel which is thread safe. Note
 * that the actual connection is established lazily by the gRPC library at the
 * time an RPC is made to the channel.
 */
class Connection
{
public:
  Connection(
      const std::string& uri,
      const std::shared_ptr<::grpc::ChannelCredentials>& credentials =
        ::grpc::InsecureChannelCredentials())
    : channel(::grpc::CreateChannel(uri, credentials)) {}

  explicit Connection(std::shared_ptr<::grpc::Channel> _channel)
    : channel(std::move(_channel)) {}

  const std::shared_ptr<::grpc::Channel> channel;
};


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
   * This function returns a `Future` of a `Try` such that the response protobuf
   * is returned only if the gRPC call returns an OK status to ensure type
   * safety (see https://github.com/grpc/grpc/issues/12824). Note that the
   * future never fails; it will return a `StatusError` if a non-OK status is
   * returned for the call, so the caller can handle the error programmatically.
   *
   * @param connection A connection to a gRPC server.
   * @param rpc The asynchronous gRPC call to make. This can be obtained
   *     by the `GRPC_CLIENT_METHOD(service, rpc)` macro.
   * @param request The request protobuf for the gRPC call.
   * @return a `Future` of `Try` waiting for a response protobuf or an error.
   */
  template <
      typename Method,
      typename Request =
        typename internal::MethodTraits<Method>::request_type,
      typename Response =
        typename internal::MethodTraits<Method>::response_type>
  Future<Try<Response, StatusError>> call(
      const Connection& connection,
      Method&& method,
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

      // Enable the gRPC wait-for-ready semantics by default. See:
      // https://github.com/grpc/grpc/blob/master/doc/wait-for-ready.md
      // TODO(chhsiao): Allow the caller to set the option.
      context->set_wait_for_ready(true);

      // Create a `Promise` and a callback lambda as a tag and invokes
      // an asynchronous gRPC call through the `CompletionQueue`
      // managed by `data`. The `Promise` will be set by the callback
      // upon server response.
      std::shared_ptr<Promise<Try<Response, StatusError>>> promise(
          new Promise<Try<Response, StatusError>>);

      promise->future().onDiscard([=] { context->TryCancel(); });

      std::shared_ptr<Response> response(new Response());
      std::shared_ptr<::grpc::Status> status(new ::grpc::Status());

      std::shared_ptr<::grpc::ClientAsyncResponseReader<Response>> reader =
        (typename internal::MethodTraits<Method>::stub_type(
            connection.channel).*method)(context.get(), request, &data->queue);

      reader->StartCall();
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
                } else {
                  promise->set(status->ok()
                    ? std::move(*response)
                    : Try<Response, StatusError>::error(std::move(*status)));
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
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    bool terminating = false;
    Promise<Nothing> terminated;
  };

  std::shared_ptr<Data> data;
};

} // namespace client {

} // namespace grpc {
} // namespace process {

#endif // __PROCESS_GRPC_HPP__

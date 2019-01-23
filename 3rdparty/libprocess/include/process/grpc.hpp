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

#include <chrono>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <google/protobuf/message.h>

#include <grpcpp/grpcpp.h>

#include <process/check.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


// This file provides libprocess "support" for using gRPC. In particular, it
// defines two wrapper classes: `client::Connection` which represents a
// connection to a gRPC server, and `client::Runtime` which integrates an event
// loop waiting for gRPC responses and provides the `call` interface to create
// an asynchronous gRPC call and return a `Future`.


#define GRPC_CLIENT_METHOD(service, rpc) \
  (&service::Stub::PrepareAsync##rpc)

namespace grpc {

std::ostream& operator<<(std::ostream& stream, StatusCode statusCode);

} // namespace grpc {


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
    : Error(stringify(_status.error_code()) +
            (_status.error_message().empty()
               ? "" : ": " + _status.error_message())),
      status(std::move(_status))
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
 * Defines the gRPC options for each call.
 */
struct CallOptions
{
  // Enable the gRPC wait-for-ready semantics by default so the call will be
  // retried if the connection is not ready. See:
  // https://github.com/grpc/grpc/blob/master/doc/wait-for-ready.md
  bool wait_for_ready = true;

  // The timeout of the call. A `DEADLINE_EXCEEDED` status will be returned if
  // there is no response in the specified amount of time. This is required to
  // avoid the call from being pending forever.
  Duration timeout = Seconds(60);
};


/**
 * A copyable interface to manage an internal runtime process for asynchronous
 * gRPC calls. A runtime process keeps a gRPC `CompletionQueue` to manage
 * outstanding requests, a looper thread to wait for any incoming responses from
 * the `CompletionQueue`, and handles the requests and responses. All `Runtime`
 * copies share the same runtime process. Usually we only need a single runtime
 * process to handle all gRPC calls, but multiple runtime processes can be
 * instantiated for better parallelism and isolation.
 *
 * NOTE: The caller must call `terminate` to drain the `CompletionQueue` before
 * finalizing libprocess to gracefully terminate the gRPC runtime.
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
   * @param method The asynchronous gRPC call to make. This should be obtained
   *     by the `GRPC_CLIENT_METHOD(service, rpc)` macro.
   * @param request The request protobuf for the gRPC call.
   * @param options The gRPC options for the call.
   * @return a `Future` of `Try` waiting for a response protobuf or an error.
   */
  template <
      typename Method,
      typename Request =
        typename internal::MethodTraits<Method>::request_type,
      typename Response =
        typename internal::MethodTraits<Method>::response_type,
      typename std::enable_if<
          std::is_convertible<
              typename std::decay<Request>::type*,
              google::protobuf::Message*>::value,
          int>::type = 0>
  Future<Try<Response, StatusError>> call(
      const Connection& connection,
      Method&& method,
      Request&& request,
      const CallOptions& options)
  {
    // Create a `Promise` that will be set upon receiving a response.
    // TODO(chhsiao): The `Promise` in the `shared_ptr` is not shared, but only
    // to be captured by the lambda below. Use a `unique_ptr` once we get C++14.
    std::shared_ptr<Promise<Try<Response, StatusError>>> promise(
        new Promise<Try<Response, StatusError>>);
    Future<Try<Response, StatusError>> future = promise->future();

    // Send the request in the internal runtime process.
    // TODO(chhsiao): We use `std::bind` here to forward `request` to avoid an
    // extra copy. We should capture it by forwarding once we get C++14.
    dispatch(data->pid, &RuntimeProcess::send, std::bind(
        [connection, method, options, promise](
            const Request& request,
            bool terminating,
            ::grpc::CompletionQueue* queue) {
          if (terminating) {
            promise->fail("Runtime has been terminated");
            return;
          }

          // TODO(chhsiao): The `shared_ptr`s here aren't shared, but only to be
          // captured by the lambda below. Use `unique_ptr`s once we get C++14.
          std::shared_ptr<::grpc::ClientContext> context(
              new ::grpc::ClientContext());

          context->set_wait_for_ready(options.wait_for_ready);

          // We need to ensure that we're using a
          // `std::chrono::system_clock::time_point` because `grpc::TimePoint`
          // provides a specialization only for this type and we cannot
          // guarantee that the operation below will always result in this type.
          auto time_point =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                std::chrono::system_clock::now() +
                std::chrono::nanoseconds(options.timeout.ns()));

          context->set_deadline(time_point);

          promise->future().onDiscard([=] { context->TryCancel(); });

          std::shared_ptr<Response> response(new Response());
          std::shared_ptr<::grpc::Status> status(new ::grpc::Status());

          std::shared_ptr<::grpc::ClientAsyncResponseReader<Response>> reader =
            (typename internal::MethodTraits<Method>::stub_type(
                connection.channel).*method)(context.get(), request, queue);

          reader->StartCall();

          // Create a `ReceiveCallback` as a tag in the `CompletionQueue` for
          // the current asynchronous gRPC call. The callback will set up the
          // above `Promise` upon receiving a response.
          // NOTE: `context` and `reader` need to be held on in order to get
          // updates for the ongoing RPC, and thus are captured here. The
          // callback itself will later be retrieved and managed in the
          // looper thread.
          void* tag = new ReceiveCallback(
              [context, reader, response, status, promise]() {
                CHECK_PENDING(promise->future());
                if (promise->future().hasDiscard()) {
                  promise->discard();
                } else {
                  promise->set(status->ok()
                    ? std::move(*response)
                    : Try<Response, StatusError>::error(std::move(*status)));
                }
              });

          reader->Finish(response.get(), status.get(), tag);
        },
        std::forward<Request>(request),
        lambda::_1,
        lambda::_2));

    return future;
  }

  /**
   * Asks the internal runtime process to shut down the `CompletionQueue`, which
   * would asynchronously drain and fail all pending gRPC calls in the
   * `CompletionQueue`, then join the looper thread.
   */
  void terminate();

  /**
   * @return A `Future` waiting for all pending gRPC calls in the
   *     `CompletionQueue` of the internal runtime process to be drained and the
   *     looper thread to be joined.
   */
  Future<Nothing> wait();

private:
  // Type of the callback functions that can get invoked when sending a request
  // or receiving a response.
  typedef lambda::CallableOnce<
      void(bool, ::grpc::CompletionQueue*)> SendCallback;
  typedef lambda::CallableOnce<void()> ReceiveCallback;

  class RuntimeProcess : public Process<RuntimeProcess>
  {
  public:
    RuntimeProcess();
    ~RuntimeProcess() override;

    void send(SendCallback callback);
    void receive(ReceiveCallback callback);
    void terminate();
    Future<Nothing> wait();

  private:
    void initialize() override;
    void finalize() override;

    void loop();

    ::grpc::CompletionQueue queue;
    std::unique_ptr<std::thread> looper;
    bool terminating;
    Promise<Nothing> terminated;
  };

  struct Data
  {
     Data();
     ~Data();

     PID<RuntimeProcess> pid;
     Future<Nothing> terminated;
  };

  std::shared_ptr<Data> data;
};

} // namespace client {

} // namespace grpc {
} // namespace process {

#endif // __PROCESS_GRPC_HPP__

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __COMMON_HTTP_HPP__
#define __COMMON_HTTP_HPP__

#include <vector>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/quota/quota.hpp>

#include <process/authenticator.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/protobuf.hpp>
#include <stout/recordio.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "internal/evolve.hpp"

// TODO(benh): Remove this once we get C++14 as an enum should have a
// default hash.
namespace std {

template <>
struct hash<mesos::authorization::Action>
{
  typedef size_t result_type;

  typedef mesos::authorization::Action argument_type;

  result_type operator()(const argument_type& action) const
  {
    size_t seed = 0;
    boost::hash_combine(
        seed, static_cast<std::underlying_type<argument_type>::type>(action));
    return seed;
  }
};

} // namespace std {

namespace mesos {

class Attributes;
class Resources;
class Task;

namespace internal {

// Name of the default, basic authenticator.
constexpr char DEFAULT_BASIC_HTTP_AUTHENTICATOR[] = "basic";

// Name of the default, basic authenticatee.
constexpr char DEFAULT_BASIC_HTTP_AUTHENTICATEE[] = "basic";

// Name of the default, JWT authenticator.
constexpr char DEFAULT_JWT_HTTP_AUTHENTICATOR[] = "jwt";


// Contains the media types corresponding to some of the "Content-*",
// "Accept-*" and "Message-*" prefixed request headers in our internal
// representation.
struct RequestMediaTypes
{
  ContentType content; // 'Content-Type' header.
  ContentType accept; // 'Accept' header.
  Option<ContentType> messageContent; // 'Message-Content-Type' header.
  Option<ContentType> messageAccept; // 'Message-Accept' header.
};


// Serializes a protobuf message for transmission
// based on the HTTP content type.
// NOTE: For streaming `contentType`, `message` would not
// be serialized in "Record-IO" format.
std::string serialize(
    ContentType contentType,
    const google::protobuf::Message& message);


// Deserializes a string message into a protobuf message based on the
// HTTP content type.
template <typename Message>
Try<Message> deserialize(
    ContentType contentType,
    const std::string& body)
{
  switch (contentType) {
    case ContentType::PROTOBUF: {
      Message message;
      if (!message.ParseFromString(body)) {
        return Error("Failed to parse body into a protobuf object");
      }
      return message;
    }
    case ContentType::JSON: {
      Try<JSON::Value> value = JSON::parse(body);
      if (value.isError()) {
        return Error("Failed to parse body into JSON: " + value.error());
      }

      return ::protobuf::parse<Message>(value.get());
    }
    case ContentType::RECORDIO: {
      return Error("Deserializing a RecordIO stream is not supported");
    }
  }

  UNREACHABLE();
}


// Returns true if the media type can be used for
// streaming requests/responses.
bool streamingMediaType(ContentType contentType);


// Represents the streaming HTTP connection to a client, such as a framework,
// executor, or operator subscribed to the '/api/vX' endpoint.
// The `Event` template is the evolved message being sent to the client,
// e.g. `v1::scheduler::Event`, `v1::master::Event`, or `v1::executor::Event`.
template <typename Event>
struct StreamingHttpConnection
{
  StreamingHttpConnection(
      const process::http::Pipe::Writer& _writer,
      ContentType _contentType,
      id::UUID _streamId = id::UUID::random())
    : writer(_writer),
      contentType(_contentType),
      streamId(_streamId) {}

  template <typename Message>
  bool send(const Message& message)
  {
    // TODO(bmahler): Remove this evolve(). Could we still
    // somehow assert that evolve(message) produces a result
    // of type Event without calling evolve()?
    Event e = evolve(message);

    std::string record = serialize(contentType, e);

    return writer.write(::recordio::encode(record));
  }

  // Like the above send, but for already serialized data.
  bool send(const std::string& event)
  {
    return writer.write(::recordio::encode(event));
  }

  bool close()
  {
    return writer.close();
  }

  process::Future<Nothing> closed() const
  {
    return writer.readerClosed();
  }

  process::http::Pipe::Writer writer;
  ContentType contentType;
  id::UUID streamId;
};


// The representation of generic v0 protobuf => v1 protobuf as JSON,
// e.g., `jsonify(asV1Protobuf(message))`.
//
// Specifically, this acts the same as JSON::Protobuf, except that
// it remaps "slave" to "agent" in field names and enum values.
struct asV1Protobuf : Representation<google::protobuf::Message>
{
  using Representation<google::protobuf::Message>::Representation;
};

void json(JSON::ObjectWriter* writer, const asV1Protobuf& protobuf);


JSON::Object model(const Resources& resources);
JSON::Object model(const hashmap<std::string, Resources>& roleResources);
JSON::Object model(const Attributes& attributes);
JSON::Object model(const CommandInfo& command);
JSON::Object model(const ExecutorInfo& executorInfo);
JSON::Array model(const Labels& labels);
JSON::Object model(const Task& task);
JSON::Object model(const FileInfo& fileInfo);
JSON::Object model(const google::protobuf::Map<std::string, Value_Scalar>& map);

void json(JSON::ObjectWriter* writer, const Task& task);


// NOTE: The `metrics` object provided as an argument must outlive
// the returned function, since the function captures `metrics`
// by reference to avoid a really expensive map copy. This is a
// rather unsafe approach, but in typical jsonify usage this is
// not an issue.
//
// TODO(bmahler): Use std::enable_if with std::is_same to check
// that T is either the master or agent GetMetrics.
template <typename T>
std::function<void(JSON::ObjectWriter*)> jsonifyGetMetrics(
    const std::map<std::string, double>& metrics)
{
  // Serialize the following message:
  //
  //   mesos::master::Response::GetMetrics getMetrics;
  //   // or: mesos::agent::Response::GetMetrics getMetrics;
  //
  //   foreachpair (const string& key, double value, metrics) {
  //     Metric* metric = getMetrics->add_metrics();
  //     metric->set_name(key);
  //     metric->set_value(value);
  //   }

  return [&](JSON::ObjectWriter* writer) {
    const google::protobuf::Descriptor* descriptor = T::descriptor();

    int field = T::kMetricsFieldNumber;

    writer->field(
        descriptor->FindFieldByNumber(field)->name(),
        [&](JSON::ArrayWriter* writer) {
          foreachpair (const std::string& key, double value, metrics) {
            writer->element([&](JSON::ObjectWriter* writer) {
              const google::protobuf::Descriptor* descriptor =
                v1::Metric::descriptor();

              int field;

              field = v1::Metric::kNameFieldNumber;
              writer->field(
                  descriptor->FindFieldByNumber(field)->name(), key);

              field = v1::Metric::kValueFieldNumber;
              writer->field(
                  descriptor->FindFieldByNumber(field)->name(), value);
            });
          }
        });
  };
}


// TODO(bmahler): Use std::enable_if with std::is_same to check
// that T is either the master or agent GetMetrics.
template <typename T>
std::string serializeGetMetrics(
    const std::map<std::string, double>& metrics)
{
  // Serialize the following message:
  //
  //   v1::master::Response::GetMetrics getMetrics;
  //   // or: v1::agent::Response::GetMetrics getMetrics;
  //
  //   foreachpair (const string& key, double value, metrics) {
  //     Metric* metric = getMetrics->add_metrics();
  //     metric->set_name(key);
  //     metric->set_value(value);
  //   }

  auto serializeMetric = [](const std::string& key, double value) {
    std::string output;
    google::protobuf::io::StringOutputStream stream(&output);
    google::protobuf::io::CodedOutputStream writer(&stream);

    google::protobuf::internal::WireFormatLite::WriteString(
        v1::Metric::kNameFieldNumber, key, &writer);
    google::protobuf::internal::WireFormatLite::WriteDouble(
        v1::Metric::kValueFieldNumber, value, &writer);

    // While an explicit Trim() isn't necessary (since the coded
    // output stream is destructed before the string is returned),
    // it's a quite tricky bug to diagnose if Trim() is missed, so
    // we always do it explicitly to signal the reader about this
    // subtlety.
    writer.Trim();
    return output;
  };

  std::string output;
  google::protobuf::io::StringOutputStream stream(&output);
  google::protobuf::io::CodedOutputStream writer(&stream);

  foreachpair (const std::string& key, double value, metrics) {
    google::protobuf::internal::WireFormatLite::WriteBytes(
        T::kMetricsFieldNumber,
        serializeMetric(key, value),
        &writer);
  }

  // While an explicit Trim() isn't necessary (since the coded
  // output stream is destructed before the string is returned),
  // it's a quite tricky bug to diagnose if Trim() is missed, so
  // we always do it explicitly to signal the reader about this
  // subtlety.
  writer.Trim();
  return output;
}


} // namespace internal {

void json(JSON::ObjectWriter* writer, const Attributes& attributes);
void json(JSON::ObjectWriter* writer, const CommandInfo& command);
void json(JSON::ObjectWriter* writer, const DomainInfo& domainInfo);
void json(JSON::ObjectWriter* writer, const ExecutorInfo& executorInfo);
void json(
    JSON::StringWriter* writer, const FrameworkInfo::Capability& capability);
void json(JSON::ArrayWriter* writer, const Labels& labels);
void json(JSON::ObjectWriter* writer, const MasterInfo& info);
void json(
    JSON::StringWriter* writer, const MasterInfo::Capability& capability);
void json(JSON::ObjectWriter* writer, const Offer& offer);
void json(JSON::ObjectWriter* writer, const Resources& resources);
void json(
    JSON::ObjectWriter* writer,
    const google::protobuf::RepeatedPtrField<Resource>& resources);
void json(JSON::ObjectWriter* writer, const ResourceQuantities& quantities);
void json(JSON::ObjectWriter* writer, const ResourceLimits& limits);
void json(JSON::ObjectWriter* writer, const SlaveInfo& slaveInfo);
void json(
    JSON::StringWriter* writer, const SlaveInfo::Capability& capability);
void json(JSON::ObjectWriter* writer, const Task& task);
void json(JSON::ObjectWriter* writer, const TaskStatus& status);


// Implementation of the `ObjectApprover` interface authorizing all objects.
class AcceptingObjectApprover : public ObjectApprover
{
public:
  Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    return true;
  }
};


class ObjectApprovers
{
public:
  static process::Future<process::Owned<ObjectApprovers>> create(
      const Option<Authorizer*>& authorizer,
      const Option<process::http::authentication::Principal>& principal,
      std::initializer_list<authorization::Action> actions);

  Try<bool> approved(
      authorization::Action action,
      const ObjectApprover::Object& object) const
  {
    if (!approvers.contains(action)) {
      LOG(WARNING)
        << "Attempted to authorize principal "
        << " '" << (principal.isSome() ? stringify(*principal) : "") << "'"
        << " for unexpected action " << authorization::Action_Name(action);

      return false;
    }

    return approvers.at(action)->approved(object);
  }

  // Constructs one (or more) authorization objects, depending on the
  // action, and returns true if all action-object pairs are authorized.
  //
  // NOTE: This template has specializations that actually check
  // more than one action-object pair.
  template <authorization::Action action, typename... Args>
  bool approved(const Args&... args) const
  {
    const Try<bool> approval =
      approved(action, ObjectApprover::Object(args...));

    if (approval.isError()) {
      // NOTE: Silently dropping errors here creates a potential for
      // _transient_ authorization errors to make API events subscriber's view
      // inconsistent (see MESOS-10085). Also, this creates potential for an
      // object to silently disappear from Operator API endpoint response in
      // case of an authorization error (see MESOS-10099).
      //
      // TODO(joerg84): Expose these errors back to the caller.
      LOG(WARNING) << "Failed to authorize principal "
                   << " '" << (principal.isSome() ? stringify(*principal) : "")
                   << "' for action " << authorization::Action_Name(action)
                   << ": " << approval.error();

      return false;
    }

    return approval.get();
  }

  const Option<process::http::authentication::Principal> principal;

private:
  ObjectApprovers(
      hashmap<
          authorization::Action,
          std::shared_ptr<const ObjectApprover>>&& _approvers,
      const Option<process::http::authentication::Principal>& _principal)
    : principal(_principal),
      approvers(std::move(_approvers)) {}

  hashmap<authorization::Action, std::shared_ptr<const ObjectApprover>>
    approvers;
};


template <>
inline bool ObjectApprovers::approved<authorization::VIEW_ROLE>(
    const Resource& resource) const
{
  // Necessary because recovered agents are presented in old format.
  if (resource.has_role() && resource.role() != "*" &&
      !approved<authorization::VIEW_ROLE>(resource.role())) {
    return false;
  }

  // Reservations follow a path model where each entry is a child of the
  // previous one. Therefore, to accept the resource the acceptor has to
  // accept all entries.
  foreach (Resource::ReservationInfo reservation, resource.reservations()) {
    if (!approved<authorization::VIEW_ROLE>(reservation.role())) {
      return false;
    }
  }

  if (resource.has_allocation_info() &&
      !approved<authorization::VIEW_ROLE>(
          resource.allocation_info().role())) {
    return false;
  }

  return true;
}


/**
 * Used to filter results for API handlers. Provides the 'accept()' method to
 * test whether the supplied ID is equal to a stored target ID. If no target
 * ID is provided when the acceptor is constructed, it will accept all inputs.
 */
template <typename T>
class IDAcceptor
{
public:
  IDAcceptor(const Option<std::string>& id = None())
  {
    if (id.isSome()) {
      T targetId_;
      targetId_.set_value(id.get());
      targetId = targetId_;
    }
  }

  bool accept(const T& candidateId) const
  {
    if (targetId.isNone()) {
      return true;
    }

    return candidateId.value() == targetId->value();
  }

protected:
  Option<T> targetId;
};


// Authorizes access to an HTTP endpoint. The `method` parameter
// determines which ACL action will be used in the authorization.
// It is expected that the caller has validated that `method` is
// supported by this function. Currently "GET" is supported.
//
// TODO(nfnt): Prefer types instead of strings
// for `endpoint` and `method`, see MESOS-5300.
process::Future<bool> authorizeEndpoint(
    const std::string& endpoint,
    const std::string& method,
    const Option<Authorizer*>& authorizer,
    const Option<process::http::authentication::Principal>& principal);


/**
 * Helper function to create HTTP authenticators
 * for a given realm and register in libprocess.
 *
 * @param realm name of the realm.
 * @param authenticatorNames a vector of authenticator names.
 * @param credentials optional credentials for BasicAuthenticator only.
 * @param jwtSecretKey optional secret key for the JWTAuthenticator only.
 * @return nothing if authenticators are initialized and registered to
 *         libprocess successfully, or error if authenticators cannot
 *         be initialized.
 */
Try<Nothing> initializeHttpAuthenticators(
    const std::string& realm,
    const std::vector<std::string>& httpAuthenticatorNames,
    const Option<Credentials>& credentials = None(),
    const Option<std::string>& jwtSecretKey = None());


// Logs the request. Route handlers can compose this with the
// desired request handler to get consistent request logging.
void logRequest(const process::http::Request& request);


// Log the response for the corresponding request together with the request
// processing time. Route handlers can compose this with the desired request
// handler to get consistent request/response logging.
//
// TODO(alexr): Consider taking `response` as a future to allow logging for
// cases when response has not been generated.
void logResponse(
    const process::http::Request& request,
    const process::http::Response& response);

} // namespace mesos {

#endif // __COMMON_HTTP_HPP__

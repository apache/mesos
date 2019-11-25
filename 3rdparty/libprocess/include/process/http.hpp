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

#ifndef __PROCESS_HTTP_HPP__
#define __PROCESS_HTTP_HPP__

#include <ctype.h>
#include <stdint.h>

#include <atomic>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <boost/functional/hash.hpp>

#include <process/address.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/socket.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

namespace process {

// Forward declaration to break cyclic dependency.
template <typename T>
class Future;

namespace http {

enum class Scheme {
  HTTP,
  HTTP_UNIX,
#ifdef USE_SSL_SOCKET
  HTTPS,
#endif
};


namespace authentication {

class Authenticator;

struct Principal;

/**
 * Sets (or overwrites) the authenticator for the realm.
 *
 * Every incoming HTTP request to an endpoint associated
 * with the realm will be authenticated with the given
 * authenticator.
 */
Future<Nothing> setAuthenticator(
    const std::string& realm,
    Owned<Authenticator> authenticator);


/**
 * Unsets the authenticator for the realm.
 *
 * Any endpoint mapped to the realm will no
 * longer be authenticated.
 */
Future<Nothing> unsetAuthenticator(const std::string& realm);

} // namespace authentication {

// Forward declaration.
struct Request;

namespace authorization {

// The `AuthorizationCallbacks` type is used for a set of authorization
// callbacks used by libprocess to authorize HTTP endpoints. The key of the map
// contains the endpoint's path, while the value contains the callback.
typedef hashmap<std::string,
                lambda::function<process::Future<bool>(
                    const Request,
                    const Option<authentication::Principal>)>>
  AuthorizationCallbacks;


// Set authorization callbacks for HTTP endpoints. These can be used to call out
// to an external, application-level authorizer. The callbacks should accept an
// HTTP request and an optional principal, and they should return a
// `Future<bool>` representing whether or not authorization was successful.
void setCallbacks(const AuthorizationCallbacks&);


// Remove any authorization callbacks which were previously installed in
// libprocess.
void unsetCallbacks();

} // namespace authorization {

// Checks if the given status code is defined by RFC 2616.
bool isValidStatus(uint16_t code);

// Represents a Uniform Resource Locator:
//   scheme://domain|ip:port/path?query#fragment
//
// This is actually a URI-reference (see 4.1 of RFC 3986).
//
// TODO(bmahler): The default port should depend on the scheme!
struct URL
{
  URL() = default;

  URL(const std::string& _scheme,
      const std::string& _domain,
      const std::string& _path,
      const hashmap<std::string, std::string>& _query =
        (hashmap<std::string, std::string>()),
      const Option<std::string>& _fragment = None())
    : scheme(_scheme),
      domain(_domain),
      path(_path),
      query(_query),
      fragment(_fragment) {}

  URL(const std::string& _scheme,
      const std::string& _domain,
      const uint16_t _port = 80,
      const std::string& _path = "/",
      const hashmap<std::string, std::string>& _query =
        (hashmap<std::string, std::string>()),
      const Option<std::string>& _fragment = None())
    : scheme(_scheme),
      domain(_domain),
      port(_port),
      path(_path),
      query(_query),
      fragment(_fragment) {}

  URL(const std::string& _scheme,
      const net::IP& _ip,
      const uint16_t _port = 80,
      const std::string& _path = "/",
      const hashmap<std::string, std::string>& _query =
        (hashmap<std::string, std::string>()),
      const Option<std::string>& _fragment = None())
    : scheme(_scheme),
      ip(_ip),
      port(_port),
      path(_path),
      query(_query),
      fragment(_fragment) {}

  static Try<URL> parse(const std::string& urlString);

  /**
   * Returns whether the URL is absolute.
   * See https://tools.ietf.org/html/rfc3986#section-4.3 for details.
   */
  bool isAbsolute() const;

  Option<std::string> scheme;

  // TODO(benh): Consider using unrestricted union for 'domain' and 'ip'.
  Option<std::string> domain;
  Option<net::IP> ip;
  Option<uint16_t> port;
  std::string path;
  hashmap<std::string, std::string> query;
  Option<std::string> fragment;
};


std::ostream& operator<<(std::ostream& stream, const URL& url);


struct CaseInsensitiveHash
{
  size_t operator()(const std::string& key) const
  {
    size_t seed = 0;
    foreach (char c, key) {
      boost::hash_combine(seed, ::tolower(c));
    }
    return seed;
  }
};


struct CaseInsensitiveEqual
{
  bool operator()(const std::string& left, const std::string& right) const
  {
    if (left.size() != right.size()) {
      return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
      if (::tolower(left[i]) != ::tolower(right[i])) {
        return false;
      }
    }
    return true;
  }
};


struct Status
{
  static const uint16_t CONTINUE;
  static const uint16_t SWITCHING_PROTOCOLS;
  static const uint16_t OK;
  static const uint16_t CREATED;
  static const uint16_t ACCEPTED;
  static const uint16_t NON_AUTHORITATIVE_INFORMATION;
  static const uint16_t NO_CONTENT;
  static const uint16_t RESET_CONTENT;
  static const uint16_t PARTIAL_CONTENT;
  static const uint16_t MULTIPLE_CHOICES;
  static const uint16_t MOVED_PERMANENTLY;
  static const uint16_t FOUND;
  static const uint16_t SEE_OTHER;
  static const uint16_t NOT_MODIFIED;
  static const uint16_t USE_PROXY;
  static const uint16_t TEMPORARY_REDIRECT;
  static const uint16_t BAD_REQUEST;
  static const uint16_t UNAUTHORIZED;
  static const uint16_t PAYMENT_REQUIRED;
  static const uint16_t FORBIDDEN;
  static const uint16_t NOT_FOUND;
  static const uint16_t METHOD_NOT_ALLOWED;
  static const uint16_t NOT_ACCEPTABLE;
  static const uint16_t PROXY_AUTHENTICATION_REQUIRED;
  static const uint16_t REQUEST_TIMEOUT;
  static const uint16_t CONFLICT;
  static const uint16_t GONE;
  static const uint16_t LENGTH_REQUIRED;
  static const uint16_t PRECONDITION_FAILED;
  static const uint16_t REQUEST_ENTITY_TOO_LARGE;
  static const uint16_t REQUEST_URI_TOO_LARGE;
  static const uint16_t UNSUPPORTED_MEDIA_TYPE;
  static const uint16_t REQUESTED_RANGE_NOT_SATISFIABLE;
  static const uint16_t EXPECTATION_FAILED;
  static const uint16_t INTERNAL_SERVER_ERROR;
  static const uint16_t NOT_IMPLEMENTED;
  static const uint16_t BAD_GATEWAY;
  static const uint16_t SERVICE_UNAVAILABLE;
  static const uint16_t GATEWAY_TIMEOUT;
  static const uint16_t HTTP_VERSION_NOT_SUPPORTED;

  static std::string string(uint16_t code);
};


// Represents an asynchronous in-memory unbuffered Pipe, currently
// used for streaming HTTP responses via chunked encoding. Note that
// being an in-memory pipe means that this cannot be used across OS
// processes.
//
// Much like unix pipes, data is read until end-of-file is
// encountered; this occurs when the write-end of the pipe is
// closed and there is no outstanding data left to read.
//
// Unlike unix pipes, if the read-end of the pipe is closed before
// the write-end is closed, rather than receiving SIGPIPE or EPIPE
// during a write, the writer is notified via a future. This future
// is discarded if the write-end is closed first.
//
// No buffering means that each non-empty write to the pipe will
// correspond to to an equivalent read from the pipe, and the
// reader must "keep up" with the writer in order to avoid
// unbounded memory growth.
//
// The writer can induce a failure on the reader in order to signal
// that an error has occurred. For example, if we are receiving a
// response but a disconnection occurs before the response is
// completed, we want the reader to detect that a disconnection
// occurred!
//
// TODO(bmahler): Consider aggregating writes into larger reads to
// help the reader keep up (a process::Stream abstraction with
// backpressure would obviate the need for this).
//
// TODO(bmahler): Add a more general process::Stream<T> abstraction
// to represent asynchronous finite/infinite streams (possibly
// with "backpressure" on the writer). This is broadly useful
// (e.g. allocator can expose Stream<Allocation>, http::Pipe
// becomes Stream<string>, process::Queue<T> is just an infinite
// Stream<T> (i.e. completion and error semantics hidden)).
class Pipe
{
private:
  struct Data; // Forward declaration.

public:
  class Reader
  {
  public:
    // Returns data written to the pipe.
    // Returns an empty read when end-of-file is reached.
    // Returns Failure if the writer failed, or the read-end
    // is closed.
    Future<std::string> read();

    // Performs a series of asynchronous reads, until EOF is reached.
    // Returns the concatenated result of the reads.
    // Returns Failure if the writer failed, or the read-end
    // is closed.
    Future<std::string> readAll();

    // Closing the read-end of the pipe before the write-end closes
    // or fails will notify the writer that the reader is no longer
    // interested. Returns false if the read-end was already closed.
    bool close();

    // Comparison operators useful for checking connection equality.
    bool operator==(const Reader& other) const { return data == other.data; }
    bool operator!=(const Reader& other) const { return !(*this == other); }

  private:
    friend class Pipe;

    enum State
    {
      OPEN,
      CLOSED,
    };

    explicit Reader(const std::shared_ptr<Data>& _data)
      : data(_data) {}

    std::shared_ptr<Data> data;
  };

  class Writer
  {
  public:
    // Returns false if the data could not be written because
    // either end of the pipe was already closed. Note that an
    // empty write has no effect.
    bool write(std::string s);

    // Closing the write-end of the pipe will send end-of-file
    // to the reader. Returns false if the write-end of the pipe
    // was already closed or failed.
    bool close();

    // Closes the write-end of the pipe but sends a failure
    // to the reader rather than end-of-file. Returns false
    // if the write-end of the pipe was already closed or failed.
    bool fail(const std::string& message);

    // Returns Nothing when the read-end of the pipe is closed
    // before the write-end is closed, which means the reader
    // was unable to continue reading!
    Future<Nothing> readerClosed() const;

    // Comparison operators useful for checking connection equality.
    bool operator==(const Writer& other) const { return data == other.data; }
    bool operator!=(const Writer& other) const { return !(*this == other); }
  private:
    friend class Pipe;

    enum State
    {
      OPEN,
      CLOSED,
      FAILED,
    };

    explicit Writer(const std::shared_ptr<Data>& _data)
      : data(_data) {}

    std::shared_ptr<Data> data;
  };

  Pipe()
    : data(new Data()) {}

  Reader reader() const;
  Writer writer() const;

  // Comparison operators useful for checking connection equality.
  bool operator==(const Pipe& other) const { return data == other.data; }
  bool operator!=(const Pipe& other) const { return !(*this == other); }
private:
  struct Data
  {
    Data()
      : readEnd(Reader::OPEN), writeEnd(Writer::OPEN) {}

    // Rather than use a process to serialize access to the pipe's
    // internal data we use a 'std::atomic_flag'.
    std::atomic_flag lock = ATOMIC_FLAG_INIT;

    Reader::State readEnd;
    Writer::State writeEnd;

    // Represents readers waiting for data from the pipe.
    std::queue<Owned<Promise<std::string>>> reads;

    // Represents unread writes in the pipe. Note that we omit
    // empty strings as they serve as a signal for end-of-file.
    std::queue<std::string> writes;

    // Signals when the read-end is closed before the write-end.
    Promise<Nothing> readerClosure;

    // Failure reason when the 'writeEnd' is FAILED.
    Option<Failure> failure;
  };

  std::shared_ptr<Data> data;
};


namespace header {

// https://tools.ietf.org/html/rfc2617.
class WWWAuthenticate
{
public:
  static constexpr const char* NAME = "WWW-Authenticate";

  WWWAuthenticate(
      const std::string& authScheme,
      const hashmap<std::string, std::string>& authParam)
    : authScheme_(authScheme),
      authParam_(authParam) {}

  static Try<WWWAuthenticate> create(const std::string& input);

  std::string authScheme();
  hashmap<std::string, std::string> authParam();

private:
  // According to RFC, HTTP/1.1 server may return multiple challenges
  // with a 401 (Authenticate) response. Each challenage is in the
  // format of 'auth-scheme 1*SP 1#auth-param' and each challenage may
  // use a different auth-scheme.
  // https://tools.ietf.org/html/rfc2617#section-4.6
  //
  // TODO(gilbert): We assume there is only one authenticate challenge.
  // Multiple challenges should be supported as well.
  std::string authScheme_;
  hashmap<std::string, std::string> authParam_;
};

} // namespace header {


class Headers : public hashmap<
    std::string,
    std::string,
    CaseInsensitiveHash,
    CaseInsensitiveEqual>
{
public:
  Headers() {}

  Headers(const std::map<std::string, std::string>& map)
    : hashmap<
          std::string,
          std::string,
          CaseInsensitiveHash,
          CaseInsensitiveEqual>(map) {}

  Headers(std::map<std::string, std::string>&& map)
    : hashmap<
          std::string,
          std::string,
          CaseInsensitiveHash,
          CaseInsensitiveEqual>(map) {}

  Headers(std::initializer_list<std::pair<std::string, std::string>> list)
     : hashmap<
          std::string,
          std::string,
          CaseInsensitiveHash,
          CaseInsensitiveEqual>(list) {}

  template <typename T>
  Result<T> get() const
  {
    Option<std::string> value = get(T::NAME);
    if (value.isNone()) {
      return None();
    }
    Try<T> header = T::create(value.get());
    if (header.isError()) {
      return Error(header.error());
    }
    return header.get();
  }

  Option<std::string> get(const std::string& key) const
  {
    return hashmap<
        std::string,
        std::string,
        CaseInsensitiveHash,
        CaseInsensitiveEqual>::get(key);
  }

  Headers operator+(const Headers& that) const
  {
    Headers result = *this;
    result.insert(that.begin(), that.end());
    return result;
  }
};


struct Request
{
  Request()
    : keepAlive(false), type(BODY), received(Clock::now()) {}

  std::string method;

  // TODO(benh): Add major/minor version.

  // For client requests, the URL should be a URI.
  // For server requests, the URL may be a URI or a relative reference.
  URL url;

  Headers headers;

  // TODO(bmahler): Ensure this is consistent with the 'Connection'
  // header; perhaps make this a function that checks the header.
  //
  // TODO(anand): Ideally, this could default to 'true' since
  // persistent connections are the default since HTTP 1.1.
  // Perhaps, we need to go from `keepAlive` to `closeConnection`
  // to reflect the header more accurately, and to have an
  // intuitive default of false.
  //
  // Default: false.
  bool keepAlive;

  // For server requests, this contains the address of the client.
  // Note that this may correspond to a proxy or load balancer address.
  Option<network::Address> client;

  // Clients can choose to provide the entire body at once
  // via BODY or can choose to stream the body over to the
  // server via PIPE.
  //
  // Default: BODY.
  enum
  {
    BODY,
    PIPE
  } type;

  // TODO(bmahler): Add a 'query' field which contains both
  // the URL query and the parsed form data from the body.

  std::string body;
  Option<Pipe::Reader> reader;

  Time received;

  /**
   * Returns whether the encoding is considered acceptable in the
   * response. See RFC 2616 section 14.3 for details.
   */
  bool acceptsEncoding(const std::string& encoding) const;

  /**
   * Returns whether the media type in the "Accept" header  is considered
   * acceptable in the response. See RFC 2616, section 14.1 for the details.
   */
  bool acceptsMediaType(const std::string& mediaType) const;

  /**
   * Returns whether the media type in the `name` header is considered
   * acceptable in the response. The media type should have similar
   * semantics as the "Accept" header. See RFC 2616, section 14.1 for
   * the details.
   */
  bool acceptsMediaType(
      const std::string& name,
      const std::string& mediaType) const;

private:
  bool _acceptsMediaType(
      Option<std::string> name,
      const std::string& mediaType) const;
};


struct Response
{
  Response()
    : type(NONE) {}

  Response(uint16_t _code)
    : type(NONE), code(_code)
  {
    status = Status::string(code);
  }

  explicit Response(
      std::string _body,
      uint16_t _code,
      const std::string& contentType = "text/plain; charset=utf-8")
    : type(BODY),
      body(std::move(_body)),
      code(_code)
  {
    headers["Content-Length"] = stringify(body.size());
    headers["Content-Type"] = contentType;
    status = Status::string(code);
  }

  // TODO(benh): Add major/minor version.
  std::string status;

  Headers headers;

  // Either provide a 'body', an absolute 'path' to a file, or a
  // 'pipe' for streaming a response. Distinguish between the cases
  // using 'type' below.
  //
  // BODY: Uses 'body' as the body of the response. These may be
  // encoded using gzip for efficiency, if 'Content-Encoding' is not
  // already specified.
  //
  // PATH: Attempts to perform a 'sendfile' operation on the file
  // found at 'path'.
  //
  // PIPE: Splices data from the Pipe 'reader' using a "chunked"
  // 'Transfer-Encoding'. The writer uses a Pipe::Writer to
  // perform writes and to detect a closed read-end of the Pipe
  // (i.e. nobody is listening any longer). Once the writer is
  // finished, it will close its end of the pipe to signal end
  // of file to the Reader.
  //
  // In all cases (BODY, PATH, PIPE), you are expected to properly
  // specify the 'Content-Type' header, but the 'Content-Length' and
  // or 'Transfer-Encoding' headers will be filled in for you.
  enum
  {
    NONE,
    BODY,
    PATH,
    PIPE
  } type;

  std::string body;
  std::string path;
  Option<Pipe::Reader> reader;

  uint16_t code;
};


struct OK : Response
{
  OK()
    : Response(Status::OK) {}

  explicit OK(const char* body)
    : Response(std::string(body), Status::OK) {}

  explicit OK(std::string body)
    : Response(std::move(body), Status::OK) {}

  explicit OK(std::string body, const std::string& contentType)
    : Response(std::move(body), Status::OK, contentType) {}

  OK(const JSON::Value& value, const Option<std::string>& jsonp = None());

  OK(JSON::Proxy&& value, const Option<std::string>& jsonp = None());
};


struct Accepted : Response
{
  Accepted()
    : Response(Status::ACCEPTED) {}

  explicit Accepted(std::string body)
    : Response(std::move(body), Status::ACCEPTED) {}
};


struct TemporaryRedirect : Response
{
  explicit TemporaryRedirect(const std::string& url)
    : Response(Status::TEMPORARY_REDIRECT)
  {
    headers["Location"] = url;
  }
};


struct BadRequest : Response
{
  BadRequest()
    : BadRequest("400 Bad Request.") {}

  explicit BadRequest(std::string body)
    : Response(std::move(body), Status::BAD_REQUEST) {}
};


struct Unauthorized : Response
{
  explicit Unauthorized(const std::vector<std::string>& challenges)
    : Unauthorized(challenges, "401 Unauthorized.") {}

  Unauthorized(
      const std::vector<std::string>& challenges,
      std::string body)
    : Response(std::move(body), Status::UNAUTHORIZED)
  {
    // TODO(arojas): Many HTTP client implementations do not support
    // multiple challenges within a single 'WWW-Authenticate' header.
    // Once MESOS-3306 is fixed, we can use multiple entries for the
    // same header.
    headers["WWW-Authenticate"] = strings::join(", ", challenges);
  }
};


struct Forbidden : Response
{
  Forbidden()
    : Forbidden("403 Forbidden.") {}

  explicit Forbidden(std::string body)
    : Response(std::move(body), Status::FORBIDDEN) {}
};


struct NotFound : Response
{
  NotFound()
    : NotFound("404 Not Found.") {}

  explicit NotFound(std::string body)
    : Response(std::move(body), Status::NOT_FOUND) {}
};


struct MethodNotAllowed : Response
{
  // According to RFC 2616, "An Allow header field MUST be present in a
  // 405 (Method Not Allowed) response".

  MethodNotAllowed(
      const std::initializer_list<std::string>& allowedMethods,
      const Option<std::string>& requestMethod = None())
    : Response(
        constructBody(allowedMethods, requestMethod),
        Status::METHOD_NOT_ALLOWED)
  {
    headers["Allow"] = strings::join(", ", allowedMethods);
  }

private:
  static std::string constructBody(
      const std::initializer_list<std::string>& allowedMethods,
      const Option<std::string>& requestMethod)
  {
    return
        "405 Method Not Allowed. Expecting one of { '" +
        strings::join("', '", allowedMethods) + "' }" +
        (requestMethod.isSome()
           ? ", but received '" + requestMethod.get() + "'"
           : "") +
        ".";
  }
};


struct NotAcceptable : Response
{
  NotAcceptable()
    : NotAcceptable("406 Not Acceptable.") {}

  explicit NotAcceptable(std::string body)
    : Response(std::move(body), Status::NOT_ACCEPTABLE) {}
};


struct Conflict : Response
{
  Conflict()
    : Conflict("409 Conflict.") {}

  explicit Conflict(std::string body)
    : Response(std::move(body), Status::CONFLICT) {}
};


struct PreconditionFailed : Response
{
  PreconditionFailed()
    : PreconditionFailed("412 Precondition Failed.") {}

  explicit PreconditionFailed(std::string body)
    : Response(std::move(body), Status::PRECONDITION_FAILED) {}
};


struct UnsupportedMediaType : Response
{
  UnsupportedMediaType()
    : UnsupportedMediaType("415 Unsupported Media Type.") {}

  explicit UnsupportedMediaType(std::string body)
    : Response(std::move(body), Status::UNSUPPORTED_MEDIA_TYPE) {}
};


struct InternalServerError : Response
{
  InternalServerError()
    : InternalServerError("500 Internal Server Error.") {}

  explicit InternalServerError(std::string body)
    : Response(std::move(body), Status::INTERNAL_SERVER_ERROR) {}
};


struct NotImplemented : Response
{
  NotImplemented()
    : NotImplemented("501 Not Implemented.") {}

  explicit NotImplemented(std::string body)
    : Response(std::move(body), Status::NOT_IMPLEMENTED) {}
};


struct ServiceUnavailable : Response
{
  ServiceUnavailable()
    : ServiceUnavailable("503 Service Unavailable.") {}

  explicit ServiceUnavailable(std::string body)
    : Response(std::move(body), Status::SERVICE_UNAVAILABLE) {}
};


namespace path {

// Parses an HTTP path into a map given a pattern (TODO(benh): Make
// the patterns be regular expressions). This returns an error if
// 'pattern' doesn't match 'path'. For example:
//
//   parse("/books/{isbn}/chapters/{chapter}",
//         "/books/0304827484/chapters/3")
//
// Would return a map with the following:
//   books: "books"
//   isbn: "0304827484"
//   chapters: "chapters"
//   chapter: "3"
//
// Another example:
//
//   parse("/books/{isbn}/chapters/{chapter}",
//         "/books/0304827484")
//
// Would return a map with the following:
//   books: "books"
//   isbn: "0304827484"
//
// And another:
//
//   parse("/books/{isbn}/chapters/{chapter}",
//         "/books/0304827484/chapters")
//
// Would return a map with the following:
//   books: "books"
//   isbn: "0304827484"
//   chapters: "chapters"
Try<hashmap<std::string, std::string>> parse(
    const std::string& pattern,
    const std::string& path);

} // namespace path {


/**
 * Returns a percent-encoded string according to RFC 3986.
 * @see <a href="https://tools.ietf.org/html/rfc3986#section-2.3">RFC3986</a>
 *
 * @param s The input string, must not arleady be percent-encoded.
 * @param additional_chars When specified, all characters in it are also
 *     percent-encoded.
 * @return The percent-encoded string of `s`.
 */
std::string encode(
    const std::string& s,
    const std::string& additional_chars = "");


// Decodes a percent-encoded string according to RFC 3986.
// The input string must not already be decoded.
// Returns error on the occurrence of a malformed % escape in s.
Try<std::string> decode(const std::string& s);


/**
 * Decode HTTP responses from the given string.
 *
 * @param s the given string.
 */
Try<std::vector<Response>> decodeResponses(const std::string& s);


namespace query {

// Decodes an HTTP query string into a map. For example:
//
//   decode("foo=1&bar=%20&baz&foo=3")
//
// Would return a map with the following:
//   bar: " "
//   baz: ""
//   foo: "3"
//
// We use the last value for a key for simplicity, since the RFC does not
// specify how to handle duplicate keys:
// http://en.wikipedia.org/wiki/Query_string
// TODO(bmahler): If needed, investigate populating the query map inline
// for better performance.
Try<hashmap<std::string, std::string>> decode(const std::string& query);

std::string encode(const hashmap<std::string, std::string>& query);

} // namespace query {


/**
 * Represents a connection to an HTTP server. Pipelining will be
 * used when there are multiple requests in-flight.
 *
 * TODO(bmahler): This does not prevent pipelining with HTTP/1.0.
 */
class Connection
{
public:
  Connection() = delete;

  /**
   * Sends a request to the server. If there are additional requests
   * in flight, pipelining will occur. If 'streamedResponse' is set,
   * the response body will be of type 'PIPE'. Note that if the
   * request or response has a 'Connection: close' header, the
   * connection will close after the response completes.
   */
  Future<Response> send(const Request& request, bool streamedResponse = false);

  /**
   * Disconnects from the server.
   */
  Future<Nothing> disconnect();

  /**
   * Returns a future that is satisfied when a disconnection occurs.
   */
  Future<Nothing> disconnected();

  bool operator==(const Connection& c) const { return data == c.data; }
  bool operator!=(const Connection& c) const { return !(*this == c); }

  const network::Address localAddress;
  const network::Address peerAddress;

private:
  Connection(
      const network::Socket& s,
      const network::Address& _localAddress,
      const network::Address& _peerAddress);

  friend Future<Connection> connect(
      const network::Address& address,
      Scheme scheme,
      const Option<std::string>& peer_hostname);
  friend Future<Connection> connect(const URL&);

  // Forward declaration.
  struct Data;

  std::shared_ptr<Data> data;
};


Future<Connection> connect(
    const network::Address& address,
    Scheme scheme,
    const Option<std::string>& peer_hostname);


Future<Connection> connect(const network::Address& address, Scheme scheme);


Future<Connection> connect(const URL& url);


namespace internal {

Future<Nothing> serve(
    network::Socket s,
    std::function<Future<Response>(const Request&)>&& f);

} // namespace internal {


// Serves HTTP requests on the specified socket using the specified
// handler.
//
// Returns `Nothing` after serving has completed, either because (1) a
// failure occurred receiving requests or sending responses or (2) the
// HTTP connection was not persistent (i.e., a 'Connection: close'
// header existed either on the request or the response) or (3)
// serving was discarded.
//
// Doing a `discard()` on the Future returned from `serve` will
// discard any current socket receiving and any current socket
// sending and shutdown the socket in both directions.
//
// NOTE: HTTP pipelining is automatically performed. If you don't want
// pipelining you must explicitly sequence/serialize the requests to
// wait for previous responses yourself.
//
// NOTE: The `Request` passed to the handler is of type `PIPE` and should
// always be read using `Request.reader`.
template <typename F>
Future<Nothing> serve(const network::Socket& s, F&& f)
{
  return internal::serve(
      s,
      std::function<Future<Response>(const Request&)>(std::forward<F>(f)));
}


// Forward declaration.
class ServerProcess;


class Server
{
public:
  // Options for creating a server.
  //
  // NOTE: until GCC 5.0 default member initializers prevented the
  // class from being an aggregate which prevented you from being able
  // to use aggregate initialization, thus we introduce and use
  // `DEFAULT_CREATE_OPTIONS` for the default parameter of `create`.
  struct CreateOptions
  {
    Scheme scheme;
    size_t backlog;
  };

  static CreateOptions DEFAULT_CREATE_OPTIONS()
  {
    return {
      /* .scheme = */ Scheme::HTTP,
      /* .backlog = */ 16384,
    };
  };

  // Options for stopping a server.
  //
  // NOTE: see note above as to why we have `DEFAULT_STOP_OPTIONS`.
  struct StopOptions
  {
    // During the grace period:
    //   * No new sockets will be accepted (but on OS X they'll still queue).
    //   * Existing sockets will be shut down for reads to prevent new
    //     requests from arriving.
    //   * Existing sockets will be shut down after already received
    //     requests have their responses sent.
    // After the grace period connections will be forcibly shut down.
    Duration grace_period;
  };

  static StopOptions DEFAULT_STOP_OPTIONS()
  {
    return {
      /* .grace_period = */ Seconds(0),
    };
  };

  static Try<Server> create(
      network::Socket socket,
      std::function<Future<Response>(
          const network::Socket& socket,
          const Request&)>&& f,
      const CreateOptions& options = DEFAULT_CREATE_OPTIONS());

  template <typename F>
  static Try<Server> create(
      network::Socket socket,
      F&& f,
      const CreateOptions& options = DEFAULT_CREATE_OPTIONS())
  {
    return create(
        std::move(socket),
        std::function<Future<Response>(
            const network::Socket&,
            const Request&)>(std::forward<F>(f)),
        options);
  }

  static Try<Server> create(
      const network::Address& address,
      std::function<Future<Response>(
          const network::Socket&,
          const Request&)>&& f,
      const CreateOptions& options = DEFAULT_CREATE_OPTIONS());

  template <typename F>
  static Try<Server> create(
      const network::Address& address,
      F&& f,
      const CreateOptions& options = DEFAULT_CREATE_OPTIONS())
  {
    return create(
        address,
        std::function<Future<Response>(
            const network::Socket&,
            const Request&)>(std::forward<F>(f)),
        options);
  }

  // Movable but not copyable, not assignable.
  Server(Server&& that) = default;
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  ~Server();

  // Runs the server, returns nothing after the server has been
  // stopped or a failure if one occured.
  Future<Nothing> run();

  // Returns after the server has been stopped and all existing
  // connections have been closed.
  Future<Nothing> stop(const StopOptions& options = DEFAULT_STOP_OPTIONS());

  // Returns the bound address of the server.
  Try<network::Address> address() const;

private:
  Server(
      network::Socket&& socket,
      std::function<Future<Response>(
          const network::Socket&,
          const Request&)>&& f);

  network::Socket socket;
  Owned<ServerProcess> process;
};


// Create a http Request from the specified parameters.
Request createRequest(
  const UPID& upid,
  const std::string& method,
  bool enableSSL = false,
  const Option<std::string>& path = None(),
  const Option<Headers>& headers = None(),
  const Option<std::string>& body = None(),
  const Option<std::string>& contentType = None());


Request createRequest(
    const URL& url,
    const std::string& method,
    const Option<Headers>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None());

/**
 * Asynchronously sends an HTTP request to the process and
 * returns the HTTP response once the entire response is received.
 *
 * @param streamedResponse Being true indicates the HTTP response will
 *     be 'PIPE' type, and caller must read the response body from the
 *     Pipe::Reader, otherwise, the HTTP response will be 'BODY' type.
 */
Future<Response> request(
    const Request& request,
    bool streamedResponse = false);


// TODO(Yongqiao Wang): Refactor other functions
// (such as post/get/requestDelete) to use the 'request' function.

// TODO(bmahler): Support discarding the future responses;
// discarding should disconnect from the server.

// TODO(joerg84): Make names consistent (see Mesos-3256).

// Asynchronously sends an HTTP GET request to the specified URL
// and returns the HTTP response of type 'BODY' once the entire
// response is received.
Future<Response> get(
    const URL& url,
    const Option<Headers>& headers = None());


// Asynchronously sends an HTTP GET request to the process with the
// given UPID and returns the HTTP response of type 'BODY' once the
// entire response is received.
Future<Response> get(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<std::string>& query = None(),
    const Option<Headers>& headers = None(),
    const Option<std::string>& scheme = None());


// Asynchronously sends an HTTP POST request to the specified URL
// and returns the HTTP response of type 'BODY' once the entire
// response is received.
Future<Response> post(
    const URL& url,
    const Option<Headers>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None());


// Asynchronously sends an HTTP POST request to the process with the
// given UPID and returns the HTTP response of type 'BODY' once the
// entire response is received.
Future<Response> post(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<Headers>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None(),
    const Option<std::string>& scheme = None());


/**
 * Asynchronously sends an HTTP DELETE request to the process with the
 * given UPID and returns the HTTP response.
 *
 * @param url The target url for the request.
 * @param headers Optional header for the request.
 * @return A future with the HTTP response.
 */
Future<Response> requestDelete(
    const URL& url,
    const Option<Headers>& headers = None());


/**
 * Asynchronously sends an HTTP DELETE request to the process with the
 * given UPID and returns the HTTP response.
 *
 * @param upid The target process's assigned untyped PID.
 * @param path The optional path to be be deleted. If not send the
     request is send to the process directly.
 * @param headers Optional headers for the request.
 * @param scheme Optional scheme for the request.
 * @return A future with the HTTP response.
 */
Future<Response> requestDelete(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<Headers>& headers = None(),
    const Option<std::string>& scheme = None());


namespace streaming {

// Asynchronously sends an HTTP GET request to the specified URL
// and returns the HTTP response of type 'PIPE' once the response
// headers are received. The caller must read the response body
// from the Pipe::Reader.
Future<Response> get(
    const URL& url,
    const Option<Headers>& headers = None());

// Asynchronously sends an HTTP GET request to the process with the
// given UPID and returns the HTTP response of type 'PIPE' once the
// response headers are received. The caller must read the response
// body from the Pipe::Reader.
Future<Response> get(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<std::string>& query = None(),
    const Option<Headers>& headers = None(),
    const Option<std::string>& scheme = None());

// Asynchronously sends an HTTP POST request to the specified URL
// and returns the HTTP response of type 'PIPE' once the response
// headers are received. The caller must read the response body
// from the Pipe::Reader.
Future<Response> post(
    const URL& url,
    const Option<Headers>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None());

// Asynchronously sends an HTTP POST request to the process with the
// given UPID and returns the HTTP response of type 'PIPE' once the
// response headers are received. The caller must read the response
// body from the Pipe::Reader.
Future<Response> post(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<Headers>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None(),
    const Option<std::string>& scheme = None());

} // namespace streaming {

} // namespace http {
} // namespace process {

#endif // __PROCESS_HTTP_HPP__

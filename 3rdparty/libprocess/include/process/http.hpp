#ifndef __PROCESS_HTTP_HPP__
#define __PROCESS_HTTP_HPP__

#include <stdint.h>

#include <iosfwd>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/memory.hpp>
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

// Status code reason strings, from the HTTP1.1 RFC:
// http://www.w3.org/Protocols/rfc2616/rfc2616-sec6.html
extern hashmap<uint16_t, std::string> statuses;

// Initializes 'statuses'.
// TODO(bmahler): Provide a function that returns the string for
// a status code instead!
void initialize();


struct Request
{
  // TODO(benh): Add major/minor version.
  // TODO(bmahler): Header names are not case sensitive! Either make these
  // case-insensitive, or add a variable for each header in HTTP 1.0/1.1 (like
  // we've done here with keepAlive).
  // Tracked by: https://issues.apache.org/jira/browse/MESOS-328.
  hashmap<std::string, std::string> headers;
  std::string method;

  // TODO(benh): Replace 'url', 'path', 'query', and 'fragment' with URL.
  std::string url; // (path?query#fragment)
  std::string path;
  hashmap<std::string, std::string> query;
  std::string fragment;

  std::string body;

  bool keepAlive;

  // Returns whether the encoding is considered acceptable in the request.
  // TODO(bmahler): Consider this logic being in decoder.hpp, and having the
  // Request contain a member variable for each popular HTTP 1.0/1.1 header.
  bool accepts(const std::string& encoding) const;
};


// Represents an asynchronous in-memory unbuffered Pipe, currently
// used for streaming HTTP responses via chunked encoding. Note that
// being an in-memory pipe means that this cannot be used across OS
// processes.
//
// Much like unix pipes, data is read until end-of-file is
// encountered; this occurs when the the write-end of the pipe is
// closed and there is no outstanding data left to read.
//
// Unlike unix pipes, if the read-end of the pipe is closed before
// the write-end is closed, rather than receiving SIGPIPE or EPIPE
// during a write, the writer is notified via a future. Like unix
// pipes, we are not notified if the read-end is closed after the
// write-end is closed, even if data is remaining in the pipe!
//
// No buffering means that each non-empty write to the pipe will
// correspond to to an equivalent read from the pipe, and the
// reader must "keep up" with the writer in order to avoid
// unbounded memory growth.
//
// TODO(bmahler): The writer needs to be able to induce a failure
// on the reader to signal an error has occurred. For example, if
// we are receiving a response but a disconnection occurs before
// the response is completed, we want the reader to detect that a
// disconnection occurred!
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
    // Returns Failure if the read-end of the pipe is closed.
    Future<std::string> read();

    // Closing the read-end of the pipe before the write-end closes
    // will notify the writer that the reader is no longer interested.
    // Returns false if the read-end of the pipe was already closed.
    bool close();

  private:
    friend class Pipe;
    explicit Reader(const memory::shared_ptr<Data>& _data) : data(_data) {}
    memory::shared_ptr<Data> data;
  };

  class Writer
  {
  public:
    // Returns false if the data could not be written because
    // either end of the pipe was already closed. Note that an
    // empty write has no effect.
    bool write(const std::string& s);

    // Closing the write-end of the pipe will send end-of-file
    // to the reader. Returns false if the write-end of the pipe
    // was already closed.
    bool close();

    // Returns Nothing when the read-end of the pipe is closed
    // before the write-end is closed, which means the reader
    // was unable to continue reading!
    Future<Nothing> readerClosed();

  private:
    friend class Pipe;
    explicit Writer(const memory::shared_ptr<Data>& _data) : data(_data) {}
    memory::shared_ptr<Data> data;
  };

  Pipe() : data(new Data()) {}

  Reader reader() const;
  Writer writer() const;

private:
  enum State
  {
    OPEN,
    CLOSED,
  };

  struct Data
  {
    Data() : lock(0), readEnd(OPEN), writeEnd(OPEN) {}

    // Rather than use a process to serialize access to the pipe's
    // internal data we use a low-level "lock" which we acquire and
    // release using atomic builtins.
    int lock;

    State readEnd;
    State writeEnd;

    // Represents readers waiting for data from the pipe.
    std::queue<Owned<Promise<std::string>>> reads;

    // Represents unread writes in the pipe. Note that we omit
    // empty strings as they serve as a signal for end-of-file.
    std::queue<std::string> writes;

    // Signals when the read-end is closed before the write-end.
    Promise<Nothing> readerClosure;
  };

  memory::shared_ptr<Data> data;
};


struct Response
{
  Response()
    : type(NONE)
  {}

  explicit Response(const std::string& _body)
    : type(BODY),
      body(_body)
  {
    headers["Content-Length"] = stringify(body.size());
  }

  // TODO(benh): Add major/minor version.
  std::string status;
  hashmap<std::string, std::string> headers;

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
  enum {
    NONE,
    BODY,
    PATH,
    PIPE
  } type;

  std::string body;
  std::string path;
  Option<Pipe::Reader> reader;
};


struct OK : Response
{
  OK()
  {
    status = "200 OK";
  }

  explicit OK(const char* body) : Response(std::string(body))
  {
    status = "200 OK";
  }

  explicit OK(const std::string& body) : Response(body)
  {
    status = "200 OK";
  }

  OK(const JSON::Value& value, const Option<std::string>& jsonp = None())
  {
    type = BODY;

    status = "200 OK";

    std::ostringstream out;

    if (jsonp.isSome()) {
      out << jsonp.get() << "(";
    }

    out << value;

    if (jsonp.isSome()) {
      out << ");";
      headers["Content-Type"] = "text/javascript";
    } else {
      headers["Content-Type"] = "application/json";
    }

    headers["Content-Length"] = stringify(out.str().size());
    body = out.str().data();
  }
};


struct Accepted : Response
{
  Accepted()
  {
    status = "202 Accepted";
  }

  explicit Accepted(const std::string& body) : Response(body)
  {
    status = "202 Accepted";
  }
};


struct TemporaryRedirect : Response
{
  explicit TemporaryRedirect(const std::string& url)
  {
    status = "307 Temporary Redirect";
    headers["Location"] = url;
  }
};

struct Unauthorized : Response
{
  Unauthorized(const std::string& realm)
  {
    status = "401 Unauthorized";
    headers["WWW-authenticate"] = "Basic realm=\"" + realm + "\"";
  }

  Unauthorized(const std::string& realm, const std::string& body)
    : Response(body)
  {
    status = "401 Unauthorized";
    headers["WWW-authenticate"] = "Basic realm=\"" + realm + "\"";
  }
};

struct BadRequest : Response
{
  BadRequest()
  {
    status = "400 Bad Request";
  }

  explicit BadRequest(const std::string& body)
    : Response(body)
  {
    status = "400 Bad Request";
  }
};


struct Forbidden : Response
{
  Forbidden()
  {
    status = "403 Forbidden";
  }

  explicit Forbidden(const std::string& body) : Response(body)
  {
    status = "403 Forbidden";
  }
};


struct NotFound : Response
{
  NotFound()
  {
    status = "404 Not Found";
  }

  explicit NotFound(const std::string& body) : Response(body)
  {
    status = "404 Not Found";
  }
};


struct InternalServerError : Response
{
  InternalServerError()
  {
    status = "500 Internal Server Error";
  }

  explicit InternalServerError(const std::string& body) : Response(body)
  {
    status = "500 Internal Server Error";
  }
};


struct ServiceUnavailable : Response
{
  ServiceUnavailable()
  {
    status = "503 Service Unavailable";
  }

  explicit ServiceUnavailable(const std::string& body) : Response(body)
  {
    status = "503 Service Unavailable";
  }
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
Try<hashmap<std::string, std::string> > parse(
    const std::string& pattern,
    const std::string& path);

} // namespace path {


// Returns a percent-encoded string according to RFC 3986.
// The input string must not already be percent encoded.
std::string encode(const std::string& s);


// Decodes a percent-encoded string according to RFC 3986.
// The input string must not already be decoded.
// Returns error on the occurrence of a malformed % escape in s.
Try<std::string> decode(const std::string& s);


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


// Represents a Uniform Resource Locator:
//   scheme://domain|ip:port/path?query#fragment
struct URL
{
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

  std::string scheme;
  // TODO(benh): Consider using unrestricted union for 'domain' and 'ip'.
  Option<std::string> domain;
  Option<net::IP> ip;
  uint16_t port;
  std::string path;
  hashmap<std::string, std::string> query;
  Option<std::string> fragment;
};


std::ostream& operator << (
    std::ostream& stream,
    const URL& url);


// TODO(bmahler): Consolidate these functions into a single
// http::request function that takes a 'Request' object.


// Asynchronously sends an HTTP GET request to the specified URL
// and returns the HTTP response of type 'BODY' once the entire
// response is received.
Future<Response> get(
    const URL& url,
    const Option<hashmap<std::string, std::string>>& headers = None());


// Asynchronously sends an HTTP GET request to the process with the
// given UPID and returns the HTTP response of type 'BODY' once the
// entire response is received.
Future<Response> get(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<std::string>& query = None(),
    const Option<hashmap<std::string, std::string>>& headers = None());


// Asynchronously sends an HTTP POST request to the specified URL
// and returns the HTTP response of type 'BODY' once the entire
// response is received.
Future<Response> post(
    const URL& url,
    const Option<hashmap<std::string, std::string>>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None());


// Asynchronously sends an HTTP POST request to the process with the
// given UPID and returns the HTTP response of type 'BODY' once the
// entire response is received.
Future<Response> post(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<hashmap<std::string, std::string>>& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None());

} // namespace http {
} // namespace process {

#endif // __PROCESS_HTTP_HPP__

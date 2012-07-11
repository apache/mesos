#ifndef __PROCESS_HTTP_HPP__
#define __PROCESS_HTTP_HPP__

#include <map>
#include <string>

namespace process {
namespace http {

struct Request
{
  // TODO(benh): Add major/minor version.
  std::map<std::string, std::string> headers;
  std::string method;
  std::string path;
  std::string url;
  std::string fragment;
  std::string query;
  std::string body;
  bool keepAlive;
};


struct Response
{
  Response() : type(BODY) {}

  // TODO(benh): Add major/minor version.
  std::string status;
  std::map<std::string, std::string> headers;

  // TODO(benh): Make body a stream (channel) instead, and allow a
  // response to be returned without forcing the stream to be
  // finished.

  // Either provide a 'body' or an absolute 'path' to a file. If a
  // path is specified then we will attempt to perform a 'sendfile'
  // operation on the file. In either case you are expected to
  // properly specify the 'Content-Type' header, but the
  // 'Content-Length' header will be filled in for you if you specify
  // a path. Distinguish between the two using 'type' below.
  enum {
    BODY,
    PATH
  } type;

  std::string body;
  std::string path;
};


struct OK : Response
{
  OK() : Response()
  {
    status = "200 OK";
  }
};


struct BadRequest : Response
{
  BadRequest() : Response()
  {
    status = "400 Bad Request";
  }
};


struct NotFound : Response
{
  NotFound() : Response()
  {
    status = "404 Not Found";
  }
};


struct InternalServerError : Response
{
  InternalServerError() : Response()
  {
    status = "500 Internal Server Error";
  }
};


struct ServiceUnavailable : Response
{
  ServiceUnavailable() : Response()
  {
    status = "503 Service Unavailable";
  }
};

} // namespace http {
} // namespace process {

#endif // __PROCESS_HTTP_HPP__

#ifndef __HTTP_HPP__
#define __HTTP_HPP__

namespace process {

struct HttpRequest
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


struct HttpResponse
{
  // TODO(benh): Add major/minor version.
  std::string status;
  std::map<std::string, std::string> headers;
  std::string body;
};


struct HttpOKResponse : HttpResponse
{
  HttpOKResponse()
  {
    status = "200 OK";
  }
};


struct HttpNotFoundResponse : HttpResponse
{
  HttpNotFoundResponse()
  {
    status = "404 Not Found";
  }
};

} // namespace process {

#endif // __HTTP_HPP

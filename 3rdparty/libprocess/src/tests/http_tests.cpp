#include <arpa/inet.h>

#include <gmock/gmock.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <string>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/io.hpp>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>

#include "encoder.hpp"

using namespace process;

using testing::_;
using testing::Assign;
using testing::DoAll;
using testing::EndsWith;
using testing::Invoke;
using testing::Return;


class HttpProcess : public Process<HttpProcess>
{
public:
  HttpProcess()
  {
    route("/body", None(), &HttpProcess::body);
    route("/pipe", None(), &HttpProcess::pipe);
    route("/get", None(), &HttpProcess::get);
    route("/post", None(), &HttpProcess::post);
  }

  MOCK_METHOD1(body, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(pipe, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(get, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(post, Future<http::Response>(const http::Request&));
};


TEST(HTTP, Endpoints)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HttpProcess process;

  spawn(process);

  // First hit '/body' (using explicit sockets and HTTP/1.0).
  int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

  ASSERT_LE(0, s);

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = PF_INET;
  addr.sin_port = htons(process.self().port);
  addr.sin_addr.s_addr = process.self().ip;

  ASSERT_EQ(0, connect(s, (sockaddr*) &addr, sizeof(addr)));

  std::ostringstream out;
  out << "GET /" << process.self().id << "/body"
      << " HTTP/1.0\r\n"
      << "Connection: Keep-Alive\r\n"
      << "\r\n";

  const std::string& data = out.str();

  EXPECT_CALL(process, body(_))
    .WillOnce(Return(http::OK()));

   ASSERT_SOME(os::write(s, data));

  std::string response = "HTTP/1.1 200 OK";

  char temp[response.size()];
  ASSERT_LT(0, ::read(s, temp, response.size()));
  ASSERT_EQ(response, std::string(temp, response.size()));

  ASSERT_EQ(0, close(s));

  // Now hit '/pipe' (by using http::get).
  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.pipe = pipes[0];

  Future<Nothing> pipe;
  EXPECT_CALL(process, pipe(_))
    .WillOnce(DoAll(FutureSatisfy(&pipe),
                    Return(ok)));

  Future<http::Response> future = http::get(process.self(), "pipe");

  AWAIT_READY(pipe);

  ASSERT_SOME(os::write(pipes[1], "Hello World\n"));
  ASSERT_SOME(os::close(pipes[1]));

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);
  ASSERT_EQ("chunked", future.get().headers["Transfer-Encoding"]);
  ASSERT_EQ("Hello World\n", future.get().body);

  terminate(process);
  wait(process);
}


TEST(HTTP, Encode)
{
  std::string unencoded = "a$&+,/:;=?@ \"<>#%{}|\\^~[]`\x19\x80\xFF";
  unencoded += std::string("\x00", 1); // Add a null byte to the end.

  std::string encoded = http::encode(unencoded);

  EXPECT_EQ("a%24%26%2B%2C%2F%3A%3B%3D%3F%40%20%22%3C%3E%23"
            "%25%7B%7D%7C%5C%5E%7E%5B%5D%60%19%80%FF%00",
            encoded);

  EXPECT_SOME_EQ(unencoded, http::decode(encoded));

  EXPECT_ERROR(http::decode("%"));
  EXPECT_ERROR(http::decode("%1"));
  EXPECT_ERROR(http::decode("%;1"));
  EXPECT_ERROR(http::decode("%1;"));
}


TEST(HTTP, PathParse)
{
  const std::string pattern = "/books/{isbn}/chapters/{chapter}";

  Try<hashmap<std::string, std::string> > parse =
    http::path::parse(pattern, "/books/0304827484/chapters/3");

  ASSERT_SOME(parse);
  EXPECT_EQ(4, parse.get().size());
  EXPECT_EQ("books", parse.get()["books"]);
  EXPECT_EQ("0304827484", parse.get()["isbn"]);
  EXPECT_EQ("chapters", parse.get()["chapters"]);
  EXPECT_EQ("3", parse.get()["chapter"]);

  parse = http::path::parse(pattern, "/books/0304827484");

  ASSERT_SOME(parse);
  EXPECT_EQ(2, parse.get().size());
  EXPECT_EQ("books", parse.get()["books"]);
  EXPECT_EQ("0304827484", parse.get()["isbn"]);

  parse = http::path::parse(pattern, "/books/0304827484/chapters");

  ASSERT_SOME(parse);
  EXPECT_EQ(3, parse.get().size());
  EXPECT_EQ("books", parse.get()["books"]);
  EXPECT_EQ("0304827484", parse.get()["isbn"]);
  EXPECT_EQ("chapters", parse.get()["chapters"]);

  parse = http::path::parse(pattern, "/foo/0304827484/chapters");

  EXPECT_ERROR(parse);
  EXPECT_EQ("Expecting 'books' not 'foo'", parse.error());

  parse = http::path::parse(pattern, "/books/0304827484/bar");

  EXPECT_ERROR(parse);
  EXPECT_EQ("Expecting 'chapters' not 'bar'", parse.error());

  parse = http::path::parse(pattern, "/books/0304827484/chapters/3/foo/bar");

  EXPECT_ERROR(parse);
  EXPECT_EQ("Not expecting suffix 'foo/bar'", parse.error());
}


http::Response validateGetWithoutQuery(const http::Request& request)
{
  EXPECT_EQ("GET", request.method);
  EXPECT_THAT(request.path, EndsWith("get"));
  EXPECT_EQ("", request.body);
  EXPECT_EQ("", request.fragment);
  EXPECT_TRUE(request.query.empty());

  return http::OK();
}


http::Response validateGetWithQuery(const http::Request& request)
{
  EXPECT_EQ("GET", request.method);
  EXPECT_THAT(request.path, EndsWith("get"));
  EXPECT_EQ("", request.body);
  EXPECT_EQ("frag", request.fragment);
  EXPECT_EQ("bar", request.query.at("foo"));
  EXPECT_EQ(1, request.query.size());

  return http::OK();
}


TEST(HTTP, Get)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HttpProcess process;

  spawn(process);

  EXPECT_CALL(process, get(_))
    .WillOnce(Invoke(validateGetWithoutQuery));

  Future<http::Response> noQueryFuture = http::get(process.self(), "get");

  AWAIT_READY(noQueryFuture);
  ASSERT_EQ(http::statuses[200], noQueryFuture.get().status);

  EXPECT_CALL(process, get(_))
    .WillOnce(Invoke(validateGetWithQuery));

  Future<http::Response> queryFuture =
    http::get(process.self(), "get", "foo=bar#frag");

  AWAIT_READY(queryFuture);
  ASSERT_EQ(http::statuses[200], queryFuture.get().status);

  terminate(process);
  wait(process);
}


http::Response validatePost(const http::Request& request)
{
  EXPECT_EQ("POST", request.method);
  EXPECT_THAT(request.path, EndsWith("post"));
  EXPECT_EQ("This is the payload.", request.body);
  EXPECT_EQ("", request.fragment);
  EXPECT_TRUE(request.query.empty());

  return http::OK();
}


TEST(HTTP, Post)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HttpProcess process;

  spawn(process);

  // Test the case where there is a content type but no body.
  Future<http::Response> future =
    http::post(process.self(), "post", None(), "text/plain");

  AWAIT_EXPECT_FAILED(future);

  EXPECT_CALL(process, post(_))
    .WillOnce(Invoke(validatePost));

  future =
    http::post(process.self(), "post", "This is the payload.", "text/plain");

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);

  terminate(process);
  wait(process);
}

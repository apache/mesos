#include <gmock/gmock.h>

#include <deque>
#include <string>

#include <process/socket.hpp>

#include <stout/gtest.hpp>

#include "decoder.hpp"

using namespace process;
using namespace process::http;

using std::deque;
using std::string;

using process::network::Socket;

TEST(DecoderTest, Request)
{
  Try<Socket> socket = Socket::create();
  ASSERT_SOME(socket);
  DataDecoder decoder = DataDecoder(socket.get());

  const string data =
    "GET /path/file.json?key1=value1&key2=value2#fragment HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: close\r\n"
    "Accept-Encoding: compress, gzip\r\n"
    "\r\n";

  deque<Request*> requests = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1, requests.size());

  Request* request = requests[0];
  EXPECT_EQ("GET", request->method);
  EXPECT_EQ("/path/file.json", request->path);
  EXPECT_EQ("/path/file.json?key1=value1&key2=value2#fragment", request->url);
  EXPECT_EQ("fragment", request->fragment);
  EXPECT_TRUE(request->body.empty());
  EXPECT_FALSE(request->keepAlive);

  EXPECT_EQ(3, request->headers.size());
  EXPECT_SOME_EQ("localhost", request->headers.get("Host"));
  EXPECT_SOME_EQ("close", request->headers.get("Connection"));
  EXPECT_SOME_EQ("compress, gzip", request->headers.get("Accept-Encoding"));

  EXPECT_EQ(2, request->query.size());
  EXPECT_SOME_EQ("value1", request->query.get("key1"));
  EXPECT_SOME_EQ("value2", request->query.get("key2"));

  delete request;
}


TEST(DecoderTest, RequestHeaderContinuation)
{
  Try<Socket> socket = Socket::create();
  ASSERT_SOME(socket);
  DataDecoder decoder = DataDecoder(socket.get());

  const string data =
    "GET /path/file.json HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: close\r\n"
    "Accept-Encoding: compress,"
    "                 gzip\r\n"
    "\r\n";

  deque<Request*> requests = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1, requests.size());

  Request* request = requests[0];
  EXPECT_SOME_EQ("compress,                 gzip",
                 request->headers.get("Accept-Encoding"));
  delete request;
}


// This is expected to fail for now, see my TODO(bmahler) on http::Request.
TEST(DecoderTest, DISABLED_RequestHeaderCaseInsensitive)
{
  Try<Socket> socket = Socket::create();
  ASSERT_SOME(socket);
  DataDecoder decoder = DataDecoder(socket.get());

  const string data =
    "GET /path/file.json HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "cOnnECtioN: close\r\n"
    "accept-ENCODING: compress, gzip\r\n"
    "\r\n";

  deque<Request*> requests = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1, requests.size());

  Request* request = requests[0];
  EXPECT_FALSE(request->keepAlive);

  EXPECT_SOME_EQ("compress, gzip", request->headers.get("Accept-Encoding"));

  delete request;
}


TEST(DecoderTest, Response)
{
  ResponseDecoder decoder;

  const string data =
    "HTTP/1.1 200 OK\r\n"
    "Date: Fri, 31 Dec 1999 23:59:59 GMT\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "hi";

  deque<Response*> responses = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1, responses.size());

  Response* response = responses[0];

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(Response::BODY, response->type);
  EXPECT_EQ("hi", response->body);

  EXPECT_EQ(3, response->headers.size());

  delete response;
}


TEST(DecoderTest, StreamingResponse)
{
  StreamingResponseDecoder decoder;

  const string headers =
    "HTTP/1.1 200 OK\r\n"
    "Date: Fri, 31 Dec 1999 23:59:59 GMT\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "\r\n";

  const string body = "hi";

  deque<Response*> responses = decoder.decode(headers.data(), headers.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1, responses.size());

  Response* response = responses[0];

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(3, response->headers.size());

  ASSERT_EQ(Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  decoder.decode(body.data(), body.length());

  // Feeding EOF to the decoder should be ok.
  decoder.decode("", 0);

  EXPECT_TRUE(read.isReady());
  EXPECT_EQ("hi", read.get());

  // Response should be complete.
  read = reader.read();
  EXPECT_TRUE(read.isReady());
  EXPECT_EQ("", read.get());
}


TEST(DecoderTest, StreamingResponseFailure)
{
  StreamingResponseDecoder decoder;

  const string headers =
    "HTTP/1.1 200 OK\r\n"
    "Date: Fri, 31 Dec 1999 23:59:59 GMT\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "\r\n";

  // The body is shorter than the content length!
  const string body = "1";

  deque<Response*> responses = decoder.decode(headers.data(), headers.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1, responses.size());

  Response* response = responses[0];

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(3, response->headers.size());

  ASSERT_EQ(Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  decoder.decode(body.data(), body.length());

  EXPECT_TRUE(read.isReady());
  EXPECT_EQ("1", read.get());

  // Body is not yet complete.
  read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Feeding EOF to the decoder should trigger a failure!
  decoder.decode("", 0);

  EXPECT_TRUE(read.isFailed());
  EXPECT_EQ("failed to decode body", read.failure());
}

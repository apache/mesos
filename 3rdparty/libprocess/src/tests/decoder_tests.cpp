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

#include <gmock/gmock.h>

#include <deque>
#include <string>

#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>

#include "decoder.hpp"

namespace http = process::http;

using process::DataDecoder;
using process::Future;
using process::Owned;
using process::ResponseDecoder;
using process::StreamingRequestDecoder;
using process::StreamingResponseDecoder;

using std::deque;
using std::string;

// TODO(anand): Parameterize the response decoder tests.

template <typename T>
class RequestDecoderTest : public ::testing::Test {};


typedef ::testing::Types<DataDecoder, StreamingRequestDecoder>
  RequestDecoderTypes;


// The request decoder tests are parameterized by the type of request decoder.
TYPED_TEST_CASE(RequestDecoderTest, RequestDecoderTypes);


TYPED_TEST(RequestDecoderTest, Request)
{
  TypeParam decoder;

  const string data =
    "GET /path/file.json?key1=value1&key2=value2#fragment HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: close\r\n"
    "Accept-Encoding: compress, gzip\r\n"
    "\r\n";

  deque<http::Request*> requests = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1u, requests.size());

  Owned<http::Request> request(requests[0]);
  EXPECT_EQ("GET", request->method);

  EXPECT_EQ("/path/file.json", request->url.path);
  EXPECT_SOME_EQ("fragment", request->url.fragment);
  EXPECT_EQ(2u, request->url.query.size());
  EXPECT_SOME_EQ("value1", request->url.query.get("key1"));
  EXPECT_SOME_EQ("value2", request->url.query.get("key2"));

  Future<string> body = [&request]() -> Future<string> {
    if (request->type == http::Request::BODY) {
      return request->body;
    }

    return request->reader->readAll();
  }();

  AWAIT_EXPECT_EQ(string(""), body);
  EXPECT_FALSE(request->keepAlive);

  EXPECT_EQ(3u, request->headers.size());
  EXPECT_SOME_EQ("localhost", request->headers.get("Host"));
  EXPECT_SOME_EQ("close", request->headers.get("Connection"));
  EXPECT_SOME_EQ("compress, gzip", request->headers.get("Accept-Encoding"));
}


TYPED_TEST(RequestDecoderTest, HeaderContinuation)
{
  TypeParam decoder;

  const string data =
    "GET /path/file.json HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: close\r\n"
    "Accept-Encoding: compress,"
    "                 gzip\r\n"
    "\r\n";

  deque<http::Request*> requests = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1u, requests.size());

  Owned<http::Request> request(requests[0]);
  EXPECT_SOME_EQ("compress,                 gzip",
                 request->headers.get("Accept-Encoding"));
}


TYPED_TEST(RequestDecoderTest, HeaderCaseInsensitive)
{
  TypeParam decoder;

  const string data =
    "GET /path/file.json HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "cOnnECtioN: close\r\n"
    "accept-ENCODING: compress, gzip\r\n"
    "\r\n";

  deque<http::Request*> requests = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1u, requests.size());

  Owned<http::Request> request(requests[0]);
  EXPECT_FALSE(request->keepAlive);

  EXPECT_SOME_EQ("compress, gzip", request->headers.get("Accept-Encoding"));
}


TYPED_TEST(RequestDecoderTest, InvalidQueryArgs)
{
  TypeParam decoder;

  const string data =
    "GET /path/file.json?%x=%y&key2=value2#fragment HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: close\r\n"
    "Accept-Encoding: compress, gzip\r\n"
    "\r\n";

  deque<http::Request*> requests = decoder.decode(data.data(), data.length());
  EXPECT_TRUE(decoder.failed());
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

  deque<http::Response*> responses = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1u, responses.size());

  Owned<http::Response> response(responses[0]);

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(http::Response::BODY, response->type);
  EXPECT_EQ("hi", response->body);

  EXPECT_EQ(3u, response->headers.size());
}


TEST(DecoderTest, ResponseWithUnspecifiedLength)
{
  ResponseDecoder decoder;

  const string data =
    "HTTP/1.1 200 OK\r\n"
    "Date: Fri, 31 Dec 1999 23:59:59 GMT\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "hi";

  deque<http::Response*> responses = decoder.decode(data.data(), data.length());
  ASSERT_FALSE(decoder.failed());
  ASSERT_TRUE(responses.empty());

  responses = decoder.decode("", 0);
  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1u, responses.size());

  Owned<http::Response> response(responses[0]);

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(http::Response::BODY, response->type);
  EXPECT_EQ("hi", response->body);

  EXPECT_EQ(2u, response->headers.size());
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

  deque<http::Response*> responses =
    decoder.decode(headers.data(), headers.length());

  EXPECT_FALSE(decoder.failed());
  EXPECT_TRUE(decoder.writingBody());
  ASSERT_EQ(1u, responses.size());

  Owned<http::Response> response(responses[0]);

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(3u, response->headers.size());

  ASSERT_EQ(http::Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  decoder.decode(body.data(), body.length());
  EXPECT_FALSE(decoder.failed());
  EXPECT_FALSE(decoder.writingBody());

  // Feeding EOF to the decoder should be ok.
  decoder.decode("", 0);
  EXPECT_FALSE(decoder.failed());
  EXPECT_FALSE(decoder.writingBody());

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

  deque<http::Response*> responses =
    decoder.decode(headers.data(), headers.length());

  EXPECT_FALSE(decoder.failed());
  EXPECT_TRUE(decoder.writingBody());

  ASSERT_EQ(1u, responses.size());
  Owned<http::Response> response(responses[0]);

  EXPECT_EQ("200 OK", response->status);
  EXPECT_EQ(3u, response->headers.size());

  ASSERT_EQ(http::Response::PIPE, response->type);
  ASSERT_SOME(response->reader);

  http::Pipe::Reader reader = response->reader.get();
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  decoder.decode(body.data(), body.length());
  EXPECT_FALSE(decoder.failed());
  EXPECT_TRUE(decoder.writingBody());

  EXPECT_TRUE(read.isReady());
  EXPECT_EQ("1", read.get());

  // Body is not yet complete.
  read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Feeding EOF to the decoder should trigger a failure!
  decoder.decode("", 0);
  EXPECT_TRUE(decoder.failed());
  EXPECT_FALSE(decoder.writingBody());

  EXPECT_TRUE(read.isFailed());
  EXPECT_EQ("failed to decode body", read.failure());
}


TEST(DecoderTest, StreamingResponseInvalidHeader)
{
  StreamingResponseDecoder decoder;

  const string headers =
    "HTTP/1.1 999 OK\r\n"
    "Date: Fri, 31 Dec 1999 23:59:59 GMT\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n";

  deque<http::Response*> responses =
    decoder.decode(headers.data(), headers.length());

  EXPECT_TRUE(decoder.failed());
}

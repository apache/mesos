/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <arpa/inet.h>

#include <gmock/gmock.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <string>
#include <vector>

#include <process/address.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/base64.hpp>
#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "encoder.hpp"

namespace http = process::http;

using process::Future;
using process::Owned;
using process::Process;
using process::Promise;

using process::http::URL;

using process::network::Socket;

using std::string;
using std::vector;

using testing::_;
using testing::Assign;
using testing::DoAll;
using testing::EndsWith;
using testing::Invoke;
using testing::Return;

class HttpProcess : public Process<HttpProcess>
{
public:
  HttpProcess() {}

  MOCK_METHOD1(body, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(pipe, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(get, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(post, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(requestDelete, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(a, Future<http::Response>(const http::Request&));
  MOCK_METHOD1(abc, Future<http::Response>(const http::Request&));

protected:
  virtual void initialize()
  {
    route("/auth", None(), &HttpProcess::auth);
    route("/body", None(), &HttpProcess::body);
    route("/pipe", None(), &HttpProcess::pipe);
    route("/get", None(), &HttpProcess::get);
    route("/post", None(), &HttpProcess::post);
    route("/delete", None(), &HttpProcess::requestDelete);
    route("/a", None(), &HttpProcess::a);
    route("/a/b/c", None(), &HttpProcess::abc);
  }

  Future<http::Response> auth(const http::Request& request)
  {
    string encodedAuth = base64::encode("testuser:testpass");
    Option<string> authHeader = request.headers.get("Authorization");
    if (!authHeader.isSome() || (authHeader.get() != "Basic " + encodedAuth)) {
      return http::Unauthorized("testrealm");
    }
    return http::OK();
  }
};


class Http
{
public:
  Http() : process(new HttpProcess())
  {
    spawn(process.get());
  }

  ~Http()
  {
    terminate(process.get());
    wait(process.get());
  }

  Owned<HttpProcess> process;
};


// TODO(vinod): Use AWAIT_EXPECT_RESPONSE_STATUS_EQ in the tests.

TEST(HTTPTest, Auth)
{
  Http http;

  // Test the case where there is no auth.
  Future<http::Response> noAuthFuture = http::get(http.process->self(), "auth");

  AWAIT_READY(noAuthFuture);
  EXPECT_EQ(http::statuses[401], noAuthFuture.get().status);
  ASSERT_SOME_EQ("Basic realm=\"testrealm\"",
                 noAuthFuture.get().headers.get("WWW-authenticate"));

  // Now test passing wrong auth header.
  http::Headers headers;
  headers["Authorization"] = "Basic " + base64::encode("testuser:wrongpass");

  Future<http::Response> wrongAuthFuture =
    http::get(http.process->self(), "auth", None(), headers);

  AWAIT_READY(wrongAuthFuture);
  EXPECT_EQ(http::statuses[401], wrongAuthFuture.get().status);
  ASSERT_SOME_EQ("Basic realm=\"testrealm\"",
                 wrongAuthFuture.get().headers.get("WWW-authenticate"));

  // Now test passing right auth header.
  headers["Authorization"] = "Basic " + base64::encode("testuser:testpass");

  Future<http::Response> rightAuthFuture =
    http::get(http.process->self(), "auth", None(), headers);

  AWAIT_READY(rightAuthFuture);
  EXPECT_EQ(http::statuses[200], rightAuthFuture.get().status);
}


TEST(HTTPTest, Endpoints)
{
  Http http;

  // First hit '/body' (using explicit sockets and HTTP/1.0).
  Try<Socket> create = Socket::create();
  ASSERT_SOME(create);

  Socket socket = create.get();

  AWAIT_READY(socket.connect(http.process->self().address));

  std::ostringstream out;
  out << "GET /" << http.process->self().id << "/body"
      << " HTTP/1.0\r\n"
      << "Connection: Keep-Alive\r\n"
      << "\r\n";

  const string data = out.str();

  EXPECT_CALL(*http.process, body(_))
    .WillOnce(Return(http::OK()));

  AWAIT_READY(socket.send(data));

  string response = "HTTP/1.1 200 OK";

  AWAIT_EXPECT_EQ(response, socket.recv(response.size()));

  // Now hit '/pipe' (by using http::get).
  http::Pipe pipe;
  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  Future<Nothing> request;
  EXPECT_CALL(*http.process, pipe(_))
    .WillOnce(DoAll(FutureSatisfy(&request),
                    Return(ok)));

  Future<http::Response> future = http::get(http.process->self(), "pipe");

  AWAIT_READY(request);

  // Write the response.
  http::Pipe::Writer writer = pipe.writer();
  EXPECT_TRUE(writer.write("Hello World\n"));
  EXPECT_TRUE(writer.close());

  AWAIT_READY(future);
  EXPECT_EQ(http::statuses[200], future.get().status);
  EXPECT_SOME_EQ("chunked", future.get().headers.get("Transfer-Encoding"));
  EXPECT_EQ("Hello World\n", future.get().body);
}


TEST(HTTPTest, PipeEOF)
{
  http::Pipe pipe;
  http::Pipe::Reader reader = pipe.reader();
  http::Pipe::Writer writer = pipe.writer();

  // A 'read' on an empty pipe should block.
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Writing an empty string should have no effect.
  EXPECT_TRUE(writer.write(""));
  EXPECT_TRUE(read.isPending());

  // After a 'write' the pending 'read' should complete.
  EXPECT_TRUE(writer.write("hello"));
  ASSERT_TRUE(read.isReady());
  EXPECT_EQ("hello", read.get());

  // After a 'write' a call to 'read' should be completed immediately.
  ASSERT_TRUE(writer.write("world"));

  read = reader.read();
  ASSERT_TRUE(read.isReady());
  EXPECT_EQ("world", read.get());

  // Close the write end of the pipe and ensure the remaining
  // data can be read.
  EXPECT_TRUE(writer.write("!"));
  EXPECT_TRUE(writer.close());
  AWAIT_EQ("!", reader.read());

  // End of file should be reached.
  AWAIT_EQ("", reader.read());
  AWAIT_EQ("", reader.read());

  // Writes to a pipe with the write end closed are ignored.
  EXPECT_FALSE(writer.write("!"));
  AWAIT_EQ("", reader.read());

  // The write end cannot be closed twice.
  EXPECT_FALSE(writer.close());

  // Close the read end, this should not notify the writer
  // since the write end was already closed.
  EXPECT_TRUE(reader.close());
  EXPECT_TRUE(writer.readerClosed().isPending());
}


TEST(HTTPTest, PipeFailure)
{
  http::Pipe pipe;
  http::Pipe::Reader reader = pipe.reader();
  http::Pipe::Writer writer = pipe.writer();

  // Fail the writer after writing some data.
  EXPECT_TRUE(writer.write("hello"));
  EXPECT_TRUE(writer.write("world"));

  EXPECT_TRUE(writer.fail("disconnected!"));

  // The reader should read the data, followed by the failure.
  AWAIT_EQ("hello", reader.read());
  AWAIT_EQ("world", reader.read());

  Future<string> read = reader.read();
  EXPECT_TRUE(read.isFailed());
  EXPECT_EQ("disconnected!", read.failure());

  // The writer cannot close or fail an already failed pipe.
  EXPECT_FALSE(writer.close());
  EXPECT_FALSE(writer.fail("not again"));

  // The writer shouldn't be notified of the reader closing,
  // since the writer had already failed.
  EXPECT_TRUE(reader.close());
  EXPECT_TRUE(writer.readerClosed().isPending());
}



TEST(HTTPTest, PipeReaderCloses)
{
  http::Pipe pipe;
  http::Pipe::Reader reader = pipe.reader();
  http::Pipe::Writer writer = pipe.writer();

  // If the read end of the pipe is closed,
  // it should discard any unread data.
  EXPECT_TRUE(writer.write("hello"));
  EXPECT_TRUE(writer.write("world"));

  // The writer should discover the closure.
  Future<Nothing> closed = writer.readerClosed();
  EXPECT_TRUE(reader.close());
  EXPECT_TRUE(closed.isReady());

  // The read end is closed, subsequent reads will fail.
  AWAIT_FAILED(reader.read());

  // The read end is closed, writes are ignored.
  EXPECT_FALSE(writer.write("!"));
  AWAIT_FAILED(reader.read());

  // The read end cannot be closed twice.
  EXPECT_FALSE(reader.close());

  // Close the write end.
  EXPECT_TRUE(writer.close());

  // Reads should fail since the read end is closed.
  AWAIT_FAILED(reader.read());
}


TEST(HTTPTest, Encode)
{
  string unencoded = "a$&+,/:;=?@ \"<>#%{}|\\^~[]`\x19\x80\xFF";
  unencoded += string("\x00", 1); // Add a null byte to the end.

  string encoded = http::encode(unencoded);

  EXPECT_EQ("a%24%26%2B%2C%2F%3A%3B%3D%3F%40%20%22%3C%3E%23"
            "%25%7B%7D%7C%5C%5E%7E%5B%5D%60%19%80%FF%00",
            encoded);

  EXPECT_SOME_EQ(unencoded, http::decode(encoded));

  encoded = "a%24%26%2B%2C%2F%3A%3B%3D%3F%40+%22%3C%3E%23"
            "%25%7B%7D%7C%5C%5E%7E%5B%5D%60%19%80%FF%00";
  EXPECT_SOME_EQ(unencoded, http::decode(encoded));

  EXPECT_ERROR(http::decode("%"));
  EXPECT_ERROR(http::decode("%1"));
  EXPECT_ERROR(http::decode("%;1"));
  EXPECT_ERROR(http::decode("%1;"));
}


TEST(HTTPTest, PathParse)
{
  const string pattern = "/books/{isbn}/chapters/{chapter}";

  Try<hashmap<string, string> > parse =
    http::path::parse(pattern, "/books/0304827484/chapters/3");

  ASSERT_SOME(parse);
  EXPECT_EQ(4, parse.get().size());
  EXPECT_SOME_EQ("books", parse.get().get("books"));
  EXPECT_SOME_EQ("0304827484", parse.get().get("isbn"));
  EXPECT_SOME_EQ("chapters", parse.get().get("chapters"));
  EXPECT_SOME_EQ("3", parse.get().get("chapter"));

  parse = http::path::parse(pattern, "/books/0304827484");

  ASSERT_SOME(parse);
  EXPECT_EQ(2, parse.get().size());
  EXPECT_SOME_EQ("books", parse.get().get("books"));
  EXPECT_SOME_EQ("0304827484", parse.get().get("isbn"));

  parse = http::path::parse(pattern, "/books/0304827484/chapters");

  ASSERT_SOME(parse);
  EXPECT_EQ(3, parse.get().size());
  EXPECT_SOME_EQ("books", parse.get().get("books"));
  EXPECT_SOME_EQ("0304827484", parse.get().get("isbn"));
  EXPECT_SOME_EQ("chapters", parse.get().get("chapters"));

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
  EXPECT_NE(process::address(), request.client);
  EXPECT_EQ("GET", request.method);
  EXPECT_THAT(request.url.path, EndsWith("get"));
  EXPECT_EQ("", request.body);
  EXPECT_NONE(request.url.fragment);
  EXPECT_TRUE(request.url.query.empty());

  return http::OK();
}


http::Response validateGetWithQuery(const http::Request& request)
{
  EXPECT_NE(process::address(), request.client);
  EXPECT_EQ("GET", request.method);
  EXPECT_THAT(request.url.path, EndsWith("get"));
  EXPECT_EQ("", request.body);
  EXPECT_NONE(request.url.fragment);
  EXPECT_EQ("bar", request.url.query.at("foo"));
  EXPECT_EQ(1, request.url.query.size());

  return http::OK();
}


TEST(HTTPTest, Get)
{
  Http http;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(Invoke(validateGetWithoutQuery));

  Future<http::Response> noQueryFuture = http::get(http.process->self(), "get");

  AWAIT_READY(noQueryFuture);
  ASSERT_EQ(http::statuses[200], noQueryFuture.get().status);

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(Invoke(validateGetWithQuery));

  Future<http::Response> queryFuture =
    http::get(http.process->self(), "get", "foo=bar");

  AWAIT_READY(queryFuture);
  ASSERT_EQ(http::statuses[200], queryFuture.get().status);
}


TEST(HTTPTest, NestedGet)
{
  Http http;

  EXPECT_CALL(*http.process, a(_))
    .WillOnce(Return(http::Accepted()));

  EXPECT_CALL(*http.process, abc(_))
    .WillOnce(Return(http::OK()));

  // The handler for "/a/b/c" should return 'http::OK()'.
  Future<http::Response> response = http::get(http.process->self(), "/a/b/c");

  AWAIT_READY(response);
  ASSERT_EQ(http::statuses[200], response.get().status);

  // "/a/b" should be handled by "/a" handler and return
  // 'http::Accepted()'.
  response = http::get(http.process->self(), "/a/b");

  AWAIT_READY(response);
  ASSERT_EQ(http::statuses[202], response.get().status);
}


TEST(HTTPTest, StreamingGetComplete)
{
  Http http;

  http::Pipe pipe;
  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  EXPECT_CALL(*http.process, pipe(_))
    .WillOnce(Return(ok));

  Future<http::Response> response =
    http::streaming::get(http.process->self(), "pipe");

  // The response should be ready since the headers were sent.
  AWAIT_READY(response);

  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(http::Response::PIPE, response.get().type);
  ASSERT_SOME(response.get().reader);

  http::Pipe::Reader reader = response.get().reader.get();

  // There is no data to read yet.
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Stream data into the body and read it from the response.
  http::Pipe::Writer writer = pipe.writer();
  EXPECT_TRUE(writer.write("hello"));
  AWAIT_EQ("hello", read);

  EXPECT_TRUE(writer.write("goodbye"));
  AWAIT_EQ("goodbye", reader.read());

  // Complete the response.
  EXPECT_TRUE(writer.close());
  AWAIT_EQ("", reader.read()); // EOF.
}


TEST(HTTPTest, StreamingGetFailure)
{
  Http http;

  http::Pipe pipe;
  http::OK ok;
  ok.type = http::Response::PIPE;
  ok.reader = pipe.reader();

  EXPECT_CALL(*http.process, pipe(_))
    .WillOnce(Return(ok));

  Future<http::Response> response =
    http::streaming::get(http.process->self(), "pipe");

  // The response should be ready since the headers were sent.
  AWAIT_READY(response);

  EXPECT_SOME_EQ("chunked", response.get().headers.get("Transfer-Encoding"));
  ASSERT_EQ(http::Response::PIPE, response.get().type);
  ASSERT_SOME(response.get().reader);

  http::Pipe::Reader reader = response.get().reader.get();

  // There is no data to read yet.
  Future<string> read = reader.read();
  EXPECT_TRUE(read.isPending());

  // Stream data into the body and read it from the response.
  http::Pipe::Writer writer = pipe.writer();
  EXPECT_TRUE(writer.write("hello"));
  AWAIT_EQ("hello", read);

  EXPECT_TRUE(writer.write("goodbye"));
  AWAIT_EQ("goodbye", reader.read());

  // Fail the response.
  EXPECT_TRUE(writer.fail("oops"));
  AWAIT_FAILED(reader.read());
}


TEST(HTTPTest, PipeEquality)
{
  // Pipes are shared objects, like Futures. Copies are considered
  // equal as they point to the same underlying object.
  http::Pipe pipe1;
  http::Pipe copy = pipe1;

  EXPECT_EQ(pipe1, copy);

  http::Pipe pipe2;
  EXPECT_NE(pipe2, pipe1);

  EXPECT_EQ(pipe1.reader(), pipe1.reader());
  EXPECT_EQ(pipe1.writer(), pipe1.writer());

  EXPECT_NE(pipe1.reader(), pipe2.reader());
  EXPECT_NE(pipe1.writer(), pipe2.writer());
}


http::Response validatePost(const http::Request& request)
{
  EXPECT_EQ("POST", request.method);
  EXPECT_THAT(request.url.path, EndsWith("post"));
  EXPECT_EQ("This is the payload.", request.body);
  EXPECT_NONE(request.url.fragment);
  EXPECT_TRUE(request.url.query.empty());

  return http::OK();
}


TEST(HTTPTest, Post)
{
  Http http;

  // Test the case where there is a content type but no body.
  Future<http::Response> future =
    http::post(http.process->self(), "post", None(), None(), "text/plain");

  AWAIT_EXPECT_FAILED(future);

  EXPECT_CALL(*http.process, post(_))
    .WillOnce(Invoke(validatePost));

  future = http::post(
      http.process->self(),
      "post",
      None(),
      "This is the payload.",
      "text/plain");

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);

  // Now test passing headers instead.
  http::Headers headers;
  headers["Content-Type"] = "text/plain";

  EXPECT_CALL(*http.process, post(_))
    .WillOnce(Invoke(validatePost));

  future =
    http::post(http.process->self(), "post", headers, "This is the payload.");

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);
}


http::Response validateDelete(const http::Request& request)
{
  EXPECT_EQ("DELETE", request.method);
  EXPECT_THAT(request.url.path, EndsWith("delete"));
  EXPECT_TRUE(request.body.empty());
  EXPECT_TRUE(request.url.query.empty());

  return http::OK();
}


TEST(HTTPTest, Delete)
{
  Http http;

  EXPECT_CALL(*http.process, requestDelete(_))
    .WillOnce(Invoke(validateDelete));

  Future<http::Response> future =
    http::requestDelete(http.process->self(), "delete", None());

  AWAIT_READY(future);
  ASSERT_EQ(http::statuses[200], future.get().status);
}


TEST(HTTPConnectionTest, Serial)
{
  Http http;

  http::URL url = http::URL(
      "http",
      http.process->self().address.ip,
      http.process->self().address.port,
      http.process->self().id + "/get");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection = connect.get();

  // First test a regular (non-streaming) request.
  Promise<http::Response> promise1;
  Future<http::Request> get1;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(DoAll(FutureArg<0>(&get1), Return(promise1.future())));

  http::Request request1;
  request1.method = "GET";
  request1.url = url;
  request1.body = "1";
  request1.keepAlive = true;

  Future<http::Response> response1 = connection.send(request1);

  AWAIT_READY(get1);
  EXPECT_EQ("1", get1->body);

  promise1.set(http::OK("1"));

  AWAIT_EXPECT_RESPONSE_BODY_EQ("1", response1);

  // Now test a streaming response.
  Promise<http::Response> promise2;
  Future<http::Request> get2;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(DoAll(FutureArg<0>(&get2), Return(promise2.future())));

  http::Request request2 = request1;
  request2.body = "2";

  Future<http::Response> response2 = connection.send(request2, true);

  AWAIT_READY(get2);
  EXPECT_EQ("2", get2->body);

  promise2.set(http::OK("2"));

  AWAIT_READY(response2);
  ASSERT_SOME(response2->reader);

  http::Pipe::Reader reader = response2->reader.get();
  AWAIT_EQ("2", reader.read());
  AWAIT_EQ("", reader.read());

  // Disconnect.
  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  // After disconnection, sends should fail.
  AWAIT_FAILED(connection.send(request1));
}


TEST(HTTPConnectionTest, Pipeline)
{
  Http http;

  http::URL url = http::URL(
      "http",
      http.process->self().address.ip,
      http.process->self().address.port,
      http.process->self().id + "/get");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection = connect.get();

  // Send three pipelined requests.
  Promise<http::Response> promise1, promise2, promise3;
  Future<http::Request> get1, get2, get3;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(DoAll(FutureArg<0>(&get1),
                    Return(promise1.future())))
    .WillOnce(DoAll(FutureArg<0>(&get2),
                    Return(promise2.future())))
    .WillOnce(DoAll(FutureArg<0>(&get3),
                    Return(promise3.future())));

  http::Request request1, request2, request3;

  request1.method = "GET";
  request2.method = "GET";
  request3.method = "GET";

  request1.url = url;
  request2.url = url;
  request3.url = url;

  request1.body = "1";
  request2.body = "2";
  request3.body = "3";

  request1.keepAlive = true;
  request2.keepAlive = true;
  request3.keepAlive = true;

  Future<http::Response> response1 = connection.send(request1);
  Future<http::Response> response2 = connection.send(request2, true);
  Future<http::Response> response3 = connection.send(request3);

  // Ensure the requests are all received before any
  // responses have been sent.
  AWAIT_READY(get1);
  AWAIT_READY(get2);
  AWAIT_READY(get3);

  EXPECT_EQ("1", get1->body);
  EXPECT_EQ("2", get2->body);
  EXPECT_EQ("3", get3->body);

  // Complete the responses in the opposite order, and ensure
  // that the pipelining in libprocess sends the responses in
  // the same order as the requests were received.
  promise3.set(http::OK("3"));
  promise2.set(http::OK("2"));

  EXPECT_TRUE(response1.isPending());
  EXPECT_TRUE(response2.isPending());
  EXPECT_TRUE(response3.isPending());

  promise1.set(http::OK("1"));

  AWAIT_READY(response1);
  AWAIT_READY(response2);
  AWAIT_READY(response3);

  EXPECT_EQ("1", response1->body);

  ASSERT_SOME(response2->reader);

  http::Pipe::Reader reader = response2->reader.get();
  AWAIT_EQ("2", reader.read());
  AWAIT_EQ("", reader.read());

  EXPECT_EQ("3", response3->body);

  // Disconnect.
  AWAIT_READY(connection.disconnect());
  AWAIT_READY(connection.disconnected());

  // After disconnection, sends should fail.
  AWAIT_FAILED(connection.send(request1));
}


TEST(HTTPConnectionTest, ClosingRequest)
{
  Http http;

  http::URL url = http::URL(
      "http",
      http.process->self().address.ip,
      http.process->self().address.port,
      http.process->self().id + "/get");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection = connect.get();

  // Issue two pipelined requests, the second will not have
  // 'keepAlive' set. This prevents further requests and leads
  // to a disconnection upon receiving the second response.
  Promise<http::Response> promise1, promise2;
  Future<http::Request> get1, get2;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(DoAll(FutureArg<0>(&get1),
                    Return(promise1.future())))
    .WillOnce(DoAll(FutureArg<0>(&get2),
                    Return(promise2.future())));

  http::Request request1, request2;

  request1.method = "GET";
  request2.method = "GET";

  request1.url = url;
  request2.url = url;

  request1.keepAlive = true;
  request2.keepAlive = false;

  Future<http::Response> response1 = connection.send(request1);
  Future<http::Response> response2 = connection.send(request2);

  // After a closing request, sends should fail.
  AWAIT_FAILED(connection.send(request1));

  // Complete the responses.
  promise1.set(http::OK("body"));
  promise2.set(http::OK("body"));

  AWAIT_READY(response1);
  AWAIT_READY(response2);

  AWAIT_READY(connection.disconnected());
}


TEST(HTTPConnectionTest, ClosingResponse)
{
  Http http;

  http::URL url = http::URL(
      "http",
      http.process->self().address.ip,
      http.process->self().address.port,
      http.process->self().id + "/get");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection = connect.get();

  // Issue two pipelined requests, the server will respond
  // with a 'Connection: close' for the first response, which
  // will lead to a disconnection.
  Promise<http::Response> promise1, promise2;
  Future<http::Request> get1, get2;

  EXPECT_CALL(*http.process, get(_))
    .WillOnce(DoAll(FutureArg<0>(&get1), Return(promise1.future())))
    .WillOnce(DoAll(FutureArg<0>(&get2), Return(promise2.future())));

  http::Request request1, request2;

  request1.method = "GET";
  request2.method = "GET";

  request1.url = url;
  request2.url = url;

  request1.keepAlive = true;
  request2.keepAlive = true;

  Future<http::Response> response1 = connection.send(request1);
  Future<http::Response> response2 = connection.send(request2);

  // The first response will close the connection.
  http::Response ok = http::OK("body");
  ok.headers["Connection"] = "close";

  promise1.set(ok);

  AWAIT_READY(response1);
  AWAIT_FAILED(response2);

  AWAIT_READY(connection.disconnected());
}


TEST(HTTPConnectionTest, ReferenceCounting)
{
  Http http;

  http::URL url = http::URL(
      "http",
      http.process->self().address.ip,
      http.process->self().address.port,
      http.process->self().id + "/get");

  // Capture the connection as a Owned in order to test that
  // when the last copy of the Connection is destructed, a
  // disconnection occurs.
  auto connect = Owned<Future<http::Connection>>(
      new Future<http::Connection>(http::connect(url)));

  AWAIT_READY(*connect);

  auto connection = Owned<http::Connection>(
      new http::Connection(connect->get()));

  connect.reset();

  Future<Nothing> disconnected = connection->disconnected();

  // This should be the last remaining copy of the connection.
  connection.reset();

  AWAIT_READY(disconnected);
}


TEST(HTTPConnectionTest, Equality)
{
  Http http;

  http::URL url = http::URL(
      "http",
      http.process->self().address.ip,
      http.process->self().address.port,
      http.process->self().id + "/get");

  Future<http::Connection> connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection1 = connect.get();

  connect = http::connect(url);
  AWAIT_READY(connect);

  http::Connection connection2 = connect.get();

  EXPECT_NE(connection1, connection2);
  EXPECT_EQ(connection2, connection2);
}


TEST(HTTPTest, QueryEncodeDecode)
{
  // If we use Type<a, b> directly inside a macro without surrounding
  // parenthesis the comma will be eaten by the macro rather than the
  // template. Typedef to avoid the problem.
  typedef hashmap<string, string> HashmapStringString;

  EXPECT_EQ("",
            http::query::encode(HashmapStringString({})));

  EXPECT_EQ("foo=bar",
            http::query::encode(HashmapStringString({{"foo", "bar"}})));

  EXPECT_EQ("c%7E%2Fasdf=%25asdf&a()=b%2520",
            http::query::encode(
                HashmapStringString({{"a()", "b%20"}, {"c~/asdf", "%asdf"}})));

  EXPECT_EQ("d",
            http::query::encode(HashmapStringString({{"d", ""}})));

  EXPECT_EQ("a%26b%3Dc=d%26e%3Dfg",
            http::query::encode(HashmapStringString({{"a&b=c", "d&e=fg"}})));

  // Explicitly not testing decoding failures.
  EXPECT_SOME_EQ(HashmapStringString(),
                 http::query::decode(""));

  EXPECT_SOME_EQ(HashmapStringString({{"foo", "bar"}}),
                 http::query::decode("foo=bar"));

  EXPECT_SOME_EQ(HashmapStringString({{"a()", "b%20"}, {"c~/asdf", "%asdf"}}),
                 http::query::decode("c%7E%2Fasdf=%25asdf&a()=b%2520"));

  EXPECT_SOME_EQ(HashmapStringString({{"d", ""}}),
                 http::query::decode("d"));

  EXPECT_SOME_EQ(HashmapStringString({{"a&b=c", "d&e=fg"}}),
                 http::query::decode("a%26b%3Dc=d%26e%3Dfg"));
}


TEST(HTTPTest, CaseInsensitiveHeaders)
{
  http::Request request;
  request.headers["Content-Length"] = "20";
  EXPECT_EQ("20", request.headers["Content-Length"]);
  EXPECT_EQ("20", request.headers["CONTENT-LENGTH"]);
  EXPECT_EQ("20", request.headers["content-length"]);

  request.headers["content-length"] = "30";
  EXPECT_EQ("30", request.headers["content-length"]);
  EXPECT_EQ("30", request.headers["Content-Length"]);
  EXPECT_EQ("30", request.headers["CONTENT-LENGTH"]);

  http::Response response;
  response.headers["Content-Type"] = "application/json";
  EXPECT_EQ("application/json", response.headers["Content-Type"]);
  EXPECT_EQ("application/json", response.headers["content-type"]);
  EXPECT_EQ("application/json", response.headers["CONTENT-TYPE"]);

  response.headers["content-type"] = "text/javascript";
  EXPECT_EQ("text/javascript", response.headers["content-type"]);
  EXPECT_EQ("text/javascript", response.headers["Content-Type"]);
  EXPECT_EQ("text/javascript", response.headers["CONTENT-TYPE"]);
}


TEST(HTTPTest, Accepts)
{
  // Create requests that do not accept the 'text/*' media type.
  vector<string> headers = {
    "text/*;q=0.0",
    "text/html;q=0.0",
    "text/",
    "text",
    "foo/*",
    "foo/*, text/*;q=0.0",
    "foo/*,\ttext/*;q=0.0",
    "*/*, text/*;q=0.0",
    "*/*;q=0.0, foo",
    "textttt/*"
  };

  foreach (const string& accept, headers) {
    http::Request request;
    request.headers["Accept"] = accept;

    EXPECT_FALSE(request.acceptsMediaType("text/*"))
      << "Not expecting " << accept << " to match 'text/*'";

    EXPECT_FALSE(request.acceptsMediaType("text/html"))
      << "Not expecting " << accept << " to match 'text/html'";
  }

  // Create requests that accept 'text/html' media type.
  headers = {
    "text/*",
    "text/*;q=0.1",
    "text/html",
    "text/html;q=0.1",
    "text/bar, text/html,q=0.1",
    "*/*, text/bar;q=0.5",
    "*/*;q=0.9, text/foo",
    "text/foo,\ttext/*;q=0.1",
    "*/*",
    "*/*, text/bar",
    "*/*, foo/*"
  };

  foreach (const string& accept, headers) {
    http::Request request;
    request.headers["Accept"] = accept;

    EXPECT_TRUE(request.acceptsMediaType("text/html"))
      << "Expecting '" << accept << "' to match 'text/html'";
  }

  // Missing header should accept all media types.
  http::Request empty;
  EXPECT_TRUE(empty.acceptsMediaType("text/html"));
}


// TODO(evelinad): Add URLTest for IPv6.
TEST(URLTest, Stringification)
{
  EXPECT_EQ("http://mesos.apache.org:80/",
            stringify(URL("http", "mesos.apache.org")));

  EXPECT_EQ("https://mesos.apache.org:8080/",
            stringify(URL("https", "mesos.apache.org", 8080)));

  Try<net::IP> ip = net::IP::parse("172.158.1.23", AF_INET);
  ASSERT_SOME(ip);

  EXPECT_EQ("http://172.158.1.23:8080/",
            stringify(URL("http", ip.get(), 8080)));

  EXPECT_EQ("http://172.158.1.23:80/path",
            stringify(URL("http", ip.get(), 80, "/path")));

  hashmap<string, string> query;
  query["foo"] = "bar";
  query["baz"] = "bam";

  // The order of the hashmap entries may vary, hence we have
  // to check if one of the possible outcomes is satisfied.
  const string url1 = stringify(URL("http", ip.get(), 80, "/", query));

  EXPECT_TRUE(url1 == "http://172.158.1.23:80/?baz=bam&foo=bar" ||
              url1 == "http://172.158.1.23:80/?foo=bar&baz=bam");

  const string url2 = stringify(URL("http", ip.get(), 80, "/path", query));

  EXPECT_TRUE(url2 == "http://172.158.1.23:80/path?baz=bam&foo=bar" ||
              url2 == "http://172.158.1.23:80/path?foo=bar&baz=bam");

  const string url3 =
    stringify(URL("http", ip.get(), 80, "/", query, "fragment"));

  EXPECT_TRUE(url3 == "http://172.158.1.23:80/?baz=bam&foo=bar#fragment" ||
              url3 == "http://172.158.1.23:80/?foo=bar&baz=bam#fragment");

  const string url4 =
    stringify(URL("http", ip.get(), 80, "/path", query, "fragment"));

  EXPECT_TRUE(url4 == "http://172.158.1.23:80/path?baz=bam&foo=bar#fragment" ||
              url4 == "http://172.158.1.23:80/path?foo=bar&baz=bam#fragment");
}

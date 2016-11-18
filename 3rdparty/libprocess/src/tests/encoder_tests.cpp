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
#include <vector>

#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/gtest.hpp>

#include "encoder.hpp"
#include "decoder.hpp"

namespace http = process::http;

using process::HttpResponseEncoder;
using process::Owned;
using process::ResponseDecoder;

using std::deque;
using std::string;
using std::vector;


TEST(EncoderTest, Response)
{
  http::Request request;
  const http::OK response("body");

  // Encode the response.
  const string encoded = HttpResponseEncoder::encode(response, request);

  // Now decode it back, and verify the encoding was correct.
  ResponseDecoder decoder;
  deque<http::Response*> responses =
    decoder.decode(encoded.data(), encoded.length());

  ASSERT_FALSE(decoder.failed());
  ASSERT_EQ(1u, responses.size());

  Owned<http::Response> decoded(responses[0]);
  EXPECT_EQ("200 OK", decoded->status);
  EXPECT_EQ("body", decoded->body);

  // Encoding should have inserted the 'Date', 'Content-Length' and
  // 'Content-Type' headers.
  EXPECT_EQ(3u, decoded->headers.size());
  EXPECT_TRUE(decoded->headers.contains("Date"));
  EXPECT_SOME_EQ(
      stringify(response.body.size()),
      decoded->headers.get("Content-Length"));
  EXPECT_SOME_EQ(
      "text/plain; charset=utf-8",
      decoded->headers.get("Content-Type"));
}


TEST(EncoderTest, AcceptableEncodings)
{
  // Create requests that do not accept gzip encoding.
  vector<http::Request> requests(10);
  requests[0].headers["Accept-Encoding"] = "gzip;q=0.0,*";
  requests[1].headers["Accept-Encoding"] = "compress";
  requests[2].headers["Accept-Encoding"] = "compress, gzip;q=0.0";
  requests[3].headers["Accept-Encoding"] = "*, gzip;q=0.0";
  requests[4].headers["Accept-Encoding"] = "*;q=0.0, compress";
  requests[5].headers["Accept-Encoding"] = "\n compress";
  requests[6].headers["Accept-Encoding"] = "compress,\tgzip;q=0.0";
  requests[7].headers["Accept-Encoding"] = "gzipbug;q=0.1";
  requests[8].headers["Accept-Encoding"] = "";
  requests[9].headers["Accept-Encoding"] = ",";

  foreach (const http::Request& request, requests) {
    EXPECT_FALSE(request.acceptsEncoding("gzip"))
      << "Gzip encoding is unacceptable for 'Accept-Encoding: "
      << request.headers.get("Accept-Encoding").get() << "'";
  }

  // Create requests that accept gzip encoding.
  vector<http::Request> gzipRequests(12);

  // Using q values.
  gzipRequests[0].headers["Accept-Encoding"] = "gzip;q=0.1,*";
  gzipRequests[1].headers["Accept-Encoding"] = "compress, gzip;q=0.1";
  gzipRequests[2].headers["Accept-Encoding"] = "*, gzip;q=0.5";
  gzipRequests[3].headers["Accept-Encoding"] = "*;q=0.9, compress";
  gzipRequests[4].headers["Accept-Encoding"] = "compress,\tgzip;q=0.1";

  // No q values.
  gzipRequests[5].headers["Accept-Encoding"] = "gzip";
  gzipRequests[6].headers["Accept-Encoding"] = "compress, gzip";
  gzipRequests[7].headers["Accept-Encoding"] = "*";
  gzipRequests[8].headers["Accept-Encoding"] = "*, compress";
  gzipRequests[9].headers["Accept-Encoding"] = "\n gzip";
  gzipRequests[10].headers["Accept-Encoding"] = "compress,\tgzip";
  gzipRequests[11].headers["Accept-Encoding"] = "gzip";

  foreach (const http::Request& gzipRequest, gzipRequests) {
    EXPECT_TRUE(gzipRequest.acceptsEncoding("gzip"))
      << "Gzip encoding is acceptable for 'Accept-Encoding: "
      << gzipRequest.headers.get("Accept-Encoding").get() << "'";
  }
}

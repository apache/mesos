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

#ifndef __MESOS_HTTP_HPP__
#define __MESOS_HTTP_HPP__

#include <iosfwd>

namespace mesos {

const char APPLICATION_PROTOBUF[] = "application/x-protobuf";
const char APPLICATION_JSON[] = "application/json";


// The content-type used for streaming requests/responses.
const char APPLICATION_RECORDIO[] = "application/recordio";


// The header used by the client/server to specify the content-type
// of messages in a streaming request/response.
const char MESSAGE_CONTENT_TYPE[] = "Message-Content-Type";


// The header used by the client to specify acceptable content-type
// of messages in a streaming response.
const char MESSAGE_ACCEPT[] = "Message-Accept";


// Possible content-types that can be used for requests to the Mesos HTTP API.
enum class ContentType
{
  PROTOBUF,
  JSON,
  RECORDIO
};


std::ostream& operator<<(std::ostream& stream, ContentType contentType);

} // namespace mesos {

#endif // __MESOS_HTTP_HPP__

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
// limitations under the License

#ifndef __HDFS_HPP__
#define __HDFS_HPP__

#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/bytes.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <mesos/uri/uri.hpp>


// TODO(benh): We should get the hostname:port (or ip:port) of the
// server via:
//
//  hadoop dfsadmin -report | grep Name: | awk '{ print $2 }'
//
// The advantage of doing this is then we can explicitly use the
// 'hdfs://hostname' prefix when we're trying to do copies to avoid
// silent failures when HDFS is down and the tools just copies
// locally.
//
// Note that if HDFS is not on port 9000 then we'll also need to do an
// HTTP GET on hostname:port and grab the information in the
// <title>...</title> (this is the best hack I can think of to get
// 'fs.default.name' given the tools available).
class HDFS
{
public:
  static Try<process::Owned<HDFS>> create(
      const Option<std::string>& hadoop = None());

  // TODO(gilbert): Remove this helper function once we have URI Parser
  // support (see MESOS-5254 for details). Ideally, we should support
  // other schemes (e.g., hftp, s3, s3n etc) with hadoop plugin. It is
  // hard coded for HDFS for now.
  static Try<mesos::URI> parse(const std::string& uri);

  process::Future<bool> exists(const std::string& path);
  process::Future<Bytes> du(const std::string& path);
  process::Future<Nothing> rm(const std::string& path);

  process::Future<Nothing> copyFromLocal(
      const std::string& from,
      const std::string& to);

  process::Future<Nothing> copyToLocal(
      const std::string& from,
      const std::string& to);

private:
  explicit HDFS(const std::string& _hadoop)
    : hadoop(_hadoop) {}

  const std::string hadoop;
};

#endif // __HDFS_HPP__

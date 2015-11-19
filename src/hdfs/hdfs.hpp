/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License
 */

#ifndef __HDFS_HPP__
#define __HDFS_HPP__

#include <string>

#include <stout/bytes.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>


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
  // Look for `hadoop' first where proposed, otherwise, look for
  // HADOOP_HOME, otherwise, assume it's on the PATH.
  explicit HDFS(const std::string& _hadoop);

  // Look for `hadoop' in HADOOP_HOME or assume it's on the PATH.
  HDFS();

  // Check if hadoop client is available at the path that was set.
  // This can be done by executing `hadoop version` command and
  // checking for status code == 0.
  Try<bool> available();

  Try<bool> exists(const std::string& path);
  Try<Bytes> du(const std::string& path);
  Try<Nothing> rm(const std::string& path);
  Try<Nothing> copyFromLocal(const std::string& from, const std::string& to);
  Try<Nothing> copyToLocal(const std::string& from, const std::string& to);

private:
  // Normalize an HDFS path such that it is either an absolute path or
  // a full hdfs:// URL.
  std::string absolutePath(const std::string& hdfsPath);

  const std::string hadoop;
};

#endif // __HDFS_HPP__

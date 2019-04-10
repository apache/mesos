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

#ifndef __URI_FETCHERS_HADOOP_HPP__
#define __URI_FETCHERS_HADOOP_HPP__

#include <set>
#include <string>

#include <process/owned.hpp>

#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <mesos/uri/fetcher.hpp>

#include "hdfs/hdfs.hpp"

namespace mesos {
namespace uri {

class HadoopFetcherPlugin : public Fetcher::Plugin
{
public:
  class Flags : public virtual flags::FlagsBase
  {
  public:
    Flags();

    Option<std::string> hadoop_client;
    std::string hadoop_client_supported_schemes;
  };

  static const char NAME[];

  static Try<process::Owned<Fetcher::Plugin>> create(const Flags& flags);

  ~HadoopFetcherPlugin() override {}

  std::set<std::string> schemes() const override;

  std::string name() const override;

  process::Future<Nothing> fetch(
      const URI& uri,
      const std::string& directory,
      const Option<std::string>& data = None(),
      const Option<std::string>& outputFileName = None()) const override;

private:
  HadoopFetcherPlugin(
      process::Owned<HDFS> _hdfs,
      const std::set<std::string>& _schemes)
    : hdfs(_hdfs),
      schemes_(_schemes) {}

  process::Owned<HDFS> hdfs;
  std::set<std::string> schemes_;
};

} // namespace uri {
} // namespace mesos {

#endif // __URI_FETCHERS_HADOOP_HPP__

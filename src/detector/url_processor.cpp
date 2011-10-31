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
 * limitations under the License.
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <glog/logging.h>

#include "common/option.hpp"

#include "detector/url_processor.hpp"

using namespace std;


namespace mesos {
namespace internal {

static Option<string> parseLabel(const string& label, const string& line)
{
  size_t index = line.find(label);
  if (index != string::npos) {
    string labeled = line.substr(index + label.size(), 1024);
    stringstream trimmer;
    trimmer << labeled;
    trimmer >> labeled;
    if (!labeled.empty()) {
      return Option<string>::some(labeled);
    }
  }
  return Option<string>::none();
}


string UrlProcessor::parseZooFile(const string &zooFilename)
{
  string zoos = "";

  LOG(INFO) << "Opening ZooFile: " << zooFilename;
  ifstream zoofile(zooFilename.c_str());
  if (!zoofile) {
    LOG(ERROR) << "ZooFile " << zooFilename << " could not be opened";
  }
  string auth = "";
  string znode = "";
  while (!zoofile.eof()) {
    string line;
    getline(zoofile, line);
    if (line == "") {
      continue;
    }
    Option<string> credentials = parseLabel("[auth]", line);
    if (credentials.isSome()) {
      CHECK(auth.empty()) << "ZooFile " << zooFilename
                          << " has multiple [auth] lines, can only have 1";

      auth = credentials.get() + "@";
      continue;
    }
    Option<string> chroot = parseLabel("[znode]", line);
    if (chroot.isSome()) {
      CHECK(znode.empty()) << "ZooFile " << zooFilename
                           << " has multiple [znode] lines, can only have 1";

      znode = chroot.get();
      continue;
    }
    if (zoos != "") {
      zoos += ',';
    }
    zoos += line;
  }
  zoos = auth + zoos + znode;
  remove_if(zoos.begin(), zoos.end(), (int (*)(int)) isspace);
  zoofile.close();
  return zoos;
}


pair<UrlProcessor::URLType, string> UrlProcessor::process(const string &url)
{
  string urlCap = url;

  transform(urlCap.begin(), urlCap.end(), urlCap.begin(),
            (int (*)(int))toupper);

  if (urlCap.find("ZOO://") == 0) {
    return pair<UrlProcessor::URLType, string>(UrlProcessor::ZOO,
                                               url.substr(6, 1024));
  } else if (urlCap.find("ZOOFILE://") == 0) {
    string zoos = parseZooFile(url.substr(10, 1024));
    return pair<UrlProcessor::URLType, string>(UrlProcessor::ZOO, zoos);
  } else if (urlCap.find("MESOS://") == 0) {
    return pair<UrlProcessor::URLType, string>(UrlProcessor::MESOS,
                                               url.substr(8, 1024));
  } else {
    return pair<UrlProcessor::URLType, string>(UrlProcessor::UNKNOWN, url);
  }
}

} // namespace internal
} // namespace mesos

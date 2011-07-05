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

#include <iostream>
#include <string>

#include "configurator.hpp"

using namespace mesos::internal;

using std::cout;
using std::string;


int main(int argc, char** argv)
{
  if (argc == 1) {
    cout << "Usage:  mesos-getconf OPTION [arg1 arg2 ... ]\n\n";
    cout << "OPTION\t\tis the name option whose value is to be extracted\n";
    cout << "arg1 ...\tOptional arguments passed to mesos, e.g. --quiet\n\n";
    cout << "This utility gives shell scripts access to Mesos parameters.\n\n";
    return 1;
  }

  Configurator configurator;
  configurator.load(argc, argv, true);

  string param(argv[1]);
  transform(param.begin(), param.end(), param.begin(), tolower);

  Configuration conf = configurator.getConfiguration();
  if (conf.contains(param)) {
    cout << conf[param] << "\n";
  }

  return 0;
}

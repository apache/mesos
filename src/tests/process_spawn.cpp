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
#include <climits>
#include <cstdio>

#include <stout/duration.hpp>
#include <stout/os.hpp>

using std::cout;
using std::cin;
using std::endl;


void processinfo()
{
  cout << "pid: " << getpid()
       << " pgid: " << getpgrp()
       << " ppid: " << getppid()
       << endl;
}


void dummywait()
{
  while(getchar() == EOF) {
    os::sleep(Seconds(INT_MAX));
  }

  cout << "Error: Shouldn't come here" << endl;
}


// This program forks 2 processes in a chain. The child process exits after
// forking its own child (the grandchild of the main). The grandchild is
// parented by Init but is in the same group as the main process.
int main()
{
  // Become session leader.
  setsid();

  pid_t pid = fork();

  if (pid > 0) {
    // Inside parent process.
    processinfo();
    dummywait();
  } else {
    // Inside child process.
    processinfo();

    pid_t pid2 = fork();

    if (pid2 > 0) {
      // Kill the child process so that the tree link is broken.
      _exit(0);
    } else {
      // Inside grandchild process.
      processinfo();
      dummywait();
    }
  }

  cout << "Error: Shouldn't come here!!" << endl;
  return -1;
}

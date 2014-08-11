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

package org.apache.mesos.state;

import java.util.concurrent.TimeUnit;

/**
 * Implementation of State that uses a replicated log to store
 * variables/values.
 */
public class LogState extends AbstractState {
  /**
   * Constructs a new instance of LogState.
   *
   * @param servers List of ZooKeeper servers, e.g., 'ip1:port1,ip2:port2'.
   * @param timeout ZooKeeper session timeout.
   * @param unit    Unit for session timeout.
   * @param znode   Path to znode where log replicas should be found.
   * @param quorum  Number of replicas necessary to persist a write.
   * @param path    Path the local replica uses to read/write data.
   */
  public LogState(String servers,
                  long timeout,
                  TimeUnit unit,
                  String znode,
                  long quorum,
                  String path) {
    initialize(servers, timeout, unit, znode, quorum, path, 0);
  }

  /**
   * Constructs a new instance of LogState.
   *
   * @param servers List of ZooKeeper servers, e.g., 'ip1:port1,ip2:port2'.
   * @param timeout ZooKeeper session timeout.
   * @param unit    Unit for session timeout.
   * @param znode   Path to znode where log replicas should be found.
   * @param quorum  Number of replicas necessary to persist a write.
   * @param path    Path the local replica uses to read/write data.
   * @param diffsBetweenSnapshots Number of diffs to write between snapshots.
   */
  public LogState(String servers,
                  long timeout,
                  TimeUnit unit,
                  String znode,
                  long quorum,
                  String path,
                  int diffsBetweenSnapshots) {
    initialize(servers, timeout, unit, znode, quorum, path, diffsBetweenSnapshots);
  }

  protected native void initialize(String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode,
                                   long quorum,
                                   String path,
                                   int diffsBetweenSnapshots);

  protected native void finalize();

  private long __log;
}

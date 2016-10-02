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
 * Implementation of State that uses ZooKeeper to store
 * variables/values. Note that this means the values associated with
 * variables cannot be more than 1 MB (actually slightly less since
 * we store some bookkeeping information).
 */
public class ZooKeeperState extends AbstractState {
  /**
   * Constructs a new instance of ZooKeeperState.
   *
   * @param servers List of ZooKeeper servers, e.g., 'ip1:port1,ip2:port2'.
   * @param timeout ZooKeeper session timeout.
   * @param unit    Unit for session timeout.
   * @param znode   Path to znode where "state" should be rooted.
   */
  public ZooKeeperState(String servers,
                        long timeout,
                        TimeUnit unit,
                        String znode) {
    initialize(servers, timeout, unit, znode);
  }

  /**
   * Constructs a new instance of ZooKeeperState.
   *
   * @param servers     List of ZooKeeper servers (e.g., 'ip1:port1,ip2:port2').
   * @param timeout     ZooKeeper session timeout.
   * @param unit        Unit for session timeout.
   * @param znode       Path to znode where "state" should be rooted.
   * @param scheme      Authentication scheme (e.g., "digest").
   * @param credentials Authentication credentials (e.g., "user:pass").
   */
  public ZooKeeperState(String servers,
                        long timeout,
                        TimeUnit unit,
                        String znode,
                        String scheme,
                        byte[] credentials) {
    initialize(servers, timeout, unit, znode, scheme, credentials);
  }

  protected native void initialize(String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode);

  protected native void initialize(String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode,
                                   String scheme,
                                   byte[] credentials);
}

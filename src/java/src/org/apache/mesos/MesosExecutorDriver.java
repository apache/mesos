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

package org.apache.mesos;

import org.apache.mesos.Protos.*;


/**
 * Concrete implementation of ExecutorDriver that communicates with a
 * Mesos slave. The slave's location is read from environment variables
 * set by it when it execs the user's executor script; users only need
 * to create the MesosExecutorDriver and call run() on it.
 */
public class MesosExecutorDriver implements ExecutorDriver {
  static {
    System.loadLibrary("mesos");
  }

  public MesosExecutorDriver(Executor exec) {
    this.exec = exec;

    initialize();
  }

  // Lifecycle methods.
  public native Status start();
  public native Status stop();
  public native Status abort();
  public native Status join();

  public Status run() {
    Status ret = start();
    return ret != Status.OK ? ret : join();
  }

  public native Status sendStatusUpdate(TaskStatus status);
  public native Status sendFrameworkMessage(byte[] data);

  protected native void initialize();
  protected native void finalize();

  private final Executor exec;

  private long __exec;
  private long __driver;
}


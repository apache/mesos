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
 * Concrete implementation of an ExecutorDriver that connects an
 * Executor with a Mesos slave. The MesosExecutorDriver is
 * thread-safe.
 * <p>
 * The driver is responsible for invoking the Executor callbacks as it
 * communicates with the Mesos slave.
 * </p>
 * <p>
 * Note that blocking on the MesosExecutorDriver (e.g., via {@link
 * #join}) doesn't affect the executor callbacks in anyway because
 * they are handled by a different thread.
 * </p>
 * <p>
 * Note that the driver uses GLOG to do its own logging. GLOG flags can
 * be set via environment variables, prefixing the flag name with
 * "GLOG_", e.g., "GLOG_v=1". For Mesos specific logging flags see
 * src/logging/flags.hpp. Mesos flags can also be set via environment
 * variables, prefixing the flag name with "MESOS_", e.g.,
 * "MESOS_QUIET=1".
 * </p>
 * <p>
 * See src/examples/java/TestExecutor.java for an example of using the
 * MesosExecutorDriver.
 * </p>
 */
public class MesosExecutorDriver implements ExecutorDriver {
  static {
    MesosNativeLibrary.load();
  }

  /**
   * Creates a new driver that uses the specified Executor.
   *
   * @param executor    The instance of the executor that will be used
   *                    to connect to the slave.
   *
   * @see Executor
   */
  public MesosExecutorDriver(Executor executor) {
    if (executor == null) {
      throw new NullPointerException("Not expecting a null Executor");
    }

    this.executor = executor;

    initialize();
  }

  /**
   * See ExecutorDriver for descriptions of these.
   *
   * @see ExecutorDriver
   */
  public native Status start();
  public native Status stop();
  public native Status abort();
  public native Status join();

  public Status run() {
    Status status = start();
    return status != Status.DRIVER_RUNNING ? status : join();
  }

  public native Status sendStatusUpdate(TaskStatus status);
  public native Status sendFrameworkMessage(byte[] data);

  protected native void initialize();
  protected native void finalize();

  private final Executor executor;

  private long __executor;
  private long __driver;
}

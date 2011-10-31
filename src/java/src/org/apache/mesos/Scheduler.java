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

import java.util.List;


/**
 * Callback interface to be implemented by frameworks' schedulers.
 */
public interface Scheduler {
  public void registered(SchedulerDriver driver, FrameworkID frameworkId);
  public void resourceOffers(SchedulerDriver driver, List<Offer> offers);
  public void offerRescinded(SchedulerDriver driver, OfferID offerId);
  public void statusUpdate(SchedulerDriver driver, TaskStatus status);
  public void frameworkMessage(SchedulerDriver driver, SlaveID slaveId, ExecutorID executorId, byte[] data);
  public void slaveLost(SchedulerDriver driver, SlaveID slaveId);
  public void error(SchedulerDriver driver, int code, String message);
}

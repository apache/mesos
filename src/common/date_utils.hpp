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

#ifndef __DATE_UTILS_HPP__
#define __DATE_UTILS_HPP__

#include <string>


namespace mesos { namespace internal {

/**
 * Utility functions for dealing with dates.
 */
class DateUtils
{
public:
  /**
   * Get the current date in the format used for Mesos IDs (YYYYMMDDhhmmss).
   */
  static std::string currentDate();

  /**
   * Unit test utility method that makes this class return a fixed string
   * as the date instead of looking up the current time.
   */
  static void setMockDate(std::string date);
  
  /**
   * Disable usage of the mock date set through setMockDate.
   */
  static void clearMockDate();

private:
  static bool useMockDate;
  static std::string mockDate;
};

}} /* namespace mesos::internal */

#endif


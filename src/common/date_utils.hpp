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
   * Get the current date in the format used for Mesos IDs (YYYYMMDDhhmm).
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


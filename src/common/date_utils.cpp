#include <ctime>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

#include "date_utils.hpp"

using std::string;

using namespace mesos::internal;


// Static fields in DateUtils
bool DateUtils::useMockDate = false;
string DateUtils::mockDate = "";


// Get the current date in the format used for Mesos IDs (YYYYMMDDhhmm).
string DateUtils::currentDate()
{
  if (useMockDate) {
    return mockDate;
  } else {
    time_t rawtime;
    struct tm* timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    char date[32];
    strftime(date, sizeof(date), "%Y%m%d%H%M", timeinfo);
    return date;
  }
}


// Get the current time in microseconds.
long DateUtils::currentDateTimeInMicro(){
  struct timeval curr_time;
  struct timezone tzp;
  gettimeofday(&curr_time, &tzp);
  return (long)(curr_time.tv_sec * 1000000 + curr_time.tv_usec);
}


// Get a human readable timestamp in microseconds.
  string DateUtils::humanReadableDateTimeInMicro(){
  time_t currTime;
  currTime=time(NULL);
  string timestamp = asctime(localtime(&currTime));
  return timestamp.erase(24); /* chop off the newline */
}


// Unit test utility method that makes this class return a fixed string
// as the date instead of looking up the current time.
void DateUtils::setMockDate(string date)
{
  useMockDate = true;
  mockDate = date;
}


// Disable usage of the mock date set through setMockDate.
void DateUtils::clearMockDate()
{
  useMockDate = false;
  mockDate = "";
}

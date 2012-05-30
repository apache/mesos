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

#ifndef __TIME_HPP__
#define __TIME_HPP__

struct hours;
struct minutes;
struct seconds;
struct milliseconds;
struct microseconds;
struct nanoseconds;


struct hours
{
  explicit hours(double _value) : value(_value) {}
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double mins() const { return value * 60; }
  double secs() const { return value * 3600; }
  double millis() const { return value * 3600 * 1000; }
  double micros() const { return value * 3600 * 1000000; }
  double nanos() const { return value * 3600 * 1000000000; }
  const double value;
};


struct minutes
{
  explicit minutes(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double hrs() const {return value / 60; }
  double secs() const { return value * 60; }
  double millis() const { return value * 60 * 1000; }
  double micros() const { return value * 60 * 1000000; }
  double nanos() const { return value * 60 * 1000000000; }
  const double value;
};


struct seconds
{
  explicit seconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double hrs() const { return value / 3600; }
  double mins() const { return value / 60; }
  double millis() const { return value * 1000; }
  double micros() const { return value * 1000000; }
  double nanos() const { return value * 1000000000; }
  const double value;
};


struct milliseconds
{
  explicit milliseconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator microseconds () const;
  inline operator nanoseconds () const;
  double hrs() const { return value / (3600 * 1000); }
  double mins() const { return value / (60 * 1000); }
  double secs() const { return value / 1000; }
  double micros() const { return value * 1000; }
  double nanos() const { return value * 1000000; }
  const double value;
};


struct microseconds
{
  explicit microseconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator nanoseconds () const;
  double hrs() const { return value / (3600 * 1000000); }
  double mins() const { return value / (60 * 1000000); }
  double secs() const { return value / 1000000; }
  double millis() const { return value / 1000; }
  double nanos() const { return value * 1000; }
  const double value;
};


struct nanoseconds
{
  explicit nanoseconds(double _value) : value(_value) {}
  inline operator hours () const;
  inline operator minutes () const;
  inline operator seconds () const;
  inline operator milliseconds () const;
  inline operator microseconds () const;
  double hrs() const { return value / (3600 * 1000000000); }
  double mins() const { return value / (60 * 1000000000); }
  double secs() const { return value / 1000000000; }
  double millis() const { return value / 1000000; }
  double micros() const { return value / 1000; }
  const double value;
};


inline hours::operator minutes () const
{
  return minutes(mins());
}


inline hours::operator seconds () const
{
  return seconds(secs());
}


inline hours::operator milliseconds () const
{
  return milliseconds(millis());
}


inline hours::operator microseconds () const
{
  return microseconds(micros());
}


inline hours::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline minutes::operator hours () const
{
  return hours(hrs());
}


inline minutes::operator seconds () const
{
  return seconds(secs());
}


inline minutes::operator milliseconds () const
{
  return milliseconds(millis());
}


inline minutes::operator microseconds () const
{
  return microseconds(micros());
}


inline minutes::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline seconds::operator hours () const
{
  return hours(hrs());
}


inline seconds::operator minutes () const
{
  return minutes(mins());
}


inline seconds::operator milliseconds () const
{
  return milliseconds(millis());
}


inline seconds::operator microseconds () const
{
  return microseconds(micros());
}


inline seconds::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline milliseconds::operator hours () const
{
  return hours(hrs());
}


inline milliseconds::operator minutes () const
{
  return minutes(mins());
}


inline milliseconds::operator seconds () const
{
  return seconds(secs());
}


inline milliseconds::operator microseconds () const
{
  return microseconds(micros());
}


inline milliseconds::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline microseconds::operator hours () const
{
  return hours(hrs());
}


inline microseconds::operator minutes () const
{
  return minutes(mins());
}


inline microseconds::operator seconds () const
{
  return seconds(secs());
}


inline microseconds::operator milliseconds () const
{
  return milliseconds(millis());
}


inline microseconds::operator nanoseconds () const
{
  return nanoseconds(nanos());
}


inline nanoseconds::operator hours () const
{
  return hours(hrs());
}


inline nanoseconds::operator minutes () const
{
  return minutes(mins());
}


inline nanoseconds::operator seconds () const
{
  return seconds(secs());
}


inline nanoseconds::operator milliseconds () const
{
  return milliseconds(millis());
}


inline nanoseconds::operator microseconds () const
{
  return microseconds(micros());
}

#endif // __TIME_HPP__

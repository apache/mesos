// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_CIRCULAR_BUFFER_HPP__
#define __STOUT_CIRCULAR_BUFFER_HPP__

// Using `boost::circular_buffer::debug_iterator` can lead to segfaults
// because they are not thread-safe (see MESOS-9177), so we must ensure
// they're disabled. Both versions of this macro are needed to account
// for differences between older and newer Boost versions.
#define BOOST_CB_DISABLE_DEBUG 1
#define BOOST_CB_ENABLE_DEBUG 0
#include <boost/circular_buffer.hpp>

using boost::circular_buffer;

#endif // __STOUT_CIRCULAR_BUFFER_HPP__

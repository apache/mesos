// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// limitations under the License.

#ifndef __STOUT_ADAPTOR_HPP__
#define __STOUT_ADAPTOR_HPP__

#include <boost/range/adaptor/reversed.hpp>

// This is modeled after boost's Range Adaptor. It wraps an existing
// iterator to provide a new iterator with different behavior. See
// details in boost's reference document:
// http://www.boost.org/doc/libs/1_53_0/libs/range/doc/html/index.html
namespace adaptor {

using boost::adaptors::reverse;

} // namespace adaptor {

#endif // __STOUT_ADAPTOR_HPP__

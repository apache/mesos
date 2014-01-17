#ifndef __STOUT_LIST_HPP__
#define __STOUT_LIST_HPP__

/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <list>

#include <stout/preprocessor.hpp>

template <typename T>
class List : public std::list<T>
{
public:
  List() {}

  // TODO(bmahler): Revisit when C++11 is required: we'll be able to
  // use the std::list constructor with an std::initiliazer_list.
#define INSERT(z, N, _) std::list<T>::push_back( t ## N );
#define TEMPLATE(Z, N, DATA) \
  List(ENUM_PARAMS(N, const T& t)) \
  { \
    REPEAT_FROM_TO(0, N, INSERT, _) \
  }

  REPEAT_FROM_TO(1, 21, TEMPLATE, _) // Args T1 -> T21.
#undef TEMPLATE
#undef INSERT
};

#endif // __STOUT_LIST_HPP__

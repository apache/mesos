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
#ifndef __STOUT_LAMBDA_HPP__
#define __STOUT_LAMBDA_HPP__

#if __cplusplus >= 201103L
#include <functional>
namespace lambda {
using std::bind;
using std::cref;
using std::function;
using std::ref;
using std::result_of;
using namespace std::placeholders;
} // namespace lambda {
#else // __cplusplus >= 201103L
#include <tr1/functional>
namespace lambda {
using std::tr1::cref;
using std::tr1::bind;
using std::tr1::function;
using std::tr1::ref;
using std::tr1::result_of;
using namespace std::tr1::placeholders;
} // namespace lambda {
#endif // __cplusplus >= 201103L

#endif // __STOUT_LAMBDA_HPP__

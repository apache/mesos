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

#ifndef __STOUT_FOREACH_HPP__
#define __STOUT_FOREACH_HPP__

#include <tuple>
#include <utility>

#include <stout/preprocessor.hpp>

#define STOUT_FOREACH_PREFIX CAT(__foreach_, __LINE__)
#define STOUT_FOREACH_BODY CAT(STOUT_FOREACH_PREFIX, _body__)
#define STOUT_FOREACH_BREAK CAT(STOUT_FOREACH_PREFIX, _break__)
#define STOUT_FOREACH_CONTINUE CAT(STOUT_FOREACH_PREFIX, _continue__)
#define STOUT_FOREACH_ELEM CAT(STOUT_FOREACH_PREFIX, _elem__)
#define STOUT_FOREACH_ONCE CAT(STOUT_FOREACH_PREFIX, _once__)


// `foreach` is a trivial expansion to the range-based `for`.
#define foreach(ELEM, ELEMS) for (ELEM : ELEMS)


// `foreachpair` is used to unpack the key and value of the pairs coming out of
// a sequence. e.g., std::map.
//
// Control flow:
//
// Labels:
//   * `STOUT_FOREACH_BREAK` is the label that when jumped to, breaks out of
//     the loop.
//   * `STOUT_FOREACH_BODY` is the label that helps to skip the loop exit checks
//     (break or continue) until we finish the current iteration.
//
// Flags:
//   * `STOUT_FOREACH_CONTINUE` determines whether the loop should continue or
//     not. If we encounter a `break`, this will be `false`. If we encounter a
//     `continue` or run the current iteration to completion,
//     `STOUT_FOREACH_CONTINUE` will be set to `true`.
//   * `STOUT_FOREACH_ONCE` is used to execute a `for` loop exactly once.
//
#define foreachpair(KEY, VALUE, ELEMS)                                       \
  foreach (auto&& STOUT_FOREACH_ELEM, ELEMS)                                 \
    if (false) STOUT_FOREACH_BREAK: break; /* set up the break path */       \
    else if (bool STOUT_FOREACH_CONTINUE = false) {} /* var decl */          \
    else if (true) goto STOUT_FOREACH_BODY; /* skip the loop exit checks */  \
    else for (;;) /* determine whether we should break or continue. */       \
      if (!STOUT_FOREACH_CONTINUE) goto STOUT_FOREACH_BREAK; /* break */     \
      else if (true) break; /* continue */                                   \
      else                                                                   \
        STOUT_FOREACH_BODY:                                                  \
        if (bool STOUT_FOREACH_ONCE = false) {} /* var decl */               \
        else for (KEY = std::get<0>(                                         \
                      std::forward<decltype(STOUT_FOREACH_ELEM)>(            \
                          STOUT_FOREACH_ELEM));                              \
                  !STOUT_FOREACH_ONCE;                                       \
                  STOUT_FOREACH_ONCE = true)                                 \
          for (VALUE = std::get<1>(                                          \
                   std::forward<decltype(STOUT_FOREACH_ELEM)>(               \
                       STOUT_FOREACH_ELEM));                                 \
               !STOUT_FOREACH_CONTINUE;                                      \
               STOUT_FOREACH_CONTINUE = true)


#define foreachkey(KEY, ELEMS) foreachpair (KEY, std::ignore, ELEMS)


#define foreachvalue(VALUE, ELEMS) foreachpair (std::ignore, VALUE, ELEMS)

#endif // __STOUT_FOREACH_HPP__

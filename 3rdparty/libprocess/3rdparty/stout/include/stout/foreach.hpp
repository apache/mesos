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
#ifndef __STOUT_FOREACH_HPP__
#define __STOUT_FOREACH_HPP__

#include <boost/foreach.hpp>

namespace __foreach__ {

// NOTE: This is a copied from Boost
// (boost/tuple/detail/tuple_basic_no_partial_spec.hpp) because the
// new 'boost::tuples::ignore' does not work in our 'foreachkey' and
// 'foreachvalue'.
struct swallow_assign {
  template<typename T>
  swallow_assign const& operator=(const T&) const {
    return *this;
  }
};

swallow_assign const ignore = swallow_assign();

} // namespace __foreach__ {

#define BOOST_FOREACH_PAIR(VARFIRST, VARSECOND, COL)                    \
  BOOST_FOREACH_PREAMBLE()                                              \
  if (boost::foreach_detail_::auto_any_t BOOST_FOREACH_ID(_foreach_col) = BOOST_FOREACH_CONTAIN(COL)) {} else /* NOLINT(whitespace/line_length) */ \
    if (boost::foreach_detail_::auto_any_t BOOST_FOREACH_ID(_foreach_cur) = BOOST_FOREACH_BEGIN(COL)) {} else /* NOLINT(whitespace/line_length) */ \
      if (boost::foreach_detail_::auto_any_t BOOST_FOREACH_ID(_foreach_end) = BOOST_FOREACH_END(COL)) {} else /* NOLINT(whitespace/line_length) */ \
        for (bool BOOST_FOREACH_ID(_foreach_continue) = true, BOOST_FOREACH_ID(_foreach_onetime) = true; /* NOLINT(whitespace/line_length) */ \
             BOOST_FOREACH_ID(_foreach_continue) && !BOOST_FOREACH_DONE(COL); /* NOLINT(whitespace/line_length) */ \
             BOOST_FOREACH_ID(_foreach_continue) ? BOOST_FOREACH_NEXT(COL) : (void)0) /* NOLINT(whitespace/line_length) */ \
          if (boost::foreach_detail_::set_false(BOOST_FOREACH_ID(_foreach_onetime))) {} else /* NOLINT(whitespace/line_length) */ \
            for (VARFIRST = BOOST_FOREACH_DEREF(COL).first;             \
                 !BOOST_FOREACH_ID(_foreach_onetime);                   \
                 BOOST_FOREACH_ID(_foreach_onetime) = true)             \
              if (boost::foreach_detail_::set_false(BOOST_FOREACH_ID(_foreach_continue))) {} else /* NOLINT(whitespace/line_length) */ \
                for (VARSECOND = BOOST_FOREACH_DEREF(COL).second;       \
                     !BOOST_FOREACH_ID(_foreach_continue);              \
                     BOOST_FOREACH_ID(_foreach_continue) = true)

#define foreach BOOST_FOREACH
#define foreachpair BOOST_FOREACH_PAIR

#define foreachkey(VAR, COL)                    \
  foreachpair (VAR, __foreach__::ignore, COL)

#define foreachvalue(VAR, COL)                  \
  foreachpair (__foreach__::ignore, VAR, COL)

#endif // __STOUT_FOREACH_HPP__

#ifndef __STOUT_LAMBDA_HPP__
#define __STOUT_LAMBDA_HPP__

#if __cplusplus >= 201103L
#include <functional>
namespace lambda {
using std::bind;
using std::function;
using std::result_of;
using namespace std::placeholders;
} // namespace lambda {
#else // __cplusplus >= 201103L
#include <tr1/functional>
namespace lambda {
using std::tr1::bind;
using std::tr1::function;
using std::tr1::result_of;
using namespace std::tr1::placeholders;
} // namespace lambda {
#endif // __cplusplus >= 201103L

#endif // __STOUT_LAMBDA_HPP__

#ifndef _URL_PRCESSOR_HPP_
#define _URL_PRCESSOR_HPP_

#include <iostream>
#include <string>
#include <glog/logging.h>

using std::string;
using std::pair;

namespace nexus { namespace internal {
    
class UrlProcessor {
      
public:
  enum URLType {ZOO, NEXUS, UNKNOWN};
  
  static string parseZooFile(const string &zooFilename);
  
  static pair<UrlProcessor::URLType, string> process(const string &_url);
};

}}

#endif /* _URL_PROCESSOR_HPP_ */

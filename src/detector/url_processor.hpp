#ifndef _URL_PRCESSOR_HPP_
#define _URL_PRCESSOR_HPP_

#include <string>


namespace mesos { namespace internal {
    
class UrlProcessor {
      
public:
  enum URLType { ZOO, MESOS, UNKNOWN };
  
  static std::string parseZooFile(const std::string &zooFilename);
  
  static std::pair<UrlProcessor::URLType, std::string> process(const std::string &_url);
};

}}

#endif /* _URL_PROCESSOR_HPP_ */

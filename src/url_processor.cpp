#include <fstream>
#include <iostream>
#include <string>
#include <algorithm>
#include <glog/logging.h>
#include "url_processor.hpp"

using namespace std;

namespace nexus { namespace internal {
    
string UrlProcessor::parseZooFile(const string &zooFilename) {
  string zoos = "";
  
  LOG(INFO) << "Opening ZooFile: " << zooFilename;
  ifstream zoofile(zooFilename.c_str());
  if (!zoofile) 
    LOG(ERROR) << "ZooFile " << zooFilename << " could not be opened";
  
  while(!zoofile.eof()) {
    string line;
    getline(zoofile, line);
    if (line == "")
      continue;
    if (zoos != "")
      zoos += ',';
    zoos += line;
  }
    remove_if(zoos.begin(),zoos.end(), (int (*)(int)) isspace);
  zoofile.close();
  return zoos;
}
  
pair<UrlProcessor::URLType, string> UrlProcessor::process(const string &url) {
  
  string urlCap = url;
  
  transform(urlCap.begin(), urlCap.end(), urlCap.begin(), (int (*)(int))toupper);
  
  if (urlCap.find("ZOO://") == 0) {
    
    return pair<UrlProcessor::URLType, string>(UrlProcessor::ZOO, url.substr(6,1024));
    
  } else if (urlCap.find("ZOOFILE://") == 0) {
    
    string zoos = parseZooFile( url.substr(10,1024) );
    
    return pair<UrlProcessor::URLType, string>(UrlProcessor::ZOO, zoos);
    
  } else if (urlCap.find("NEXUS://") == 0) {

    return pair<UrlProcessor::URLType, string>(UrlProcessor::NEXUS, url.substr(8,1024));

  } else {

    LOG(WARNING) << "Could not parse master/zoo URL";
    return pair<UrlProcessor::URLType, string>(UrlProcessor::UNKNOWN, "");

  }
}

}}

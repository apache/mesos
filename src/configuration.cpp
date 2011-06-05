#include <stdlib.h>
#include <unistd.h>

#include <iostream>

#include "configuration.hpp"

#include "params.hpp"
#include "boost/foreach.hpp"

using std::cout;
using std::endl;

using namespace nexus::internal;

extern char** environ;


Configuration::Configuration() 
{
  parseEnv();
  string cnf;
  const map<string,string> &map = params.getMap();
  if (map.find("CONF") != map.end())
    cnf = params["CONF"];
  else 
    cnf = getMesosHome() + "/" + DEFAULT_CONF_NAME;
  parseConfig(cnf);
}

Configuration::Configuration(int argc, char **argv) 
{
  parseEnv();
  parseCmdline(argc, argv);
  string cnf;
  const map<string,string> &map = params.getMap();
  if (map.find("CONF") != map.end())
    cnf = params["CONF"];
  else 
    cnf = getMesosHome() + "/" + DEFAULT_CONF_NAME;
  parseConfig(cnf);
}

Configuration::Configuration(const map<string,string> &_params) 
{
  parseEnv();
  params.parseMap(_params);
  string cnf;
  const map<string,string> &map = params.getMap();
  if (map.find("CONF") != map.end())
    cnf = params["CONF"];
  else 
    cnf = getMesosHome() + "/" + DEFAULT_CONF_NAME;
  parseConfig(cnf);
}

string Configuration::getMesosHome() 
{
  string mesosHome;
  if (params.getMap().find("HOME") == params.getMap().end()) {
    mesosHome = DEFAULT_HOME;
    LOG(WARNING) << "MESOS_HOME environment variable not set. Using default " 
                 << mesosHome;
  } else {
    mesosHome = params["HOME"];
  }
  return mesosHome;
}

void Configuration::parseEnv()
{
  int i = 0;
  while(environ[i] != NULL) {
    string line = environ[i];
    if (line.find(DEFAULT_PREFIX) == 0) {
      string key, val;
      string::size_type eq = line.find_first_of("=");
      if (eq == string::npos)  
        continue; // ignore problematic strings
      key = line.substr(sizeof(DEFAULT_PREFIX)-1, eq - (sizeof(DEFAULT_PREFIX)-1));
      val = line.substr(eq + 1);
      params[key] = val;
    }
    i++;
  }
}

void Configuration::parseMap(const map<string, string> &map)
{
  params.parseMap(map);
}

void Configuration::parseCmdline(int argc, char **argv)
{
  vector<string> args;
  map<string,string> store;

  for (int i=1; i < argc; i++)
    args.push_back(argv[i]);

  argc = args.size(); // argc -= 1
    
  for (int i=0; i < argc; i++) {
    string key, val;
    bool set = false;
    if (args[i].find("--", 0) == 0) {  // handles --blah=25 and --blah
      string::size_type eq = args[i].find_first_of("=");
      if (eq == string::npos) {        
        key = args[i].substr(2);
        val = "1";
        set = true;
      } else {                         
        key = args[i].substr(2, eq-2);
        val = args[i].substr(eq+1);
        set = true;
      } 
    } else if (args[i].find_first_of("-", 0) == 0) { // handles -blah 25 and -blah
      if ((i+1 >= argc) || (i+1 < argc && args[i+1].find_first_of("-",0) == 0)) {
        key = args[i].substr(1);
        val = "1";
        set = true;
      } else if (i+1 < argc && args[i+1].find_first_of("-",0) != 0) {
        key = args[i].substr(1);
        val = args[i+1];
        set = true;
        i++;  // we've consumed next parameter as "value"-parameter
      }
    }
    if (set) {
      params[key] = val;
    }
  }
}

int Configuration::parseConfig(const string &fname) {
  ifstream cfg(fname.c_str(), std::ios::in);
  if (!cfg.is_open()) {
    LOG(ERROR) << "Couldn't read Mesos configuration file from: " 
               << fname;
    return -1;
  }

  string buf, line;

  while (!cfg.eof()) {
    getline(cfg, line);
    string::size_type beg = line.find_first_of("#"); // strip comments
    beg = line.find_last_not_of("#\t \n\r", beg) + 1; // strip trailing ws
    buf += line.substr(0, beg) + "\n";
  }
  cfg.close();
  params.parseString(buf);
  return 0;
}

Params &Configuration::getParams() 
{
  return params;
}


// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_MIME_HPP__
#define __PROCESS_MIME_HPP__

#include <map>
#include <string>

namespace process {
namespace mime {

extern std::map<std::string, std::string> types;

inline void initialize()
{
  // These MIME types were collected via:
  /*
    python -c '
    import mimetypes
    for extension, type in mimetypes.types_map.iteritems():
      print "types[\"%s\"] = \"%s\";" % (extension, type)
    '
  */

  types[".obj"] = "application/octet-stream";
  types[".ra"] = "audio/x-pn-realaudio";
  types[".wsdl"] = "application/xml";
  types[".dll"] = "application/octet-stream";
  types[".ras"] = "image/x-cmu-raster";
  types[".ram"] = "application/x-pn-realaudio";
  types[".bcpio"] = "application/x-bcpio";
  types[".sh"] = "application/x-sh";
  types[".m1v"] = "video/mpeg";
  types[".xwd"] = "image/x-xwindowdump";
  types[".doc"] = "application/msword";
  types[".bmp"] = "image/x-ms-bmp";
  types[".shar"] = "application/x-shar";
  types[".js"] = "application/x-javascript";
  types[".src"] = "application/x-wais-source";
  types[".dvi"] = "application/x-dvi";
  types[".aif"] = "audio/x-aiff";
  types[".ksh"] = "text/plain";
  types[".dot"] = "application/msword";
  types[".mht"] = "message/rfc822";
  types[".p12"] = "application/x-pkcs12";
  types[".css"] = "text/css";
  types[".csh"] = "application/x-csh";
  types[".pwz"] = "application/vnd.ms-powerpoint";
  types[".pdf"] = "application/pdf";
  types[".cdf"] = "application/x-netcdf";
  types[".pl"] = "text/plain";
  types[".ai"] = "application/postscript";
  types[".jpe"] = "image/jpeg";
  types[".jpg"] = "image/jpeg";
  types[".py"] = "text/x-python";
  types[".xml"] = "text/xml";
  types[".jpeg"] = "image/jpeg";
  types[".ps"] = "application/postscript";
  types[".gtar"] = "application/x-gtar";
  types[".xpm"] = "image/x-xpixmap";
  types[".hdf"] = "application/x-hdf";
  types[".nws"] = "message/rfc822";
  types[".tsv"] = "text/tab-separated-values";
  types[".xpdl"] = "application/xml";
  types[".p7c"] = "application/pkcs7-mime";
  types[".eps"] = "application/postscript";
  types[".ief"] = "image/ief";
  types[".so"] = "application/octet-stream";
  types[".xlb"] = "application/vnd.ms-excel";
  types[".pbm"] = "image/x-portable-bitmap";
  types[".texinfo"] = "application/x-texinfo";
  types[".xls"] = "application/vnd.ms-excel";
  types[".tex"] = "application/x-tex";
  types[".rtx"] = "text/richtext";
  types[".html"] = "text/html";
  types[".aiff"] = "audio/x-aiff";
  types[".aifc"] = "audio/x-aiff";
  types[".exe"] = "application/octet-stream";
  types[".sgm"] = "text/x-sgml";
  types[".tif"] = "image/tiff";
  types[".mpeg"] = "video/mpeg";
  types[".ustar"] = "application/x-ustar";
  types[".gif"] = "image/gif";
  types[".ppt"] = "application/vnd.ms-powerpoint";
  types[".pps"] = "application/vnd.ms-powerpoint";
  types[".sgml"] = "text/x-sgml";
  types[".ppm"] = "image/x-portable-pixmap";
  types[".latex"] = "application/x-latex";
  types[".bat"] = "text/plain";
  types[".mov"] = "video/quicktime";
  types[".ppa"] = "application/vnd.ms-powerpoint";
  types[".tr"] = "application/x-troff";
  types[".rdf"] = "application/xml";
  types[".xsl"] = "application/xml";
  types[".eml"] = "message/rfc822";
  types[".nc"] = "application/x-netcdf";
  types[".sv4cpio"] = "application/x-sv4cpio";
  types[".bin"] = "application/octet-stream";
  types[".h"] = "text/plain";
  types[".tcl"] = "application/x-tcl";
  types[".wiz"] = "application/msword";
  types[".o"] = "application/octet-stream";
  types[".a"] = "application/octet-stream";
  types[".c"] = "text/plain";
  types[".wav"] = "audio/x-wav";
  types[".vcf"] = "text/x-vcard";
  types[".xbm"] = "image/x-xbitmap";
  types[".txt"] = "text/plain";
  types[".au"] = "audio/basic";
  types[".t"] = "application/x-troff";
  types[".tiff"] = "image/tiff";
  types[".texi"] = "application/x-texinfo";
  types[".oda"] = "application/oda";
  types[".ms"] = "application/x-troff-ms";
  types[".rgb"] = "image/x-rgb";
  types[".me"] = "application/x-troff-me";
  types[".sv4crc"] = "application/x-sv4crc";
  types[".qt"] = "video/quicktime";
  types[".mpa"] = "video/mpeg";
  types[".mpg"] = "video/mpeg";
  types[".mpe"] = "video/mpeg";
  types[".avi"] = "video/x-msvideo";
  types[".pgm"] = "image/x-portable-graymap";
  types[".pot"] = "application/vnd.ms-powerpoint";
  types[".mif"] = "application/x-mif";
  types[".roff"] = "application/x-troff";
  types[".htm"] = "text/html";
  types[".man"] = "application/x-troff-man";
  types[".etx"] = "text/x-setext";
  types[".zip"] = "application/zip";
  types[".movie"] = "video/x-sgi-movie";
  types[".pyc"] = "application/x-python-code";
  types[".png"] = "image/png";
  types[".pfx"] = "application/x-pkcs12";
  types[".mhtml"] = "message/rfc822";
  types[".tar"] = "application/x-tar";
  types[".pnm"] = "image/x-portable-anymap";
  types[".pyo"] = "application/x-python-code";
  types[".snd"] = "audio/basic";
  types[".cpio"] = "application/x-cpio";
  types[".swf"] = "application/x-shockwave-flash";
  types[".mp3"] = "audio/mpeg";
  types[".mp2"] = "audio/mpeg";
  types[".mp4"] = "video/mp4";
}

} // namespace mime {
} // namespace process {

#endif // __PROCESS_MIME_HPP__

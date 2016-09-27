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

#include <process/mime.hpp>

namespace process {
namespace mime {

void initialize()
{
  // These MIME types were collected via:
  /*
    python -c '
    import mimetypes
    for extension, type in mimetypes.types_map.iteritems():
      print "types[\"%s\"] = \"%s\";" % (extension, type)
    '
  */

  types.insert({
    {".obj", "application/octet-stream"},
    {".ra", "audio/x-pn-realaudio"},
    {".wsdl", "application/xml"},
    {".dll", "application/octet-stream"},
    {".ras", "image/x-cmu-raster"},
    {".ram", "application/x-pn-realaudio"},
    {".bcpio", "application/x-bcpio"},
    {".sh", "application/x-sh"},
    {".m1v", "video/mpeg"},
    {".xwd", "image/x-xwindowdump"},
    {".doc", "application/msword"},
    {".bmp", "image/x-ms-bmp"},
    {".shar", "application/x-shar"},
    {".js", "application/x-javascript"},
    {".src", "application/x-wais-source"},
    {".dvi", "application/x-dvi"},
    {".aif", "audio/x-aiff"},
    {".ksh", "text/plain"},
    {".dot", "application/msword"},
    {".mht", "message/rfc822"},
    {".p12", "application/x-pkcs12"},
    {".css", "text/css"},
    {".csh", "application/x-csh"},
    {".pwz", "application/vnd.ms-powerpoint"},
    {".pdf", "application/pdf"},
    {".cdf", "application/x-netcdf"},
    {".pl", "text/plain"},
    {".ai", "application/postscript"},
    {".jpe", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".py", "text/x-python"},
    {".xml", "text/xml"},
    {".jpeg", "image/jpeg"},
    {".ps", "application/postscript"},
    {".gtar", "application/x-gtar"},
    {".xpm", "image/x-xpixmap"},
    {".hdf", "application/x-hdf"},
    {".nws", "message/rfc822"},
    {".tsv", "text/tab-separated-values"},
    {".xpdl", "application/xml"},
    {".p7c", "application/pkcs7-mime"},
    {".eps", "application/postscript"},
    {".ief", "image/ief"},
    {".so", "application/octet-stream"},
    {".xlb", "application/vnd.ms-excel"},
    {".pbm", "image/x-portable-bitmap"},
    {".texinfo", "application/x-texinfo"},
    {".xls", "application/vnd.ms-excel"},
    {".tex", "application/x-tex"},
    {".rtx", "text/richtext"},
    {".html", "text/html"},
    {".aiff", "audio/x-aiff"},
    {".aifc", "audio/x-aiff"},
    {".exe", "application/octet-stream"},
    {".sgm", "text/x-sgml"},
    {".tif", "image/tiff"},
    {".mpeg", "video/mpeg"},
    {".ustar", "application/x-ustar"},
    {".gif", "image/gif"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pps", "application/vnd.ms-powerpoint"},
    {".sgml", "text/x-sgml"},
    {".ppm", "image/x-portable-pixmap"},
    {".latex", "application/x-latex"},
    {".bat", "text/plain"},
    {".mov", "video/quicktime"},
    {".ppa", "application/vnd.ms-powerpoint"},
    {".tr", "application/x-troff"},
    {".rdf", "application/xml"},
    {".xsl", "application/xml"},
    {".eml", "message/rfc822"},
    {".nc", "application/x-netcdf"},
    {".sv4cpio", "application/x-sv4cpio"},
    {".bin", "application/octet-stream"},
    {".h", "text/plain"},
    {".tcl", "application/x-tcl"},
    {".wiz", "application/msword"},
    {".o", "application/octet-stream"},
    {".a", "application/octet-stream"},
    {".c", "text/plain"},
    {".wav", "audio/x-wav"},
    {".vcf", "text/x-vcard"},
    {".xbm", "image/x-xbitmap"},
    {".txt", "text/plain"},
    {".au", "audio/basic"},
    {".t", "application/x-troff"},
    {".tiff", "image/tiff"},
    {".texi", "application/x-texinfo"},
    {".oda", "application/oda"},
    {".ms", "application/x-troff-ms"},
    {".rgb", "image/x-rgb"},
    {".me", "application/x-troff-me"},
    {".sv4crc", "application/x-sv4crc"},
    {".qt", "video/quicktime"},
    {".mpa", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".mpe", "video/mpeg"},
    {".avi", "video/x-msvideo"},
    {".pgm", "image/x-portable-graymap"},
    {".pot", "application/vnd.ms-powerpoint"},
    {".mif", "application/x-mif"},
    {".roff", "application/x-troff"},
    {".htm", "text/html"},
    {".man", "application/x-troff-man"},
    {".etx", "text/x-setext"},
    {".zip", "application/zip"},
    {".movie", "video/x-sgi-movie"},
    {".pyc", "application/x-python-code"},
    {".png", "image/png"},
    {".pfx", "application/x-pkcs12"},
    {".mhtml", "message/rfc822"},
    {".tar", "application/x-tar"},
    {".pnm", "image/x-portable-anymap"},
    {".pyo", "application/x-python-code"},
    {".snd", "audio/basic"},
    {".cpio", "application/x-cpio"},
    {".swf", "application/x-shockwave-flash"},
    {".mp3", "audio/mpeg"},
    {".mp2", "audio/mpeg"},
    {".mp4", "video/mp4"},
  });
}

} // namespace mime {
} // namespace process {

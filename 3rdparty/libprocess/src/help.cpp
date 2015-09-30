/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <process/help.hpp>

#include <map>
#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/preprocessor.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

using std::string;
using std::vector;

namespace process {

string HELP(
    string tldr,
    string description,
    const Option<string>& references)
{
  // Make sure 'tldr' and 'description' end with a newline.
  if (!strings::endsWith(tldr, "\n")) {
    tldr += "\n";
  }

  if (!strings::endsWith(description, "\n")) {
    description += "\n";
  }

  // Construct the help string.
  string help =
    "### TL;DR; ###\n" +
    tldr +
    "\n" +
    "### DESCRIPTION ###\n" +
    description;

  if (references.isSome()) {
    help += "\n";
    help += references.get();
  }

  return help;
}


Help::Help() : ProcessBase("help") {}


string Help::getUsagePath(const string& id, const string& name)
{
  return id + strings::remove(name, "/", strings::Mode::SUFFIX);
}


void Help::add(
    const string& id,
    const string& name,
    const Option<string>& help)
{
  // TODO(benh): Enable help for help.
  if (id != "help" && id != "__processes__") {
    // Remove tail slash in usage information.
    const string path = "/" + getUsagePath(id, name);

    if (help.isSome()) {
      string usage = "### USAGE ###\n" + USAGE(path) + "\n";
      helps[id][name] = usage + help.get();
    } else {
      helps[id][name] = "## No help page for `" + path + "`\n";
    }

    route("/" + id, "Help for " + id, &Help::help);
  }
}


void Help::initialize()
{
  route("/", None(), &Help::help);
}


Future<http::Response> Help::help(const http::Request& request)
{
  // Split the path by '/'.
  vector<string> tokens = strings::tokenize(request.url.path, "/");

  Option<string> id = None();
  Option<string> name = None();

  if (tokens.size() > 3) {
    return http::BadRequest("Malformed URL, expecting '/help/id/name/'\n");
  } else if (tokens.size() == 3) {
    id = tokens[1];
    name = tokens[2];
  } else if (tokens.size() > 1) {
    id = tokens[1];
  }

  string document;
  string references;

  if (id.isNone()) {             // http://ip:port/help
    document += "## HELP\n";
    foreachkey (const string& id, helps) {
      document += "> [/" + id + "][" + id + "]\n";
      references += "[" + id + "]: help/" + id + "\n";
    }
  } else if (name.isNone()) {    // http://ip:port/help/id
    if (helps.count(id.get()) == 0) {
      return http::BadRequest(
          "No help available for '/" + id.get() + "'.\n");
    }

    document += "## `/" + id.get() + "` ##\n";
    foreachkey (const string& name, helps[id.get()]) {
      const string path = getUsagePath(id.get(), name);
      document += "> [/" +  path + "][" + path + "]\n";
      references += "[" + path + "]: " + path + "\n";
    }
  } else {                       // http://ip:port/help/id/name
    if (helps.count(id.get()) == 0) {
      return http::BadRequest(
          "No help available for '/" + id.get() + "'.\n");
    } else if (helps[id.get()].count("/" + name.get()) == 0) {
      return http::BadRequest(
        "No help available for '/" + id.get() + "/" + name.get() + "'.\n");
    }

    document += helps[id.get()]["/" + name.get()];
  }

  // Final Markdown is 'document' followed by the 'references'.
  string markdown = document + "\n" + references;

  // Just send the Markdown if we aren't speaking to a browser. For
  // now we only check for the 'curl' or 'http' utilities.
  Option<string> agent = request.headers.get("User-Agent");

  if (agent.isSome() &&
      (strings::startsWith(agent.get(), "curl") ||
       strings::startsWith(agent.get(), "HTTPie"))) {
    http::Response response = http::OK(markdown);
    response.headers["Content-Type"] = "text/x-markdown";
    return response;
  }

  // Need to JSONify the markdown for embedding into JavaScript.
  markdown = stringify(JSON::String(markdown));

  // Provide some JavaScript to render the Markdown into some aesthetically
  // pleasing HTML. ;)
  return http::OK(
      "<html>"
      "<head>"
      "<title>Help</title>"
      "<script>"
      "  /**"
      "  * Minified version of:"
      "  * marked - a markdown parser"
      "  * Copyright (c) 2011-2013, Christopher Jeffrey. (MIT Licensed)"
      "  * https://github.com/chjj/marked"
      "  */"
      "  (function(){var d={newline:/^\\n+/,code:/^( {4}[^\\n]+\\n*)+/,fences:j,hr:/^( *[-*_]){3,} *(?:\\n+|$)/,heading:/^ *(#{1,6}) *([^\\n]+?) *#* *(?:\\n+|$)/,nptable:j,lheading:/^([^\\n]+)\\n *(=|-){2,} *(?:\\n+|$)/,blockquote:/^( *>[^\\n]+(\\n[^\\n]+)*\\n*)+/,list:/^( *)(bull) [\\s\\S]+?(?:hr|\\n{2,}(?! )(?!\\1bull )\\n*|\\s*$)/,html:/^ *(?:comment|closed|closing) *(?:\\n{2,}|\\s*$)/,def:/^ *\\[([^\\]]+)\\]: *<?([^\\s>]+)>?(?: +[\"(]([^\\n]+)[\")])? *(?:\\n+|$)/,table:j,paragraph:/^((?:[^\\n]+\\n?(?!hr|heading|lheading|blockquote|tag|def))+)\\n*/,text:/^[^\\n]+/};d.bullet=/(?:[*+-]|\\d+\\.)/;d.item=/^( *)(bull) [^\\n]*(?:\\n(?!\\1bull )[^\\n]*)*/;d.item=c(d.item,\"gm\")(/bull/g,d.bullet)();d.list=c(d.list)(/bull/g,d.bullet)(\"hr\",/\\n+(?=(?: *[-*_]){3,} *(?:\\n+|$))/)();d._tag=\"(?!(?:a|em|strong|small|s|cite|q|dfn|abbr|data|time|code|var|samp|kbd|sub|sup|i|b|u|mark|ruby|rt|rp|bdi|bdo|span|br|wbr|ins|del|img)\\\\b)\\\\w+(?!:/|@)\\\\b\";d.html=c(d.html)(\"comment\",/<!--[\\s\\S]*?-->/)(\"closed\",/<(tag)[\\s\\S]+?<\\/\\1>/)(\"closing\",/<tag(?:\"[^\"]*\"|'[^']*'|[^'\">])*?>/)(/tag/g,d._tag)();d.paragraph=c(d.paragraph)(\"hr\",d.hr)(\"heading\",d.heading)(\"lheading\",d.lheading)(\"blockquote\",d.blockquote)(\"tag\",\"<\"+d._tag)(\"def\",d.def)();d.normal=g({},d);d.gfm=g({},d.normal,{fences:/^ *(`{3,}|~{3,}) *(\\S+)? *\\n([\\s\\S]+?)\\s*\\1 *(?:\\n+|$)/,paragraph:/^/});d.gfm.paragraph=c(d.paragraph)(\"(?!\",\"(?!\"+d.gfm.fences.source.replace(\"\\\\1\",\"\\\\2\")+\"|\"+d.list.source.replace(\"\\\\1\",\"\\\\3\")+\"|\")();d.tables=g({},d.gfm,{nptable:/^ *(\\S.*\\|.*)\\n *([-:]+ *\\|[-| :]*)\\n((?:.*\\|.*(?:\\n|$))*)\\n*/,table:/^ *\\|(.+)\\n *\\|( *[-:]+[-| :]*)\\n((?: *\\|.*(?:\\n|$))*)\\n*/});function b(k){this.tokens=[];this.tokens.links={};this.options=k||a.defaults;this.rules=d.normal;if(this.options.gfm){if(this.options.tables){this.rules=d.tables}else{this.rules=d.gfm}}}b.rules=d;b.lex=function(m,k){var l=new b(k);return l.lex(m)};b.prototype.lex=function(k){k=k.replace(/\\r\\n|\\r/g,\"\\n\").replace(/\\t/g,\"    \").replace(/\\u00a0/g,\" \").replace(/\\u2424/g,\"\\n\");return this.token(k,true)};b.prototype.token=function(m,s){var m=m.replace(/^ +$/gm,\"\"),q,o,u,r,t,v,k,p,n;while(m){if(u=this.rules.newline.exec(m)){m=m.substring(u[0].length);if(u[0].length>1){this.tokens.push({type:\"space\"})}}if(u=this.rules.code.exec(m)){m=m.substring(u[0].length);u=u[0].replace(/^ {4}/gm,\"\");this.tokens.push({type:\"code\",text:!this.options.pedantic?u.replace(/\\n+$/,\"\"):u});continue}if(u=this.rules.fences.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:\"code\",lang:u[2],text:u[3]});continue}if(u=this.rules.heading.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:\"heading\",depth:u[1].length,text:u[2]});continue}if(s&&(u=this.rules.nptable.exec(m))){m=m.substring(u[0].length);v={type:\"table\",header:u[1].replace(/^ *| *\\| *$/g,\"\").split(/ *\\| */),align:u[2].replace(/^ *|\\| *$/g,\"\").split(/ *\\| */),cells:u[3].replace(/\\n$/,\"\").split(\"\\n\")};for(p=0;p<v.align.length;p++){if(/^ *-+: *$/.test(v.align[p])){v.align[p]=\"right\"}else{if(/^ *:-+: *$/.test(v.align[p])){v.align[p]=\"center\"}else{if(/^ *:-+ *$/.test(v.align[p])){v.align[p]=\"left\"}else{v.align[p]=null}}}}for(p=0;p<v.cells.length;p++){v.cells[p]=v.cells[p].split(/ *\\| */)}this.tokens.push(v);continue}if(u=this.rules.lheading.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:\"heading\",depth:u[2]===\"=\"?1:2,text:u[1]});continue}if(u=this.rules.hr.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:\"hr\"});continue}if(u=this.rules.blockquote.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:\"blockquote_start\"});u=u[0].replace(/^ *> ?/gm,\"\");this.token(u,s);this.tokens.push({type:\"blockquote_end\"});continue}if(u=this.rules.list.exec(m)){m=m.substring(u[0].length);r=u[2];this.tokens.push({type:\"list_start\",ordered:r.length>1});u=u[0].match(this.rules.item);q=false;n=u.length;p=0;for(;p<n;p++){v=u[p];k=v.length;v=v.replace(/^ *([*+-]|\\d+\\.) +/,\"\");if(~v.indexOf(\"\\n \")){k-=v.length;v=!this.options.pedantic?v.replace(new RegExp(\"^ {1,\"+k+\"}\",\"gm\"),\"\"):v.replace(/^ {1,4}/gm,\"\")}if(this.options.smartLists&&p!==n-1){t=d.bullet.exec(u[p+1])[0];if(r!==t&&!(r.length>1&&t.length>1)){m=u.slice(p+1).join(\"\\n\")+m;p=n-1}}o=q||/\\n\\n(?!\\s*$)/.test(v);if(p!==n-1){q=v.charAt(v.length-1)===\"\\n\";if(!o){o=q}}this.tokens.push({type:o?\"loose_item_start\":\"list_item_start\"});this.token(v,false);this.tokens.push({type:\"list_item_end\"})}this.tokens.push({type:\"list_end\"});continue}if(u=this.rules.html.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:this.options.sanitize?\"paragraph\":\"html\",pre:u[1]===\"pre\"||u[1]===\"script\"||u[1]===\"style\",text:u[0]});continue}if(s&&(u=this.rules.def.exec(m))){m=m.substring(u[0].length);this.tokens.links[u[1].toLowerCase()]={href:u[2],title:u[3]};continue}if(s&&(u=this.rules.table.exec(m))){m=m.substring(u[0].length);v={type:\"table\",header:u[1].replace(/^ *| *\\| *$/g,\"\").split(/ *\\| */),align:u[2].replace(/^ *|\\| *$/g,\"\").split(/ *\\| */),cells:u[3].replace(/(?: *\\| *)?\\n$/,\"\").split(\"\\n\")};for(p=0;p<v.align.length;p++){if(/^ *-+: *$/.test(v.align[p])){v.align[p]=\"right\"}else{if(/^ *:-+: *$/.test(v.align[p])){v.align[p]=\"center\"}else{if(/^ *:-+ *$/.test(v.align[p])){v.align[p]=\"left\"}else{v.align[p]=null}}}}for(p=0;p<v.cells.length;p++){v.cells[p]=v.cells[p].replace(/^ *\\| *| *\\| *$/g,\"\").split(/ *\\| */)}this.tokens.push(v);continue}if(s&&(u=this.rules.paragraph.exec(m))){m=m.substring(u[0].length);this.tokens.push({type:\"paragraph\",text:u[1].charAt(u[1].length-1)===\"\\n\"?u[1].slice(0,-1):u[1]});continue}if(u=this.rules.text.exec(m)){m=m.substring(u[0].length);this.tokens.push({type:\"text\",text:u[0]});continue}if(m){throw new Error(\"Infinite loop on byte: \"+m.charCodeAt(0))}}return this.tokens};var f={escape:/^\\\\([\\\\`*{}\\[\\]()#+\\-.!_>])/,autolink:/^<([^ >]+(@|:\\/)[^ >]+)>/,url:j,tag:/^<!--[\\s\\S]*?-->|^<\\/?\\w+(?:\"[^\"]*\"|'[^']*'|[^'\">])*?>/,link:/^!?\\[(inside)\\]\\(href\\)/,reflink:/^!?\\[(inside)\\]\\s*\\[([^\\]]*)\\]/,nolink:/^!?\\[((?:\\[[^\\]]*\\]|[^\\[\\]])*)\\]/,strong:/^__([\\s\\S]+?)__(?!_)|^\\*\\*([\\s\\S]+?)\\*\\*(?!\\*)/,em:/^\\b_((?:__|[\\s\\S])+?)_\\b|^\\*((?:\\*\\*|[\\s\\S])+?)\\*(?!\\*)/,code:/^(`+)\\s*([\\s\\S]*?[^`])\\s*\\1(?!`)/,br:/^ {2,}\\n(?!\\s*$)/,del:j,text:/^[\\s\\S]+?(?=[\\\\<!\\[_*`]| {2,}\\n|$)/};f._inside=/(?:\\[[^\\]]*\\]|[^\\[\\]]|\\](?=[^\\[]*\\]))*/;f._href=/\\s*<?([\\s\\S]*?)>?(?:\\s+['\"]([\\s\\S]*?)['\"])?\\s*/;f.link=c(f.link)(\"inside\",f._inside)(\"href\",f._href)();f.reflink=c(f.reflink)(\"inside\",f._inside)();f.normal=g({},f);f.pedantic=g({},f.normal,{strong:/^__(?=\\S)([\\s\\S]*?\\S)__(?!_)|^\\*\\*(?=\\S)([\\s\\S]*?\\S)\\*\\*(?!\\*)/,em:/^_(?=\\S)([\\s\\S]*?\\S)_(?!_)|^\\*(?=\\S)([\\s\\S]*?\\S)\\*(?!\\*)/});f.gfm=g({},f.normal,{escape:c(f.escape)(\"])\",\"~|])\")(),url:/^(https?:\\/\\/[^\\s<]+[^<.,:;\"')\\]\\s])/,del:/^~~(?=\\S)([\\s\\S]*?\\S)~~/,text:c(f.text)(\"]|\",\"~]|\")(\"|\",\"|https?://|\")()});f.breaks=g({},f.gfm,{br:c(f.br)(\"{2,}\",\"*\")(),text:c(f.gfm.text)(\"{2,}\",\"*\")()});function h(k,l){this.options=l||a.defaults;this.links=k;this.rules=f.normal;if(!this.links){throw new Error(\"Tokens array requires a `links` property.\")}if(this.options.gfm){if(this.options.breaks){this.rules=f.breaks}else{this.rules=f.gfm}}else{if(this.options.pedantic){this.rules=f.pedantic}}}h.rules=f;h.output=function(n,k,l){var m=new h(k,l);return m.output(n)};h.prototype.output=function(p){var l=\"\",n,o,k,m;while(p){if(m=this.rules.escape.exec(p)){p=p.substring(m[0].length);l+=m[1];continue}if(m=this.rules.autolink.exec(p)){p=p.substring(m[0].length);if(m[2]===\"@\"){o=m[1].charAt(6)===\":\"?this.mangle(m[1].substring(7)):this.mangle(m[1]);k=this.mangle(\"mailto:\")+o}else{o=i(m[1]);k=o}l+='<a href=\"'+k+'\">'+o+\"</a>\";continue}if(m=this.rules.url.exec(p)){p=p.substring(m[0].length);o=i(m[1]);k=o;l+='<a href=\"'+k+'\">'+o+\"</a>\";continue}if(m=this.rules.tag.exec(p)){p=p.substring(m[0].length);l+=this.options.sanitize?i(m[0]):m[0];continue}if(m=this.rules.link.exec(p)){p=p.substring(m[0].length);l+=this.outputLink(m,{href:m[2],title:m[3]});continue}if((m=this.rules.reflink.exec(p))||(m=this.rules.nolink.exec(p))){p=p.substring(m[0].length);n=(m[2]||m[1]).replace(/\\s+/g,\" \");n=this.links[n.toLowerCase()];if(!n||!n.href){l+=m[0].charAt(0);p=m[0].substring(1)+p;continue}l+=this.outputLink(m,n);continue}if(m=this.rules.strong.exec(p)){p=p.substring(m[0].length);l+=\"<strong>\"+this.output(m[2]||m[1])+\"</strong>\";continue}if(m=this.rules.em.exec(p)){p=p.substring(m[0].length);l+=\"<em>\"+this.output(m[2]||m[1])+\"</em>\";continue}if(m=this.rules.code.exec(p)){p=p.substring(m[0].length);l+=\"<code>\"+i(m[2],true)+\"</code>\";continue}if(m=this.rules.br.exec(p)){p=p.substring(m[0].length);l+=\"<br>\";continue}if(m=this.rules.del.exec(p)){p=p.substring(m[0].length);l+=\"<del>\"+this.output(m[1])+\"</del>\";continue}if(m=this.rules.text.exec(p)){p=p.substring(m[0].length);l+=i(this.smartypants(m[0]));continue}if(p){throw new Error(\"Infinite loop on byte: \"+p.charCodeAt(0))}}return l};h.prototype.outputLink=function(k,l){if(k[0].charAt(0)!==\"!\"){return'<a href=\"'+i(l.href)+'\"'+(l.title?' title=\"'+i(l.title)+'\"':\"\")+\">\"+this.output(k[1])+\"</a>\"}else{return'<img src=\"'+i(l.href)+'\" alt=\"'+i(k[1])+'\"'+(l.title?' title=\"'+i(l.title)+'\"':\"\")+\">\"}};h.prototype.smartypants=function(k){if(!this.options.smartypants){return k}return k.replace(/--/g,\"\\u2014\").replace(/(^|[-\\u2014/(\\[{\"\\s])'/g,\"$1\\u2018\").replace(/'/g,\"\\u2019\").replace(/(^|[-\\u2014/(\\[{\\u2018\\s])\"/g,\"$1\\u201c\").replace(/\"/g,\"\\u201d\").replace(/\\.{3}/g,\"\\u2026\")};h.prototype.mangle=function(p){var m=\"\",k=p.length,n=0,o;for(;n<k;n++){o=p.charCodeAt(n);if(Math.random()>0.5){o=\"x\"+o.toString(16)}m+=\"&#\"+o+\";\"}return m};function e(k){this.tokens=[];this.token=null;this.options=k||a.defaults}e.parse=function(l,k){var m=new e(k);return m.parse(l)};e.prototype.parse=function(l){this.inline=new h(l.links,this.options);this.tokens=l.reverse();var k=\"\";while(this.next()){k+=this.tok()}return k};e.prototype.next=function(){return this.token=this.tokens.pop()};e.prototype.peek=function(){return this.tokens[this.tokens.length-1]||0};e.prototype.parseText=function(){var k=this.token.text;while(this.peek().type===\"text\"){k+=\"\\n\"+this.next().text}return this.inline.output(k)};e.prototype.tok=function(){switch(this.token.type){case\"space\":return\"\";case\"hr\":return\"<hr>\\n\";case\"heading\":return\"<h\"+this.token.depth+' id=\"'+this.token.text.toLowerCase().replace(/[^\\w]+/g,\"-\")+'\">'+this.inline.output(this.token.text)+\"</h\"+this.token.depth+\">\\n\";case\"code\":if(this.options.highlight){var p=this.options.highlight(this.token.text,this.token.lang);if(p!=null&&p!==this.token.text){this.token.escaped=true;this.token.text=p}}if(!this.token.escaped){this.token.text=i(this.token.text,true)}return\"<pre><code\"+(this.token.lang?' class=\"'+this.options.langPrefix+this.token.lang+'\"':\"\")+\">\"+this.token.text+\"</code></pre>\\n\";case\"table\":var l=\"\",q,n,r,k,m;l+=\"<thead>\\n<tr>\\n\";for(n=0;n<this.token.header.length;n++){q=this.inline.output(this.token.header[n]);l+=\"<th\";if(this.token.align[n]){l+=' style=\"text-align:'+this.token.align[n]+'\"'}l+=\">\"+q+\"</th>\\n\"}l+=\"</tr>\\n</thead>\\n\";l+=\"<tbody>\\n\";for(n=0;n<this.token.cells.length;n++){r=this.token.cells[n];l+=\"<tr>\\n\";for(m=0;m<r.length;m++){k=this.inline.output(r[m]);l+=\"<td\";if(this.token.align[m]){l+=' style=\"text-align:'+this.token.align[m]+'\"'}l+=\">\"+k+\"</td>\\n\"}l+=\"</tr>\\n\"}l+=\"</tbody>\\n\";return\"<table>\\n\"+l+\"</table>\\n\";case\"blockquote_start\":var l=\"\";while(this.next().type!==\"blockquote_end\"){l+=this.tok()}return\"<blockquote>\\n\"+l+\"</blockquote>\\n\";case\"list_start\":var o=this.token.ordered?\"ol\":\"ul\",l=\"\";while(this.next().type!==\"list_end\"){l+=this.tok()}return\"<\"+o+\">\\n\"+l+\"</\"+o+\">\\n\";case\"list_item_start\":var l=\"\";while(this.next().type!==\"list_item_end\"){l+=this.token.type===\"text\"?this.parseText():this.tok()}return\"<li>\"+l+\"</li>\\n\";case\"loose_item_start\":var l=\"\";while(this.next().type!==\"list_item_end\"){l+=this.tok()}return\"<li>\"+l+\"</li>\\n\";case\"html\":return !this.token.pre&&!this.options.pedantic?this.inline.output(this.token.text):this.token.text;case\"paragraph\":return\"<p>\"+this.inline.output(this.token.text)+\"</p>\\n\";case\"text\":return\"<p>\"+this.parseText()+\"</p>\\n\"}};function i(k,l){return k.replace(!l?/&(?!#?\\w+;)/g:/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\").replace(/\"/g,\"&quot;\").replace(/'/g,\"&#39;\")}function c(m,l){m=m.source;l=l||\"\";return function k(n,o){if(!n){return new RegExp(m,l)}o=o.source||o;o=o.replace(/(^|[^\\[])\\^/g,\"$1\");m=m.replace(n,o);return k}}function j(){}j.exec=j;function g(n){var l=1,m,k;for(;l<arguments.length;l++){m=arguments[l];for(k in m){if(Object.prototype.hasOwnProperty.call(m,k)){n[k]=m[k]}}}return n}function a(k,m,s){if(s||typeof m===\"function\"){if(!s){s=m;m=null}m=g({},a.defaults,m||{});var n=m.highlight,r,l,p=0;try{r=b.lex(k,m)}catch(q){return s(q)}l=r.length;var o=function(){var t,u;try{t=e.parse(r,m)}catch(v){u=v}m.highlight=n;return u?s(u):s(null,t)};if(!n||n.length<3){return o()}delete m.highlight;if(!l){return o()}for(;p<r.length;p++){(function(t){if(t.type!==\"code\"){return --l||o()}return n(t.text,t.lang,function(v,u){if(u==null||u===t.text){return --l||o()}t.text=u;t.escaped=true;--l||o()})})(r[p])}return}try{if(m){m=g({},a.defaults,m)}return e.parse(b.lex(k,m),m)}catch(q){q.message+=\"\\nPlease report this to https://github.com/chjj/marked.\";if((m||a.defaults).silent){return\"<p>An error occured:</p><pre>\"+i(q.message+\"\",true)+\"</pre>\"}throw q}}a.options=a.setOptions=function(k){g(a.defaults,k);return a};a.defaults={gfm:true,tables:true,breaks:false,pedantic:false,sanitize:false,smartLists:false,silent:false,highlight:null,langPrefix:\"lang-\",smartypants:false};a.Parser=e;a.parser=e.parse;a.Lexer=b;a.lexer=b.lex;a.InlineLexer=h;a.inlineLexer=h.output;a.parse=a;if(typeof exports===\"object\"){module.exports=a}else{if(typeof define===\"function\"&&define.amd){define(function(){return a})}else{this.marked=a}}}).call(function(){return this||(typeof window!==\"undefined\"?window:global)}());" // NOLINT(whitespace/line_length)
      "  function loaded() {"
      "    marked.setOptions({ breaks: true });"
      "    document.body.innerHTML = marked(" + markdown + ");"
      "  }"
      "</script>"
      "<style>"
      "body {"
      "  font-family: Helvetica, arial, sans-serif;"
      "  font-size: 14px;"
      "  line-height: 1.6;"
      "  padding-top: 10px;"
      "  padding-bottom: 10px;"
      "  background-color: white;"
      "  padding: 30px;"
      "}"
      "blockquote {"
      "  border-left: 5px solid #dddddd;"
      "  padding: 0 10px;"
      "  color: #777777;"
      "  margin: 0 0 20px;"
      "}"
      "a {"
      "  color: #0088cc;"
      "  text-decoration: none;"
      "}"
      "</style>"
      "</head>"
      "<body onload=\"loaded()\">"
      "</body>"
      "</html>");
}

} // namespace process {

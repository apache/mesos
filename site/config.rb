#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
require "middleman-blog"
require "htmlentities"

set :markdown_engine, :rdiscount
set :markdown, :layout_engine => :erb,
               :tables => true,
               :autolink => true,
               :smartypants => true,
               :fenced_code_blocks => true

set :build_dir, 'publish'

set :css_dir, 'assets/css'
set :js_dir, 'assets/js'
set :images_dir, 'assets/img'
set :font_dir, 'assets/font'

configure :build do
  activate :relative_assets
end

activate :blog do |blog|
  blog.prefix = "blog"
  blog.sources = "{year}-{month}-{day}-{title}"
  blog.default_extension = ".md"
  blog.layout = "post",
  blog.permalink = ":title"
end

page "/index.html", :layout => "basic"
page "/documentation/*", :layout => "documentation"
proxy "/documentation/index.html", "/documentation/latest.html", :layout => "documentation"
latest_doc_pages = Dir.glob("./source/documentation/latest/*.md")
latest_doc_pages.each do |page_path|
  page_name = File.basename(page_path, '.md')
  proxy "/documentation/#{page_name}.html", "/documentation/latest/#{page_name}.html", :layout => "documentation"
end

page "/sitemap.xml", :layout => false

page "/blog/feed.xml", :layout => false

# Turn off directory index for API docs because it breaks links
page "/api/*", :directory_index => false

activate :directory_indexes
activate :syntax
activate :livereload

page "asf.yaml", :layout => false

after_build do
  File.rename 'publish/asf.yaml', 'publish/.asf.yaml'
end

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

param(
    [string]$username = "mesos-review-windows",
    [string]$password = $(Read-Host "Input Password"),

    # Query can be adjusted per the API:
    # https://www.reviewboard.org/docs/manual/2.5/webapi/2.0/resources/review-request-list/
    [string]$query = "?to-groups=mesos&status=pending&last-updated-from=2017-01-01T00:00:00",
    [switch]$pull,
    [string]$folder = "$PSScriptRoot/../../logs",
    [string]$url = $(Read-Host "Input URL")
)

$ErrorActionPreference = "Stop"

# Check for Python.
$null = Get-Command python

# Script lives in support, cd up a directory.
Push-Location "$PSScriptRoot/.."

# Logs are saved to this folder.
if (!(Test-Path "$folder")) {
    $null = New-Item -ItemType Directory "$folder"
}

# Run ReviewBot in a loop, saving incrementally numbered logs.
while ($true) {
    $i = [int](Get-ChildItem $folder | Sort-Object { [int]$_.Name } | Select-Object -Last 1).Name + 1
    $logfolder = "$folder/$i"
    $null = New-Item -ItemType Directory $logfolder

    # We assume the $folder is being served at url/folder (not url/path/to/folder),
    # and split here to avoid any relative paths in the url. NOTE: The trailing
    # slash must remain for Python's urlparse to include the leaf.
    $env:BUILD_URL = "$url/$(Split-Path -Leaf $folder)/$i/"
    if ($pull) {
        git pull --rebase origin master
    }

    python .\support\verify-reviews.py $username $password 1 $query 2>&1 | Tee-Object -FilePath "$logfolder/console"

    # If the log is short, we assume there was nothing to test and so delete the log.
    # Otherwise we'd end up with thousands of logs.
    if ((Get-Item "$logfolder/console").Length -lt 10000) {
        Remove-Item -Recurse $logfolder
        Start-Sleep -Seconds 300
    }
}

Pop-Location

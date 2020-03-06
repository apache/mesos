// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>

#include <sasl/md5global.h>
#include <sasl/md5.h>

#include "slave/containerizer/mesos/isolators/network/cni/utils.hpp"

void compressLongLabels(mesos::Labels * labels)
{
  MD5_CTX ctx;
  unsigned char md5[16];
  char short_label[64];

  for (int i = 0; i < labels->labels().size(); i++) {
    mesos::Label * label = labels->mutable_labels(i);
    if (label->has_value()) {
      const std::string & value = label->value();
      if (value.length() > 63) {
        const char * value_str = value.c_str();

        // Compute the MD5 sum of the string
        _sasl_MD5Init(&ctx);
        _sasl_MD5Update(&ctx, (const unsigned char *)value_str, value.length());
        _sasl_MD5Final(md5, &ctx);

        // Keep the first 7 characters of the MD5 sum and the last
        size_t ofs = value.length() - 10;
        snprintf(short_label, 64, "%02x%02x%02x%02x%02x%02x%02x...%s",
          md5[0], md5[1], md5[2], md5[3], md5[4], md5[5], md5[6],
          &value_str[ofs]
        );

        // Replace label
        label->set_value(short_label, 63);
      }
    }
  }
}

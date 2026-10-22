/**
   @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "proxy/ReverseProxy.h"
#include "shared/rpc/yaml_codecs.h"
#include "swoc/Errata.h"
#include "tsutil/DbgCtl.h"
#include "tsutil/Regex.h"
#include "yaml-cpp/node/node.h"

#include "proxy/http/remap/UrlRewrite.h"

namespace
{
DbgCtl dbg_ctl_rpc_remap{"rpc.remap"};
}

namespace rpc::handlers::remap
{

struct SearchParams {
  std::string authority;
};

swoc::Rv<YAML::Node>
remap_search(std::string_view const & /* id ATS_UNUSED */, YAML::Node const &params)
{
  swoc::Rv<YAML::Node> resp{};
  const auto           p = params.as<SearchParams>();
  Regex                auth_re{};

  auth_re.compile(p.authority);

  auto render_mappings = [&auth_re](YAML::Node &&dest, const UrlRewrite::MappingsStore &store) {
    YAML::Node s{};
    YAML::Node keys{};
    if (!store.empty()) {
      for (auto [k, tbl] : *store.hash_lookup) {
        if (auth_re.exec(k)) {
          keys.push_back(k);
        }
      }
      if (keys.IsSequence()) {
        s["keys"] = keys;
        dest      = s;
      }
    }
  };

  Dbg(dbg_ctl_rpc_remap, "Remap search: %s", p.authority.c_str());

  UrlRewrite *maps = rewrite_table->acquire();

  render_mappings(resp.result()["forward"], maps->forward_mappings());
  render_mappings(resp.result()["reverse"], maps->reverse_mappings());
  render_mappings(resp.result()["permanent_redirects"], maps->permanent_redirects());
  render_mappings(resp.result()["temporary_redirects"], maps->temporary_redirects());
  render_mappings(resp.result()["forward_mappings_with_recv_port"], maps->forward_mappings_with_recv_port());

  maps->release();

  return resp;
}
} // namespace rpc::handlers::remap

namespace YAML
{
template <> struct convert<rpc::handlers::remap::SearchParams> {
  static bool
  decode(Node const &node, rpc::handlers::remap::SearchParams &meta)
  {
    meta.authority = helper::try_extract<std::string>(node, "authority");
    return true;
  }
};
} // namespace YAML

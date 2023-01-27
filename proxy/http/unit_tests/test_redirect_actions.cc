/** @file

Unit Tests for Pre-Warming Pool Size Algorithm

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

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "tscore/Tokenizer.h"
#include "HttpConfig.h"

RedirectEnabled::Action
lookup(IpMap *m, const IpAddr &ip)
{
  intptr_t x{intptr_t(RedirectEnabled::Action::INVALID)};
  m->contains(ip, reinterpret_cast<void **>(&x));
  return static_cast<RedirectEnabled::Action>(x);
}

IpMap *
parse_redirect_actions(char *input_string, RedirectEnabled::Action &self_action)
{
  using RedirectEnabled::Action;
  using RedirectEnabled::AddressClass;
  using RedirectEnabled::action_map;
  using RedirectEnabled::address_class_map;

  if (nullptr == input_string) {
    Error("parse_redirect_actions: The configuration value is empty.");
    return nullptr;
  }
  Tokenizer configTokens(", ");
  int n_rules = configTokens.Initialize(input_string);
  std::map<AddressClass, Action> configMapping;
  for (int i = 0; i < n_rules; i++) {
    const char *rule = configTokens[i];
    Tokenizer ruleTokens(":");
    int n_mapping = ruleTokens.Initialize(rule);
    if (2 != n_mapping) {
      Error("parse_redirect_actions: Individual rules must be an address class and an action separated by a colon (:)");
      return nullptr;
    }
    std::string c_input(ruleTokens[0]), a_input(ruleTokens[1]);
    AddressClass c =
      address_class_map.find(ruleTokens[0]) != address_class_map.end() ? address_class_map[ruleTokens[0]] : AddressClass::INVALID;
    Action a = action_map.find(ruleTokens[1]) != action_map.end() ? action_map[ruleTokens[1]] : Action::INVALID;

    if (AddressClass::INVALID == c) {
      Error("parse_redirect_actions: '%.*s' is not a valid address class", static_cast<int>(c_input.size()), c_input.data());
      return nullptr;
    } else if (Action::INVALID == a) {
      Error("parse_redirect_actions: '%.*s' is not a valid action", static_cast<int>(a_input.size()), a_input.data());
      return nullptr;
    }
    configMapping[c] = a;
  }

  // Ensure the default.
  if (configMapping.end() == configMapping.find(AddressClass::DEFAULT)) {
    configMapping[AddressClass::DEFAULT] = Action::RETURN;
  }

  IpMap *ret = new IpMap();
  IpAddr min, max;
  Action action = Action::INVALID;

  // Order Matters. IpAddr::mark uses Painter's Algorithm. Last one wins.

  // PRIVATE
  action = configMapping.find(AddressClass::PRIVATE) != configMapping.end() ? configMapping[AddressClass::PRIVATE] :
                                                                              configMapping[AddressClass::DEFAULT];
  // 10.0.0.0/8
  min.load("10.0.0.0");
  max.load("10.255.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  // 100.64.0.0/10
  min.load("100.64.0.0");
  max.load("100.127.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  // 172.16.0.0/12
  min.load("172.16.0.0");
  max.load("172.31.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  // 192.168.0.0/16
  min.load("192.168.0.0");
  max.load("192.168.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  // fc00::/7
  min.load("fc00::");
  max.load("feff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
  ret->mark(min, max, reinterpret_cast<void *>(action));

  // LOOPBACK
  action = configMapping.find(AddressClass::LOOPBACK) != configMapping.end() ? configMapping[AddressClass::LOOPBACK] :
                                                                               configMapping[AddressClass::DEFAULT];
  min.load("127.0.0.0");
  max.load("127.255.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  min.load("::1");
  max.load("::1");
  ret->mark(min, max, reinterpret_cast<void *>(action));

  // MULTICAST
  action = configMapping.find(AddressClass::MULTICAST) != configMapping.end() ? configMapping[AddressClass::MULTICAST] :
                                                                                configMapping[AddressClass::DEFAULT];
  // 224.0.0.0/4
  min.load("224.0.0.0");
  max.load("239.255.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  // ff00::/8
  min.load("ff00::");
  max.load("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
  ret->mark(min, max, reinterpret_cast<void *>(action));

  // LINKLOCAL
  action = configMapping.find(AddressClass::LINKLOCAL) != configMapping.end() ? configMapping[AddressClass::LINKLOCAL] :
                                                                                configMapping[AddressClass::DEFAULT];
  // 169.254.0.0/16
  min.load("169.254.0.0");
  max.load("169.254.255.255");
  ret->mark(min, max, reinterpret_cast<void *>(action));
  // fe80::/10
  min.load("fe80::");
  max.load("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
  ret->mark(min, max, reinterpret_cast<void *>(action));

  // SELF
  // We must store the self address class separately instead of adding the addresses to our map.
  // The addresses Trafficserver will use depend on configurations that are loaded here, so they are not available yet.
  action = configMapping.find(AddressClass::SELF) != configMapping.end() ? configMapping[AddressClass::SELF] :
                                                                           configMapping[AddressClass::DEFAULT];
  self_action = action;

  // IpMap::fill only marks things that are not already marked.

  // ROUTABLE
  action = configMapping.find(AddressClass::ROUTABLE) != configMapping.end() ? configMapping[AddressClass::ROUTABLE] :
                                                                               configMapping[AddressClass::DEFAULT];
  min.load("0.0.0.0");
  max.load("255.255.255.255");
  ret->fill(min, max, reinterpret_cast<void *>(action));
  min.load("::");
  max.load("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
  ret->fill(min, max, reinterpret_cast<void *>(action));

  return ret;
}

TEST_CASE("redirect actions", "[http]")
{
  IpAddr target;
  RedirectEnabled::Action self_action = RedirectEnabled::Action::INVALID;
  char input[]                        = "routable:follow";
  auto *m                             = parse_redirect_actions(input, self_action);

  REQUIRE(m);
  // private
  target.load("10.0.10.1");
  auto action = lookup(m, target);
  REQUIRE(action == RedirectEnabled::Action::RETURN);
  REQUIRE(self_action == RedirectEnabled::Action::RETURN);

  delete m;

  char other_input[] = "routable:follow,loopback:follow,self:follow";
  m                  = parse_redirect_actions(other_input, self_action);
  target.load("10.0.10.1");
  action = lookup(m, target);
  REQUIRE(action == RedirectEnabled::Action::RETURN);
  REQUIRE(self_action == RedirectEnabled::Action::FOLLOW);
}

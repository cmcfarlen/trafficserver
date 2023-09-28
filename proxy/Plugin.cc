/** @file

  Plugin init

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

#include <cstdio>
#include "tscore/ink_platform.h"
#include "tscore/ink_file.h"
#include "tscore/ParseRules.h"
#include "records/I_RecCore.h"
#include "tscore/I_Layout.h"
#include "api/InkAPIInternal.h"
#include "Plugin.h"
#include "tscore/ink_cap.h"
#include "tscore/Filenames.h"

static PluginDynamicReloadMode plugin_dynamic_reload_mode = PluginDynamicReloadMode::RELOAD_ON;

bool
isPluginDynamicReloadEnabled()
{
  return PluginDynamicReloadMode::RELOAD_ON == plugin_dynamic_reload_mode;
}

void
parsePluginDynamicReloadConfig()
{
  int int_plugin_dynamic_reload_mode;

  REC_ReadConfigInteger(int_plugin_dynamic_reload_mode, "proxy.config.plugin.dynamic_reload_mode");
  plugin_dynamic_reload_mode = static_cast<PluginDynamicReloadMode>(int_plugin_dynamic_reload_mode);

  if (plugin_dynamic_reload_mode < 0 || plugin_dynamic_reload_mode >= PluginDynamicReloadMode::RELOAD_COUNT) {
    Warning("proxy.config.plugin.dynamic_reload_mode out of range. using default value.");
    plugin_dynamic_reload_mode = PluginDynamicReloadMode::RELOAD_ON;
  }
  Note("Initialized plugin_dynamic_reload_mode: %d", plugin_dynamic_reload_mode);
}

void
parsePluginConfig()
{
  parsePluginDynamicReloadConfig();
}

// Plugin registration vars
//
//    plugin_reg_list has an entry for each plugin
//      we've successfully been able to load
//    plugin_reg_current is used to associate the
//      plugin we're in the process of loading with
//      it struct.  We need this global pointer since
//      the API doesn't have any plugin context.  Init
//      is single threaded so we can get away with the
//      global pointer
//
DLL<PluginRegInfo> plugin_reg_list;
PluginRegInfo *plugin_reg_current = nullptr;

PluginRegInfo::PluginRegInfo() = default;

PluginRegInfo::~PluginRegInfo()
{
  // We don't support unloading plugins once they are successfully loaded, so assert
  // that we don't accidentally attempt this.
  ink_release_assert(this->plugin_registered == false);
  ink_release_assert(this->link.prev == nullptr);

  ats_free(this->plugin_path);
  ats_free(this->plugin_name);
  ats_free(this->vendor_name);
  ats_free(this->support_email);
  if (dlh) {
    dlclose(dlh);
  }
}

bool
plugin_dso_load(const char *path, void *&handle, void *&init, std::string &error)
{
  handle = dlopen(path, RTLD_NOW);
  init   = nullptr;
  if (!handle) {
    error.assign("unable to load '").append(path).append("': ").append(dlerror());
    Error("%s", error.c_str());
    return false;
  }

  init = dlsym(handle, "TSPluginInit");
  if (!init) {
    error.assign("unable to find TSPluginInit function in '").append(path).append("': ").append(dlerror());
    Error("%s", error.c_str());
    dlclose(handle);
    handle = nullptr;
    return false;
  }

  return true;
}

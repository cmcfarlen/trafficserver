/** @file

    A brief file description

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
#pragma once

#include "tscore/ink_platform.h"

#include <map>

#include "proxy/hdrs/URL.h"
#include "proxy/http/remap/UrlMapping.h"
#include "tscore/Trie.h"

class UrlMappingPathIndex
{
public:
  using UrlMappingTrie = Trie<url_mapping>;

  UrlMappingPathIndex() {}
  virtual ~UrlMappingPathIndex();
  bool         Insert(url_mapping *mapping);
  url_mapping *Search(URL *request_url, int request_port, bool normal_search = true) const;
  void         Print() const;
  std::string  PrintUrlMappingPathIndex() const;

  struct UrlMappingTrieKey {
    int scheme_wks_idx;
    int port;

    UrlMappingTrieKey(int idx, int p) : scheme_wks_idx(idx), port(p) {}
    bool
    operator<(const UrlMappingTrieKey &rhs) const
    {
      if (scheme_wks_idx == rhs.scheme_wks_idx) {
        return (port < rhs.port);
      }
      return (scheme_wks_idx < rhs.scheme_wks_idx);
    }
  };

  using UrlMappingGroup = std::map<UrlMappingTrieKey, UrlMappingTrie *>;

  class iterator
  {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type        = UrlMappingTrie::const_iterator::value_type;
    using difference_type   = ptrdiff_t;
    using pointer           = value_type *;
    using reference         = value_type &;

    iterator(UrlMappingGroup::const_iterator b, UrlMappingGroup::const_iterator e) : _it(b), _it_end(e)
    {
      if (b != e) {
        _tit     = _it->second->begin();
        _tit_end = _it->second->end();
      }
    }

    iterator &
    operator++()
    {
      next();

      return *this;
    }

    iterator
    operator++(int)
    {
      iterator result = *this;

      next();

      return result;
    }

    reference
    operator*()
    {
      return *_tit;
    }

    bool
    operator==(const iterator &o) const
    {
      return _it == o._it;
    }

    bool
    operator!=(const iterator &o) const
    {
      return _it != o._it;
    }

  private:
    void
    next()
    {
      ++_tit;
      if (_tit == _tit_end) {
        if (_it != _it_end) {
          ++_it;
          _tit     = _it->second->begin();
          _tit_end = _it->second->end();
        }
      }
    }

    UrlMappingGroup::const_iterator _it{};
    UrlMappingGroup::const_iterator _it_end{};
    UrlMappingTrie::const_iterator  _tit{};
    UrlMappingTrie::const_iterator  _tit_end{};
  };

  iterator
  begin() const
  {
    return iterator(m_tries.begin(), m_tries.end());
  }

  iterator
  end() const
  {
    return iterator(m_tries.end(), m_tries.end());
  }

private:
  UrlMappingGroup m_tries;

  // make copy-constructor and assignment operator private
  // till we properly implement them
  UrlMappingPathIndex(const UrlMappingPathIndex & /* rhs ATS_UNUSED */){};
  UrlMappingPathIndex &
  operator=(const UrlMappingPathIndex & /* rhs ATS_UNUSED */)
  {
    return *this;
  }

  inline UrlMappingTrie *
  _GetTrie(URL *url, int &idx, int port, bool search = true) const
  {
    idx = url->scheme_get_wksidx();
    // If the scheme is empty (e.g. because of a CONNECT method), guess it
    // based on port
    if (idx == -1 && url->scheme_get().empty()) {
      if (port == 80) {
        idx = URL_WKSIDX_HTTP;
      } else {
        idx = URL_WKSIDX_HTTPS;
      }
    }
    UrlMappingGroup::const_iterator group_iter;
    if (search) { // normal search
      group_iter = m_tries.find(UrlMappingTrieKey(idx, port));
    } else { // return the first trie arbitrarily
      static DbgCtl dc{"UrlMappingPathIndex::_GetTrie"};
      Dbg(dc, "Not performing search; will return first available trie");
      group_iter = m_tries.begin();
    }
    if (group_iter != m_tries.end()) {
      return group_iter->second;
    }
    return nullptr;
  }
};

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

#include "tscore/ink_defs.h"
#include "tscore/ink_platform.h"
#include "tscore/ink_memory.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include "proxy/hdrs/MIME.h"
#include "proxy/hdrs/HdrHeap.h"
#include "proxy/hdrs/HdrToken.h"
#include "proxy/hdrs/HdrUtils.h"
#include "proxy/hdrs/HttpCompat.h"

using swoc::TextView;

/***********************************************************************
 *                                                                     *
 *                    C O M P I L E    O P T I O N S                   *
 *                                                                     *
 ***********************************************************************/
#define TRACK_FIELD_FIND_CALLS            0
#define TRACK_COOKING                     0
#define MIME_FORMAT_DATE_USE_LOOKUP_TABLE 1

/***********************************************************************
 *                                                                     *
 *                          C O N S T A N T S                          *
 *                                                                     *
 ***********************************************************************/
static DFA *day_names_dfa   = nullptr;
static DFA *month_names_dfa = nullptr;

static const char *day_names[] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

static const char *month_names[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

struct MDY {
  uint8_t  m;
  uint8_t  d;
  uint16_t y;
};

static MDY   *_days_to_mdy_fast_lookup_table = nullptr;
static time_t _days_to_mdy_fast_lookup_table_first_day;
static time_t _days_to_mdy_fast_lookup_table_last_day;

/***********************************************************************
 *                                                                     *
 *                             G L O B A L S                           *
 *                                                                     *
 ***********************************************************************/
c_str_view MIME_FIELD_ACCEPT;
c_str_view MIME_FIELD_ACCEPT_CHARSET;
c_str_view MIME_FIELD_ACCEPT_ENCODING;
c_str_view MIME_FIELD_ACCEPT_LANGUAGE;
c_str_view MIME_FIELD_ACCEPT_RANGES;
c_str_view MIME_FIELD_AGE;
c_str_view MIME_FIELD_ALLOW;
c_str_view MIME_FIELD_APPROVED;
c_str_view MIME_FIELD_AUTHORIZATION;
c_str_view MIME_FIELD_BYTES;
c_str_view MIME_FIELD_CACHE_CONTROL;
c_str_view MIME_FIELD_CLIENT_IP;
c_str_view MIME_FIELD_CONNECTION;
c_str_view MIME_FIELD_CONTENT_BASE;
c_str_view MIME_FIELD_CONTENT_ENCODING;
c_str_view MIME_FIELD_CONTENT_LANGUAGE;
c_str_view MIME_FIELD_CONTENT_LENGTH;
c_str_view MIME_FIELD_CONTENT_LOCATION;
c_str_view MIME_FIELD_CONTENT_MD5;
c_str_view MIME_FIELD_CONTENT_RANGE;
c_str_view MIME_FIELD_CONTENT_TYPE;
c_str_view MIME_FIELD_CONTROL;
c_str_view MIME_FIELD_COOKIE;
c_str_view MIME_FIELD_DATE;
c_str_view MIME_FIELD_DISTRIBUTION;
c_str_view MIME_FIELD_ETAG;
c_str_view MIME_FIELD_EXPECT;
c_str_view MIME_FIELD_EXPIRES;
c_str_view MIME_FIELD_FOLLOWUP_TO;
c_str_view MIME_FIELD_FROM;
c_str_view MIME_FIELD_HOST;
c_str_view MIME_FIELD_IF_MATCH;
c_str_view MIME_FIELD_IF_MODIFIED_SINCE;
c_str_view MIME_FIELD_IF_NONE_MATCH;
c_str_view MIME_FIELD_IF_RANGE;
c_str_view MIME_FIELD_IF_UNMODIFIED_SINCE;
c_str_view MIME_FIELD_KEEP_ALIVE;
c_str_view MIME_FIELD_KEYWORDS;
c_str_view MIME_FIELD_LAST_MODIFIED;
c_str_view MIME_FIELD_LINES;
c_str_view MIME_FIELD_LOCATION;
c_str_view MIME_FIELD_MAX_FORWARDS;
c_str_view MIME_FIELD_MESSAGE_ID;
c_str_view MIME_FIELD_NEWSGROUPS;
c_str_view MIME_FIELD_ORGANIZATION;
c_str_view MIME_FIELD_PATH;
c_str_view MIME_FIELD_PRAGMA;
c_str_view MIME_FIELD_PROXY_AUTHENTICATE;
c_str_view MIME_FIELD_PROXY_AUTHORIZATION;
c_str_view MIME_FIELD_PROXY_CONNECTION;
c_str_view MIME_FIELD_PUBLIC;
c_str_view MIME_FIELD_RANGE;
c_str_view MIME_FIELD_REFERENCES;
c_str_view MIME_FIELD_REFERER;
c_str_view MIME_FIELD_REPLY_TO;
c_str_view MIME_FIELD_RETRY_AFTER;
c_str_view MIME_FIELD_SENDER;
c_str_view MIME_FIELD_SERVER;
c_str_view MIME_FIELD_SET_COOKIE;
c_str_view MIME_FIELD_STRICT_TRANSPORT_SECURITY;
c_str_view MIME_FIELD_SUBJECT;
c_str_view MIME_FIELD_SUMMARY;
c_str_view MIME_FIELD_TE;
c_str_view MIME_FIELD_TRANSFER_ENCODING;
c_str_view MIME_FIELD_UPGRADE;
c_str_view MIME_FIELD_USER_AGENT;
c_str_view MIME_FIELD_VARY;
c_str_view MIME_FIELD_VIA;
c_str_view MIME_FIELD_WARNING;
c_str_view MIME_FIELD_WWW_AUTHENTICATE;
c_str_view MIME_FIELD_XREF;
c_str_view MIME_FIELD_ATS_INTERNAL;
c_str_view MIME_FIELD_X_ID;
c_str_view MIME_FIELD_X_FORWARDED_FOR;
c_str_view MIME_FIELD_FORWARDED;
c_str_view MIME_FIELD_SEC_WEBSOCKET_KEY;
c_str_view MIME_FIELD_SEC_WEBSOCKET_VERSION;
c_str_view MIME_FIELD_HTTP2_SETTINGS;
c_str_view MIME_FIELD_EARLY_DATA;

c_str_view MIME_VALUE_BYTES;
c_str_view MIME_VALUE_CHUNKED;
c_str_view MIME_VALUE_CLOSE;
c_str_view MIME_VALUE_COMPRESS;
c_str_view MIME_VALUE_DEFLATE;
c_str_view MIME_VALUE_GZIP;
c_str_view MIME_VALUE_BROTLI;
c_str_view MIME_VALUE_IDENTITY;
c_str_view MIME_VALUE_KEEP_ALIVE;
c_str_view MIME_VALUE_MAX_AGE;
c_str_view MIME_VALUE_MAX_STALE;
c_str_view MIME_VALUE_MIN_FRESH;
c_str_view MIME_VALUE_MUST_REVALIDATE;
c_str_view MIME_VALUE_NONE;
c_str_view MIME_VALUE_NO_CACHE;
c_str_view MIME_VALUE_NO_STORE;
c_str_view MIME_VALUE_NO_TRANSFORM;
c_str_view MIME_VALUE_ONLY_IF_CACHED;
c_str_view MIME_VALUE_PRIVATE;
c_str_view MIME_VALUE_PROXY_REVALIDATE;
c_str_view MIME_VALUE_PUBLIC;
c_str_view MIME_VALUE_S_MAXAGE;
c_str_view MIME_VALUE_NEED_REVALIDATE_ONCE;
c_str_view MIME_VALUE_WEBSOCKET;
c_str_view MIME_VALUE_H2C;

// Cache-control: extension "need-revalidate-once" is used internally by T.S.
// to invalidate a document, and it is not returned/forwarded.
// If a cached document has this extension set (ie, is invalidated),
// then the T.S. needs to revalidate the document once before returning it.
// After a successful revalidation, the extension will be removed by T.S.
// To set or unset this directive should be done via the following two
// function:
//      set_cooked_cc_need_revalidate_once()
//      unset_cooked_cc_need_revalidate_once()
// To test, use regular Cache-control testing functions, eg,
//      is_cache_control_set(HTTP_VALUE_NEED_REVALIDATE_ONCE)

int MIME_WKSIDX_ACCEPT;
int MIME_WKSIDX_ACCEPT_CHARSET;
int MIME_WKSIDX_ACCEPT_ENCODING;
int MIME_WKSIDX_ACCEPT_LANGUAGE;
int MIME_WKSIDX_ACCEPT_RANGES;
int MIME_WKSIDX_AGE;
int MIME_WKSIDX_ALLOW;
int MIME_WKSIDX_APPROVED;
int MIME_WKSIDX_AUTHORIZATION;
int MIME_WKSIDX_BYTES;
int MIME_WKSIDX_CACHE_CONTROL;
int MIME_WKSIDX_CLIENT_IP;
int MIME_WKSIDX_CONNECTION;
int MIME_WKSIDX_CONTENT_BASE;
int MIME_WKSIDX_CONTENT_ENCODING;
int MIME_WKSIDX_CONTENT_LANGUAGE;
int MIME_WKSIDX_CONTENT_LENGTH;
int MIME_WKSIDX_CONTENT_LOCATION;
int MIME_WKSIDX_CONTENT_MD5;
int MIME_WKSIDX_CONTENT_RANGE;
int MIME_WKSIDX_CONTENT_TYPE;
int MIME_WKSIDX_CONTROL;
int MIME_WKSIDX_COOKIE;
int MIME_WKSIDX_DATE;
int MIME_WKSIDX_DISTRIBUTION;
int MIME_WKSIDX_ETAG;
int MIME_WKSIDX_EXPECT;
int MIME_WKSIDX_EXPIRES;
int MIME_WKSIDX_FOLLOWUP_TO;
int MIME_WKSIDX_FROM;
int MIME_WKSIDX_HOST;
int MIME_WKSIDX_IF_MATCH;
int MIME_WKSIDX_IF_MODIFIED_SINCE;
int MIME_WKSIDX_IF_NONE_MATCH;
int MIME_WKSIDX_IF_RANGE;
int MIME_WKSIDX_IF_UNMODIFIED_SINCE;
int MIME_WKSIDX_KEEP_ALIVE;
int MIME_WKSIDX_KEYWORDS;
int MIME_WKSIDX_LAST_MODIFIED;
int MIME_WKSIDX_LINES;
int MIME_WKSIDX_LOCATION;
int MIME_WKSIDX_MAX_FORWARDS;
int MIME_WKSIDX_MESSAGE_ID;
int MIME_WKSIDX_NEWSGROUPS;
int MIME_WKSIDX_ORGANIZATION;
int MIME_WKSIDX_PATH;
int MIME_WKSIDX_PRAGMA;
int MIME_WKSIDX_PROXY_AUTHENTICATE;
int MIME_WKSIDX_PROXY_AUTHORIZATION;
int MIME_WKSIDX_PROXY_CONNECTION;
int MIME_WKSIDX_PUBLIC;
int MIME_WKSIDX_RANGE;
int MIME_WKSIDX_REFERENCES;
int MIME_WKSIDX_REFERER;
int MIME_WKSIDX_REPLY_TO;
int MIME_WKSIDX_RETRY_AFTER;
int MIME_WKSIDX_SENDER;
int MIME_WKSIDX_SERVER;
int MIME_WKSIDX_SET_COOKIE;
int MIME_WKSIDX_STRICT_TRANSPORT_SECURITY;
int MIME_WKSIDX_SUBJECT;
int MIME_WKSIDX_SUMMARY;
int MIME_WKSIDX_TE;
int MIME_WKSIDX_TRANSFER_ENCODING;
int MIME_WKSIDX_UPGRADE;
int MIME_WKSIDX_USER_AGENT;
int MIME_WKSIDX_VARY;
int MIME_WKSIDX_VIA;
int MIME_WKSIDX_WARNING;
int MIME_WKSIDX_WWW_AUTHENTICATE;
int MIME_WKSIDX_XREF;
int MIME_WKSIDX_ATS_INTERNAL;
int MIME_WKSIDX_X_ID;
int MIME_WKSIDX_X_FORWARDED_FOR;
int MIME_WKSIDX_FORWARDED;
int MIME_WKSIDX_SEC_WEBSOCKET_KEY;
int MIME_WKSIDX_SEC_WEBSOCKET_VERSION;
int MIME_WKSIDX_HTTP2_SETTINGS;
int MIME_WKSIDX_EARLY_DATA;

namespace
{
DbgCtl dbg_ctl_http{"http"};

} // end anonymous namespace

/***********************************************************************
 *                                                                     *
 *                 U T I L I T Y    R O U T I N E S                    *
 *                                                                     *
 ***********************************************************************/
inline static int
is_digit(char c)
{
  return ((c <= '9') && (c >= '0'));
}

inline static int
is_ws(char c)
{
  return ((c == ParseRules::CHAR_SP) || (c == ParseRules::CHAR_HT));
}

/***********************************************************************
 *                                                                     *
 *                    P R E S E N C E    B I T S                       *
 *                                                                     *
 ***********************************************************************/
uint64_t
mime_field_presence_mask(const char *well_known_str)
{
  return hdrtoken_wks_to_mask(well_known_str);
}

uint64_t
mime_field_presence_mask(int well_known_str_index)
{
  return hdrtoken_index_to_mask(well_known_str_index);
}

int
mime_field_presence_get(MIMEHdrImpl *h, const char *well_known_str)
{
  uint64_t mask = mime_field_presence_mask(well_known_str);
  return ((mask == 0) ? 1 : ((h->m_presence_bits & mask) == 0 ? 0 : 1));
}

int
mime_field_presence_get(MIMEHdrImpl *h, int well_known_str_index)
{
  const char *wks = hdrtoken_index_to_wks(well_known_str_index);
  return mime_field_presence_get(h, wks);
}

void
mime_hdr_presence_set(MIMEHdrImpl *h, const char *well_known_str)
{
  uint64_t mask = mime_field_presence_mask(well_known_str);
  if (mask != 0) {
    h->m_presence_bits |= mask;
  }
}

void
mime_hdr_presence_set(MIMEHdrImpl *h, int well_known_str_index)
{
  const char *wks = hdrtoken_index_to_wks(well_known_str_index);
  mime_hdr_presence_set(h, wks);
}

void
mime_hdr_presence_unset(MIMEHdrImpl *h, const char *well_known_str)
{
  uint64_t mask = mime_field_presence_mask(well_known_str);
  if (mask != 0) {
    h->m_presence_bits &= (~mask);
  }
}

void
mime_hdr_presence_unset(MIMEHdrImpl *h, int well_known_str_index)
{
  const char *wks = hdrtoken_index_to_wks(well_known_str_index);
  mime_hdr_presence_unset(h, wks);
}

/***********************************************************************
 *                                                                     *
 *                  S L O T    A C C E L E R A T O R S                 *
 *                                                                     *
 ***********************************************************************/
inline void
mime_hdr_init_accelerators_and_presence_bits(MIMEHdrImpl *mh)
{
  mh->m_presence_bits        = 0;
  mh->m_slot_accelerators[0] = 0xFFFFFFFF;
  mh->m_slot_accelerators[1] = 0xFFFFFFFF;
  mh->m_slot_accelerators[2] = 0xFFFFFFFF;
  mh->m_slot_accelerators[3] = 0xFFFFFFFF;
}

inline uint32_t
mime_hdr_get_accelerator_slotnum(MIMEHdrImpl *mh, int32_t slot_id)
{
  ink_assert((slot_id != MIME_SLOTID_NONE) && (slot_id < 32));

  uint32_t word_index = slot_id / 8;                         // 4 words of 8 slots
  uint32_t word       = mh->m_slot_accelerators[word_index]; // 8 slots of 4 bits each
  uint32_t nybble     = slot_id % 8;                         // which of the 8 nybbles?
  uint32_t slot       = ((word >> (nybble * 4)) & 15);       // grab the 4 bit slotnum
  return slot;
}

inline void
mime_hdr_set_accelerator_slotnum(MIMEHdrImpl *mh, int32_t slot_id, uint32_t slot_num)
{
  ink_assert((slot_id != MIME_SLOTID_NONE) && (slot_id < 32));
  ink_assert(slot_num < 16);

  uint32_t word_index = slot_id / 8;                         // 4 words of 8 slots
  uint32_t word       = mh->m_slot_accelerators[word_index]; // 8 slots of 4 bits each
  uint32_t nybble     = slot_id % 8;                         // which of the 8 nybbles?
  uint32_t shift      = nybble * 4;                          // shift in chunks of 4 bits
  uint32_t mask       = ~(MIME_FIELD_SLOTNUM_MASK << shift); // mask to zero out old slot
  uint32_t graft      = (slot_num << shift);                 // plug to insert into slot
  uint32_t new_word   = (word & mask) | graft;               // new value

  mh->m_slot_accelerators[word_index] = new_word;
}

inline void
mime_hdr_set_accelerators_and_presence_bits(MIMEHdrImpl *mh, MIMEField *field)
{
  int       slot_id;
  ptrdiff_t slot_num;
  if (field->m_wks_idx < 0) {
    return;
  }

  ink_assert(mh);

  mime_hdr_presence_set(mh, field->m_wks_idx);

  slot_id = hdrtoken_index_to_slotid(field->m_wks_idx);
  if (slot_id != MIME_SLOTID_NONE) {
    if (mh->m_first_fblock.contains(field)) {
      slot_num = (field - &(mh->m_first_fblock.m_field_slots[0]));
      // constains() assure that the field is in the block, and the calculated
      // slot_num will be between 0 and 15, which seem valid.
      // However, strangely, this function regards slot number 14 and 15 as
      // unknown for some reason that is not clear. It might be a bug.
      // The block below is left to keep the original behavior. See also TS-4316.
      if (slot_num >= MIME_FIELD_SLOTNUM_UNKNOWN) {
        slot_num = MIME_FIELD_SLOTNUM_UNKNOWN;
      }
    } else {
      slot_num = MIME_FIELD_SLOTNUM_UNKNOWN;
    }
    mime_hdr_set_accelerator_slotnum(mh, slot_id, slot_num);
  }
}

inline void
mime_hdr_unset_accelerators_and_presence_bits(MIMEHdrImpl *mh, MIMEField *field)
{
  int slot_id;
  if (field->m_wks_idx < 0) {
    return;
  }

  mime_hdr_presence_unset(mh, field->m_wks_idx);

  slot_id = hdrtoken_index_to_slotid(field->m_wks_idx);
  if (slot_id != MIME_SLOTID_NONE) {
    mime_hdr_set_accelerator_slotnum(mh, slot_id, MIME_FIELD_SLOTNUM_MAX);
  }
}

/// Reset data in the header.
/// Clear all the presence bits and accelerators.
/// Update all the m_wks_idx values, presence bits and accelerators.
inline void
mime_hdr_reset_accelerators_and_presence_bits(MIMEHdrImpl *mh)
{
  mime_hdr_init_accelerators_and_presence_bits(mh);

  for (MIMEFieldBlockImpl *fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    for (MIMEField *field = fblock->m_field_slots, *limit = field + fblock->m_freetop; field < limit; ++field) {
      if (field->is_live()) {
        field->m_wks_idx = hdrtoken_tokenize(field->m_ptr_name, field->m_len_name);
        if (field->is_dup_head()) {
          mime_hdr_set_accelerators_and_presence_bits(mh, field);
        }
      }
    }
  }
}

int
checksum_block(const char *s, int len)
{
  int sum = 0;
  while (len--) {
    sum ^= *s++;
  }
  return sum;
}

#ifdef ENABLE_MIME_SANITY_CHECK
void
mime_hdr_sanity_check(MIMEHdrImpl *mh)
{
  MIMEFieldBlockImpl *fblock, *blk, *last_fblock;
  MIMEField          *field, *next_dup;
  uint32_t            slot_index, index;
  uint64_t            masksum;

  ink_assert(mh != nullptr);

  masksum     = 0;
  slot_index  = 0;
  last_fblock = nullptr;

  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    for (index = 0; index < fblock->m_freetop; index++) {
      field = &(fblock->m_field_slots[index]);

      if (field->is_live()) {
        // dummy operations just to make sure deref doesn't crash
        checksum_block(field->m_ptr_name, field->m_len_name);
        if (field->m_ptr_value) {
          checksum_block(field->m_ptr_value, field->m_len_value);
        }

        if (field->m_n_v_raw_printable) {
          int total_len = field->m_len_name + field->m_len_value + field->m_n_v_raw_printable_pad;
          checksum_block(field->m_ptr_name, total_len);
        }
        // walk the dup list, quickly checking each cell
        if (field->m_next_dup != nullptr) {
          int field_slotnum = mime_hdr_field_slotnum(mh, field);

          for (next_dup = field->m_next_dup; next_dup; next_dup = next_dup->m_next_dup) {
            int next_slotnum = mime_hdr_field_slotnum(mh, next_dup);
            ink_release_assert((next_dup->m_flags & MIME_FIELD_SLOT_FLAGS_DUP_HEAD) == 0);
            ink_release_assert((next_dup->m_readiness == MIME_FIELD_SLOT_READINESS_LIVE));
            ink_release_assert(next_dup->m_wks_idx == field->m_wks_idx);
            ink_release_assert(next_dup->m_len_name == field->m_len_name);
            ink_release_assert(strncasecmp(field->m_ptr_name, next_dup->m_ptr_name, field->m_len_name) == 0);
            ink_release_assert(next_slotnum > field_slotnum);
          }
        }
        // if this is a well known string, check presence bits & slot accelerators
        if (field->m_wks_idx >= 0) {
          const char *wks = hdrtoken_index_to_wks(field->m_wks_idx);
          int         len = hdrtoken_index_to_length(field->m_wks_idx);

          if (field->m_len_name != len || strncasecmp(field->m_ptr_name, wks, field->m_len_name) != 0) {
            Warning("Encountered WKS hash collision on '%.*s'", field->m_len_name, field->m_ptr_name);
          }

          uint64_t mask  = mime_field_presence_mask(field->m_wks_idx);
          masksum       |= mask;

          int32_t slot_id = hdrtoken_index_to_slotid(field->m_wks_idx);
          if ((slot_id != MIME_SLOTID_NONE) && (slot_index < MIME_FIELD_SLOTNUM_UNKNOWN) &&
              (field->m_flags & MIME_FIELD_SLOT_FLAGS_DUP_HEAD)) {
            uint32_t slot_num = mime_hdr_get_accelerator_slotnum(mh, slot_id);
            if (slot_num <= 14) {
              ink_release_assert(slot_num == slot_index);
            }
          }
        } else {
          int idx = hdrtoken_tokenize(field->m_ptr_name, field->m_len_name, nullptr);
          ink_release_assert(idx < 0);
        }

        // verify that the next dup pointer points to a block in this list
        if (field->m_next_dup) {
          bool found = false;
          for (blk = &(mh->m_first_fblock); blk != nullptr; blk = blk->m_next) {
            const char *addr = reinterpret_cast<const char *>(field->m_next_dup);
            if ((addr >= reinterpret_cast<const char *>(blk)) &&
                (addr < reinterpret_cast<const char *>(blk) + sizeof(MIMEFieldBlockImpl))) {
              found = true;
              break;
            }
          }
          ink_release_assert(found);
        }
        // re-find the field --- should always find the head dup
        MIMEField *mf = mime_hdr_field_find(mh, field->m_ptr_name, field->m_len_name);
        ink_release_assert(mf != nullptr);
        if (mf == field) {
          ink_release_assert((field->m_flags & MIME_FIELD_SLOT_FLAGS_DUP_HEAD) != 0);
        } else {
          ink_release_assert((field->m_flags & MIME_FIELD_SLOT_FLAGS_DUP_HEAD) == 0);
        }
      }

      ++slot_index;
    }
    last_fblock = fblock;
  }

  ink_release_assert(last_fblock == mh->m_fblock_list_tail);
  ink_release_assert(masksum == mh->m_presence_bits);
}
#endif

void
mime_init()
{
  static int init = 1;

  if (init) {
    init = 0;

    hdrtoken_init();
    day_names_dfa = new DFA;
    day_names_dfa->compile(day_names, SIZEOF(day_names), RE_CASE_INSENSITIVE);

    month_names_dfa = new DFA;
    month_names_dfa->compile(month_names, SIZEOF(month_names), RE_CASE_INSENSITIVE);

    MIME_FIELD_ACCEPT                    = hdrtoken_string_to_wks_sv("Accept");
    MIME_FIELD_ACCEPT_CHARSET            = hdrtoken_string_to_wks_sv("Accept-Charset");
    MIME_FIELD_ACCEPT_ENCODING           = hdrtoken_string_to_wks_sv("Accept-Encoding");
    MIME_FIELD_ACCEPT_LANGUAGE           = hdrtoken_string_to_wks_sv("Accept-Language");
    MIME_FIELD_ACCEPT_RANGES             = hdrtoken_string_to_wks_sv("Accept-Ranges");
    MIME_FIELD_AGE                       = hdrtoken_string_to_wks_sv("Age");
    MIME_FIELD_ALLOW                     = hdrtoken_string_to_wks_sv("Allow");
    MIME_FIELD_APPROVED                  = hdrtoken_string_to_wks_sv("Approved");
    MIME_FIELD_AUTHORIZATION             = hdrtoken_string_to_wks_sv("Authorization");
    MIME_FIELD_BYTES                     = hdrtoken_string_to_wks_sv("Bytes");
    MIME_FIELD_CACHE_CONTROL             = hdrtoken_string_to_wks_sv("Cache-Control");
    MIME_FIELD_CLIENT_IP                 = hdrtoken_string_to_wks_sv("Client-ip");
    MIME_FIELD_CONNECTION                = hdrtoken_string_to_wks_sv("Connection");
    MIME_FIELD_CONTENT_BASE              = hdrtoken_string_to_wks_sv("Content-Base");
    MIME_FIELD_CONTENT_ENCODING          = hdrtoken_string_to_wks_sv("Content-Encoding");
    MIME_FIELD_CONTENT_LANGUAGE          = hdrtoken_string_to_wks_sv("Content-Language");
    MIME_FIELD_CONTENT_LENGTH            = hdrtoken_string_to_wks_sv("Content-Length");
    MIME_FIELD_CONTENT_LOCATION          = hdrtoken_string_to_wks_sv("Content-Location");
    MIME_FIELD_CONTENT_MD5               = hdrtoken_string_to_wks_sv("Content-MD5");
    MIME_FIELD_CONTENT_RANGE             = hdrtoken_string_to_wks_sv("Content-Range");
    MIME_FIELD_CONTENT_TYPE              = hdrtoken_string_to_wks_sv("Content-Type");
    MIME_FIELD_CONTROL                   = hdrtoken_string_to_wks_sv("Control");
    MIME_FIELD_COOKIE                    = hdrtoken_string_to_wks_sv("Cookie");
    MIME_FIELD_DATE                      = hdrtoken_string_to_wks_sv("Date");
    MIME_FIELD_DISTRIBUTION              = hdrtoken_string_to_wks_sv("Distribution");
    MIME_FIELD_ETAG                      = hdrtoken_string_to_wks_sv("Etag");
    MIME_FIELD_EXPECT                    = hdrtoken_string_to_wks_sv("Expect");
    MIME_FIELD_EXPIRES                   = hdrtoken_string_to_wks_sv("Expires");
    MIME_FIELD_FOLLOWUP_TO               = hdrtoken_string_to_wks_sv("Followup-To");
    MIME_FIELD_FROM                      = hdrtoken_string_to_wks_sv("From");
    MIME_FIELD_HOST                      = hdrtoken_string_to_wks_sv("Host");
    MIME_FIELD_IF_MATCH                  = hdrtoken_string_to_wks_sv("If-Match");
    MIME_FIELD_IF_MODIFIED_SINCE         = hdrtoken_string_to_wks_sv("If-Modified-Since");
    MIME_FIELD_IF_NONE_MATCH             = hdrtoken_string_to_wks_sv("If-None-Match");
    MIME_FIELD_IF_RANGE                  = hdrtoken_string_to_wks_sv("If-Range");
    MIME_FIELD_IF_UNMODIFIED_SINCE       = hdrtoken_string_to_wks_sv("If-Unmodified-Since");
    MIME_FIELD_KEEP_ALIVE                = hdrtoken_string_to_wks_sv("Keep-Alive");
    MIME_FIELD_KEYWORDS                  = hdrtoken_string_to_wks_sv("Keywords");
    MIME_FIELD_LAST_MODIFIED             = hdrtoken_string_to_wks_sv("Last-Modified");
    MIME_FIELD_LINES                     = hdrtoken_string_to_wks_sv("Lines");
    MIME_FIELD_LOCATION                  = hdrtoken_string_to_wks_sv("Location");
    MIME_FIELD_MAX_FORWARDS              = hdrtoken_string_to_wks_sv("Max-Forwards");
    MIME_FIELD_MESSAGE_ID                = hdrtoken_string_to_wks_sv("Message-ID");
    MIME_FIELD_NEWSGROUPS                = hdrtoken_string_to_wks_sv("Newsgroups");
    MIME_FIELD_ORGANIZATION              = hdrtoken_string_to_wks_sv("Organization");
    MIME_FIELD_PATH                      = hdrtoken_string_to_wks_sv("Path");
    MIME_FIELD_PRAGMA                    = hdrtoken_string_to_wks_sv("Pragma");
    MIME_FIELD_PROXY_AUTHENTICATE        = hdrtoken_string_to_wks_sv("Proxy-Authenticate");
    MIME_FIELD_PROXY_AUTHORIZATION       = hdrtoken_string_to_wks_sv("Proxy-Authorization");
    MIME_FIELD_PROXY_CONNECTION          = hdrtoken_string_to_wks_sv("Proxy-Connection");
    MIME_FIELD_PUBLIC                    = hdrtoken_string_to_wks_sv("Public");
    MIME_FIELD_RANGE                     = hdrtoken_string_to_wks_sv("Range");
    MIME_FIELD_REFERENCES                = hdrtoken_string_to_wks_sv("References");
    MIME_FIELD_REFERER                   = hdrtoken_string_to_wks_sv("Referer");
    MIME_FIELD_REPLY_TO                  = hdrtoken_string_to_wks_sv("Reply-To");
    MIME_FIELD_RETRY_AFTER               = hdrtoken_string_to_wks_sv("Retry-After");
    MIME_FIELD_SENDER                    = hdrtoken_string_to_wks_sv("Sender");
    MIME_FIELD_SERVER                    = hdrtoken_string_to_wks_sv("Server");
    MIME_FIELD_SET_COOKIE                = hdrtoken_string_to_wks_sv("Set-Cookie");
    MIME_FIELD_STRICT_TRANSPORT_SECURITY = hdrtoken_string_to_wks_sv("Strict-Transport-Security");
    MIME_FIELD_SUBJECT                   = hdrtoken_string_to_wks_sv("Subject");
    MIME_FIELD_SUMMARY                   = hdrtoken_string_to_wks_sv("Summary");
    MIME_FIELD_TE                        = hdrtoken_string_to_wks_sv("TE");
    MIME_FIELD_TRANSFER_ENCODING         = hdrtoken_string_to_wks_sv("Transfer-Encoding");
    MIME_FIELD_UPGRADE                   = hdrtoken_string_to_wks_sv("Upgrade");
    MIME_FIELD_USER_AGENT                = hdrtoken_string_to_wks_sv("User-Agent");
    MIME_FIELD_VARY                      = hdrtoken_string_to_wks_sv("Vary");
    MIME_FIELD_VIA                       = hdrtoken_string_to_wks_sv("Via");
    MIME_FIELD_WARNING                   = hdrtoken_string_to_wks_sv("Warning");
    MIME_FIELD_WWW_AUTHENTICATE          = hdrtoken_string_to_wks_sv("Www-Authenticate");
    MIME_FIELD_XREF                      = hdrtoken_string_to_wks_sv("Xref");
    MIME_FIELD_ATS_INTERNAL              = hdrtoken_string_to_wks_sv("@Ats-Internal");
    MIME_FIELD_X_ID                      = hdrtoken_string_to_wks_sv("X-ID");
    MIME_FIELD_X_FORWARDED_FOR           = hdrtoken_string_to_wks_sv("X-Forwarded-For");
    MIME_FIELD_FORWARDED                 = hdrtoken_string_to_wks_sv("Forwarded");
    MIME_FIELD_SEC_WEBSOCKET_KEY         = hdrtoken_string_to_wks_sv("Sec-WebSocket-Key");
    MIME_FIELD_SEC_WEBSOCKET_VERSION     = hdrtoken_string_to_wks_sv("Sec-WebSocket-Version");
    MIME_FIELD_HTTP2_SETTINGS            = hdrtoken_string_to_wks_sv("HTTP2-Settings");
    MIME_FIELD_EARLY_DATA                = hdrtoken_string_to_wks_sv("Early-Data");

    MIME_WKSIDX_ACCEPT                    = hdrtoken_wks_to_index(MIME_FIELD_ACCEPT.c_str());
    MIME_WKSIDX_ACCEPT_CHARSET            = hdrtoken_wks_to_index(MIME_FIELD_ACCEPT_CHARSET.c_str());
    MIME_WKSIDX_ACCEPT_ENCODING           = hdrtoken_wks_to_index(MIME_FIELD_ACCEPT_ENCODING.c_str());
    MIME_WKSIDX_ACCEPT_LANGUAGE           = hdrtoken_wks_to_index(MIME_FIELD_ACCEPT_LANGUAGE.c_str());
    MIME_WKSIDX_ACCEPT_RANGES             = hdrtoken_wks_to_index(MIME_FIELD_ACCEPT_RANGES.c_str());
    MIME_WKSIDX_AGE                       = hdrtoken_wks_to_index(MIME_FIELD_AGE.c_str());
    MIME_WKSIDX_ALLOW                     = hdrtoken_wks_to_index(MIME_FIELD_ALLOW.c_str());
    MIME_WKSIDX_APPROVED                  = hdrtoken_wks_to_index(MIME_FIELD_APPROVED.c_str());
    MIME_WKSIDX_AUTHORIZATION             = hdrtoken_wks_to_index(MIME_FIELD_AUTHORIZATION.c_str());
    MIME_WKSIDX_BYTES                     = hdrtoken_wks_to_index(MIME_FIELD_BYTES.c_str());
    MIME_WKSIDX_CACHE_CONTROL             = hdrtoken_wks_to_index(MIME_FIELD_CACHE_CONTROL.c_str());
    MIME_WKSIDX_CLIENT_IP                 = hdrtoken_wks_to_index(MIME_FIELD_CLIENT_IP.c_str());
    MIME_WKSIDX_CONNECTION                = hdrtoken_wks_to_index(MIME_FIELD_CONNECTION.c_str());
    MIME_WKSIDX_CONTENT_BASE              = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_BASE.c_str());
    MIME_WKSIDX_CONTENT_ENCODING          = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_ENCODING.c_str());
    MIME_WKSIDX_CONTENT_LANGUAGE          = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_LANGUAGE.c_str());
    MIME_WKSIDX_CONTENT_LENGTH            = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_LENGTH.c_str());
    MIME_WKSIDX_CONTENT_LOCATION          = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_LOCATION.c_str());
    MIME_WKSIDX_CONTENT_MD5               = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_MD5.c_str());
    MIME_WKSIDX_CONTENT_RANGE             = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_RANGE.c_str());
    MIME_WKSIDX_CONTENT_TYPE              = hdrtoken_wks_to_index(MIME_FIELD_CONTENT_TYPE.c_str());
    MIME_WKSIDX_CONTROL                   = hdrtoken_wks_to_index(MIME_FIELD_CONTROL.c_str());
    MIME_WKSIDX_COOKIE                    = hdrtoken_wks_to_index(MIME_FIELD_COOKIE.c_str());
    MIME_WKSIDX_DATE                      = hdrtoken_wks_to_index(MIME_FIELD_DATE.c_str());
    MIME_WKSIDX_DISTRIBUTION              = hdrtoken_wks_to_index(MIME_FIELD_DISTRIBUTION.c_str());
    MIME_WKSIDX_ETAG                      = hdrtoken_wks_to_index(MIME_FIELD_ETAG.c_str());
    MIME_WKSIDX_EXPECT                    = hdrtoken_wks_to_index(MIME_FIELD_EXPECT.c_str());
    MIME_WKSIDX_EXPIRES                   = hdrtoken_wks_to_index(MIME_FIELD_EXPIRES.c_str());
    MIME_WKSIDX_FOLLOWUP_TO               = hdrtoken_wks_to_index(MIME_FIELD_FOLLOWUP_TO.c_str());
    MIME_WKSIDX_FROM                      = hdrtoken_wks_to_index(MIME_FIELD_FROM.c_str());
    MIME_WKSIDX_HOST                      = hdrtoken_wks_to_index(MIME_FIELD_HOST.c_str());
    MIME_WKSIDX_IF_MATCH                  = hdrtoken_wks_to_index(MIME_FIELD_IF_MATCH.c_str());
    MIME_WKSIDX_IF_MODIFIED_SINCE         = hdrtoken_wks_to_index(MIME_FIELD_IF_MODIFIED_SINCE.c_str());
    MIME_WKSIDX_IF_NONE_MATCH             = hdrtoken_wks_to_index(MIME_FIELD_IF_NONE_MATCH.c_str());
    MIME_WKSIDX_IF_RANGE                  = hdrtoken_wks_to_index(MIME_FIELD_IF_RANGE.c_str());
    MIME_WKSIDX_IF_UNMODIFIED_SINCE       = hdrtoken_wks_to_index(MIME_FIELD_IF_UNMODIFIED_SINCE.c_str());
    MIME_WKSIDX_KEEP_ALIVE                = hdrtoken_wks_to_index(MIME_FIELD_KEEP_ALIVE.c_str());
    MIME_WKSIDX_KEYWORDS                  = hdrtoken_wks_to_index(MIME_FIELD_KEYWORDS.c_str());
    MIME_WKSIDX_LAST_MODIFIED             = hdrtoken_wks_to_index(MIME_FIELD_LAST_MODIFIED.c_str());
    MIME_WKSIDX_LINES                     = hdrtoken_wks_to_index(MIME_FIELD_LINES.c_str());
    MIME_WKSIDX_LOCATION                  = hdrtoken_wks_to_index(MIME_FIELD_LOCATION.c_str());
    MIME_WKSIDX_MAX_FORWARDS              = hdrtoken_wks_to_index(MIME_FIELD_MAX_FORWARDS.c_str());
    MIME_WKSIDX_MESSAGE_ID                = hdrtoken_wks_to_index(MIME_FIELD_MESSAGE_ID.c_str());
    MIME_WKSIDX_NEWSGROUPS                = hdrtoken_wks_to_index(MIME_FIELD_NEWSGROUPS.c_str());
    MIME_WKSIDX_ORGANIZATION              = hdrtoken_wks_to_index(MIME_FIELD_ORGANIZATION.c_str());
    MIME_WKSIDX_PATH                      = hdrtoken_wks_to_index(MIME_FIELD_PATH.c_str());
    MIME_WKSIDX_PRAGMA                    = hdrtoken_wks_to_index(MIME_FIELD_PRAGMA.c_str());
    MIME_WKSIDX_PROXY_AUTHENTICATE        = hdrtoken_wks_to_index(MIME_FIELD_PROXY_AUTHENTICATE.c_str());
    MIME_WKSIDX_PROXY_AUTHORIZATION       = hdrtoken_wks_to_index(MIME_FIELD_PROXY_AUTHORIZATION.c_str());
    MIME_WKSIDX_PROXY_CONNECTION          = hdrtoken_wks_to_index(MIME_FIELD_PROXY_CONNECTION.c_str());
    MIME_WKSIDX_PUBLIC                    = hdrtoken_wks_to_index(MIME_FIELD_PUBLIC.c_str());
    MIME_WKSIDX_RANGE                     = hdrtoken_wks_to_index(MIME_FIELD_RANGE.c_str());
    MIME_WKSIDX_REFERENCES                = hdrtoken_wks_to_index(MIME_FIELD_REFERENCES.c_str());
    MIME_WKSIDX_REFERER                   = hdrtoken_wks_to_index(MIME_FIELD_REFERER.c_str());
    MIME_WKSIDX_REPLY_TO                  = hdrtoken_wks_to_index(MIME_FIELD_REPLY_TO.c_str());
    MIME_WKSIDX_RETRY_AFTER               = hdrtoken_wks_to_index(MIME_FIELD_RETRY_AFTER.c_str());
    MIME_WKSIDX_SENDER                    = hdrtoken_wks_to_index(MIME_FIELD_SENDER.c_str());
    MIME_WKSIDX_SERVER                    = hdrtoken_wks_to_index(MIME_FIELD_SERVER.c_str());
    MIME_WKSIDX_SET_COOKIE                = hdrtoken_wks_to_index(MIME_FIELD_SET_COOKIE.c_str());
    MIME_WKSIDX_STRICT_TRANSPORT_SECURITY = hdrtoken_wks_to_index(MIME_FIELD_STRICT_TRANSPORT_SECURITY.c_str());
    MIME_WKSIDX_SUBJECT                   = hdrtoken_wks_to_index(MIME_FIELD_SUBJECT.c_str());
    MIME_WKSIDX_SUMMARY                   = hdrtoken_wks_to_index(MIME_FIELD_SUMMARY.c_str());
    MIME_WKSIDX_TE                        = hdrtoken_wks_to_index(MIME_FIELD_TE.c_str());
    MIME_WKSIDX_TRANSFER_ENCODING         = hdrtoken_wks_to_index(MIME_FIELD_TRANSFER_ENCODING.c_str());
    MIME_WKSIDX_UPGRADE                   = hdrtoken_wks_to_index(MIME_FIELD_UPGRADE.c_str());
    MIME_WKSIDX_USER_AGENT                = hdrtoken_wks_to_index(MIME_FIELD_USER_AGENT.c_str());
    MIME_WKSIDX_VARY                      = hdrtoken_wks_to_index(MIME_FIELD_VARY.c_str());
    MIME_WKSIDX_VIA                       = hdrtoken_wks_to_index(MIME_FIELD_VIA.c_str());
    MIME_WKSIDX_WARNING                   = hdrtoken_wks_to_index(MIME_FIELD_WARNING.c_str());
    MIME_WKSIDX_WWW_AUTHENTICATE          = hdrtoken_wks_to_index(MIME_FIELD_WWW_AUTHENTICATE.c_str());
    MIME_WKSIDX_XREF                      = hdrtoken_wks_to_index(MIME_FIELD_XREF.c_str());
    MIME_WKSIDX_X_ID                      = hdrtoken_wks_to_index(MIME_FIELD_X_ID.c_str());
    MIME_WKSIDX_X_FORWARDED_FOR           = hdrtoken_wks_to_index(MIME_FIELD_X_FORWARDED_FOR.c_str());
    MIME_WKSIDX_FORWARDED                 = hdrtoken_wks_to_index(MIME_FIELD_FORWARDED.c_str());
    MIME_WKSIDX_SEC_WEBSOCKET_KEY         = hdrtoken_wks_to_index(MIME_FIELD_SEC_WEBSOCKET_KEY.c_str());
    MIME_WKSIDX_SEC_WEBSOCKET_VERSION     = hdrtoken_wks_to_index(MIME_FIELD_SEC_WEBSOCKET_VERSION.c_str());
    MIME_WKSIDX_HTTP2_SETTINGS            = hdrtoken_wks_to_index(MIME_FIELD_HTTP2_SETTINGS.c_str());
    MIME_WKSIDX_EARLY_DATA                = hdrtoken_wks_to_index(MIME_FIELD_EARLY_DATA.c_str());

    MIME_VALUE_BYTES                = hdrtoken_string_to_wks_sv("bytes");
    MIME_VALUE_CHUNKED              = hdrtoken_string_to_wks_sv("chunked");
    MIME_VALUE_CLOSE                = hdrtoken_string_to_wks_sv("close");
    MIME_VALUE_COMPRESS             = hdrtoken_string_to_wks_sv("compress");
    MIME_VALUE_DEFLATE              = hdrtoken_string_to_wks_sv("deflate");
    MIME_VALUE_GZIP                 = hdrtoken_string_to_wks_sv("gzip");
    MIME_VALUE_BROTLI               = hdrtoken_string_to_wks_sv("br");
    MIME_VALUE_IDENTITY             = hdrtoken_string_to_wks_sv("identity");
    MIME_VALUE_KEEP_ALIVE           = hdrtoken_string_to_wks_sv("keep-alive");
    MIME_VALUE_MAX_AGE              = hdrtoken_string_to_wks_sv("max-age");
    MIME_VALUE_MAX_STALE            = hdrtoken_string_to_wks_sv("max-stale");
    MIME_VALUE_MIN_FRESH            = hdrtoken_string_to_wks_sv("min-fresh");
    MIME_VALUE_MUST_REVALIDATE      = hdrtoken_string_to_wks_sv("must-revalidate");
    MIME_VALUE_NONE                 = hdrtoken_string_to_wks_sv("none");
    MIME_VALUE_NO_CACHE             = hdrtoken_string_to_wks_sv("no-cache");
    MIME_VALUE_NO_STORE             = hdrtoken_string_to_wks_sv("no-store");
    MIME_VALUE_NO_TRANSFORM         = hdrtoken_string_to_wks_sv("no-transform");
    MIME_VALUE_ONLY_IF_CACHED       = hdrtoken_string_to_wks_sv("only-if-cached");
    MIME_VALUE_PRIVATE              = hdrtoken_string_to_wks_sv("private");
    MIME_VALUE_PROXY_REVALIDATE     = hdrtoken_string_to_wks_sv("proxy-revalidate");
    MIME_VALUE_PUBLIC               = hdrtoken_string_to_wks_sv("public");
    MIME_VALUE_S_MAXAGE             = hdrtoken_string_to_wks_sv("s-maxage");
    MIME_VALUE_NEED_REVALIDATE_ONCE = hdrtoken_string_to_wks_sv("need-revalidate-once");
    MIME_VALUE_WEBSOCKET            = hdrtoken_string_to_wks_sv("websocket");
    MIME_VALUE_H2C                  = hdrtoken_string_to_wks_sv(MIME_UPGRADE_H2C_TOKEN);

    mime_init_date_format_table();
    mime_init_cache_control_cooking_masks();
  }
}

void
mime_init_cache_control_cooking_masks()
{
  static struct {
    const char *name;
    uint32_t    mask;
  } cc_mask_table[] = {
    {"max-age",              MIME_COOKED_MASK_CC_MAX_AGE             },
    {"no-cache",             MIME_COOKED_MASK_CC_NO_CACHE            },
    {"no-store",             MIME_COOKED_MASK_CC_NO_STORE            },
    {"no-transform",         MIME_COOKED_MASK_CC_NO_TRANSFORM        },
    {"max-stale",            MIME_COOKED_MASK_CC_MAX_STALE           },
    {"min-fresh",            MIME_COOKED_MASK_CC_MIN_FRESH           },
    {"only-if-cached",       MIME_COOKED_MASK_CC_ONLY_IF_CACHED      },
    {"public",               MIME_COOKED_MASK_CC_PUBLIC              },
    {"private",              MIME_COOKED_MASK_CC_PRIVATE             },
    {"must-revalidate",      MIME_COOKED_MASK_CC_MUST_REVALIDATE     },
    {"proxy-revalidate",     MIME_COOKED_MASK_CC_PROXY_REVALIDATE    },
    {"s-maxage",             MIME_COOKED_MASK_CC_S_MAXAGE            },
    {"need-revalidate-once", MIME_COOKED_MASK_CC_NEED_REVALIDATE_ONCE},
    {nullptr,                0                                       }
  };

  for (int i = 0; cc_mask_table[i].name != nullptr; i++) {
    const char         *wks                      = hdrtoken_string_to_wks(cc_mask_table[i].name);
    HdrTokenHeapPrefix *p                        = hdrtoken_wks_to_prefix(wks);
    p->wks_type_specific.u.cache_control.cc_mask = cc_mask_table[i].mask;
  }
}

void
mime_init_date_format_table()
{
  ////////////////////////////////////////////////////////////////
  // to speed up the days_since_epoch to m/d/y conversion, we   //
  // use a pre-computed lookup table to support the common case //
  // of dates that are +/- one year from today --- this code    //
  // builds the lookup table during the first call.             //
  ////////////////////////////////////////////////////////////////

  time_t now_secs;
  time_t i, now_days, first_days, last_days, num_days;
  int    m = 0, d = 0, y = 0;

  time(&now_secs);
  now_days   = static_cast<time_t>(now_secs / (60 * 60 * 24));
  first_days = now_days - 366;
  last_days  = now_days + 366;
  num_days   = last_days - first_days + 1;

  _days_to_mdy_fast_lookup_table           = static_cast<MDY *>(ats_malloc(num_days * sizeof(MDY)));
  _days_to_mdy_fast_lookup_table_first_day = first_days;
  _days_to_mdy_fast_lookup_table_last_day  = last_days;

  for (i = 0; i < num_days; i++) {
    mime_days_since_epoch_to_mdy_slowcase(first_days + i, &m, &d, &y);
    _days_to_mdy_fast_lookup_table[i].m = m;
    _days_to_mdy_fast_lookup_table[i].d = d;
    _days_to_mdy_fast_lookup_table[i].y = y;
  }
}

MIMEHdrImpl *
mime_hdr_create(HdrHeap *heap)
{
  MIMEHdrImpl *mh;

  mh = (MIMEHdrImpl *)heap->allocate_obj(sizeof(MIMEHdrImpl), HDR_HEAP_OBJ_MIME_HEADER);
  mime_hdr_init(mh);
  return mh;
}

void
_mime_hdr_field_block_init(MIMEFieldBlockImpl *fblock)
{
  fblock->m_freetop = 0;
  fblock->m_next    = nullptr;

#ifdef BLOCK_INIT_PARANOIA
  int i;

  // FIX: Could eliminate this initialization loop if we assumed
  //      every slot above the freetop of the block was garbage;
  //      but to be safe, and help debugging, for now we are eating
  //      the cost of initializing all slots in a block.

  for (i = 0; i < MIME_FIELD_BLOCK_SLOTS; i++) {
    MIMEField *field   = &(fblock->m_field_slots[i]);
    field->m_readiness = MIME_FIELD_SLOT_READINESS_EMPTY;
  }
#endif
}

void
mime_hdr_cooked_stuff_init(MIMEHdrImpl *mh, MIMEField *changing_field_or_null)
{
  // to be safe, reinitialize unless you know this call is for other cooked field
  if ((changing_field_or_null == nullptr) || (changing_field_or_null->m_wks_idx != MIME_WKSIDX_PRAGMA)) {
    mh->m_cooked_stuff.m_cache_control.m_mask           = 0;
    mh->m_cooked_stuff.m_cache_control.m_secs_max_age   = 0;
    mh->m_cooked_stuff.m_cache_control.m_secs_s_maxage  = 0;
    mh->m_cooked_stuff.m_cache_control.m_secs_max_stale = 0;
    mh->m_cooked_stuff.m_cache_control.m_secs_min_fresh = 0;
  }
  if ((changing_field_or_null == nullptr) || (changing_field_or_null->m_wks_idx != MIME_WKSIDX_CACHE_CONTROL)) {
    mh->m_cooked_stuff.m_pragma.m_no_cache = false;
  }
}

void
mime_hdr_init(MIMEHdrImpl *mh)
{
  mime_hdr_init_accelerators_and_presence_bits(mh);

  mime_hdr_cooked_stuff_init(mh, nullptr);

  // first header is inline: fake an object header for uniformity
  obj_init_header((HdrHeapObjImpl *)&(mh->m_first_fblock), HDR_HEAP_OBJ_FIELD_BLOCK, sizeof(MIMEFieldBlockImpl), 0);

  _mime_hdr_field_block_init(&(mh->m_first_fblock));
  mh->m_fblock_list_tail = &(mh->m_first_fblock);

  MIME_HDR_SANITY_CHECK(mh);
}

MIMEFieldBlockImpl *
_mime_field_block_copy(MIMEFieldBlockImpl *s_fblock, HdrHeap * /* s_heap ATS_UNUSED */, HdrHeap *d_heap)
{
  MIMEFieldBlockImpl *d_fblock;

  d_fblock = (MIMEFieldBlockImpl *)d_heap->allocate_obj(sizeof(MIMEFieldBlockImpl), HDR_HEAP_OBJ_FIELD_BLOCK);
  memcpy(d_fblock, s_fblock, sizeof(MIMEFieldBlockImpl));
  return d_fblock;
}

void
_mime_field_block_destroy(HdrHeap *heap, MIMEFieldBlockImpl *fblock)
{
  heap->deallocate_obj(fblock);
}

void
mime_hdr_destroy_field_block_list(HdrHeap *heap, MIMEFieldBlockImpl *head)
{
  MIMEFieldBlockImpl *next;

  while (head != nullptr) {
    next = head->m_next;
    _mime_field_block_destroy(heap, head);
    head = next;
  }
}

void
mime_hdr_destroy(HdrHeap *heap, MIMEHdrImpl *mh)
{
  mime_hdr_destroy_field_block_list(heap, mh->m_first_fblock.m_next);

  // INKqa11458: if we deallocate mh here and call TSMLocRelease
  // again, the plugin fails in assert. We leave deallocating to
  // the plugin using TSMLocRelease

  // heap->deallocate_obj(mh);
}

void
mime_hdr_copy_onto(MIMEHdrImpl *s_mh, HdrHeap *s_heap, MIMEHdrImpl *d_mh, HdrHeap *d_heap, bool inherit_strs)
{
  int                 block_count;
  MIMEFieldBlockImpl *s_fblock, *d_fblock, *prev_d_fblock;

  // If there are chained field blocks beyond the first one, we're just going to
  //   destroy them.  Ideally, we'd use them if the copied in header needed
  //   extra blocks.  It's too late in the Tomcat code cycle to implement
  //   reuse.
  if (d_mh->m_first_fblock.m_next) {
    mime_hdr_destroy_field_block_list(d_heap, d_mh->m_first_fblock.m_next);
  }

  ink_assert(((char *)&(s_mh->m_first_fblock.m_field_slots[MIME_FIELD_BLOCK_SLOTS]) - (char *)s_mh) == sizeof(struct MIMEHdrImpl));

  int   top             = s_mh->m_first_fblock.m_freetop;
  char *end             = reinterpret_cast<char *>(&(s_mh->m_first_fblock.m_field_slots[top]));
  int   bytes_below_top = end - reinterpret_cast<char *>(s_mh);

  // copies useful part of enclosed first block too
  memcpy(d_mh, s_mh, bytes_below_top);

  if (d_mh->m_first_fblock.m_next == nullptr) // common case: no other block
  {
    d_mh->m_fblock_list_tail = &(d_mh->m_first_fblock);
    block_count              = 1;
  } else // uncommon case: block list exists
  {
    prev_d_fblock = &(d_mh->m_first_fblock);
    block_count   = 1;
    for (s_fblock = s_mh->m_first_fblock.m_next; s_fblock != nullptr; s_fblock = s_fblock->m_next) {
      ++block_count;
      d_fblock              = _mime_field_block_copy(s_fblock, s_heap, d_heap);
      prev_d_fblock->m_next = d_fblock;
      prev_d_fblock         = d_fblock;
    }
    d_mh->m_fblock_list_tail = prev_d_fblock;
  }

  if (inherit_strs) {
    d_heap->inherit_string_heaps(s_heap);
  }

  mime_hdr_field_block_list_adjust(block_count, &(s_mh->m_first_fblock), &(d_mh->m_first_fblock));

  MIME_HDR_SANITY_CHECK(s_mh);
  MIME_HDR_SANITY_CHECK(d_mh);
}

MIMEHdrImpl *
mime_hdr_clone(MIMEHdrImpl *s_mh, HdrHeap *s_heap, HdrHeap *d_heap, bool inherit_strs)
{
  MIMEHdrImpl *d_mh;

  d_mh = mime_hdr_create(d_heap);
  mime_hdr_copy_onto(s_mh, s_heap, d_mh, d_heap, inherit_strs);
  return d_mh;
}

/** Move a pointer from one list to another, keeping the relative offset.
 * @return A pointer that has the same relative offset to @a dest_base as
 * @a dest_ptr does to @a src_base.
 */
static inline MIMEField *
rebase(MIMEField *dest_ptr,  ///< Original pointer into @src_base memory.
       void      *dest_base, ///< New base pointer.
       void      *src_base   ///< Original base pointer.
)
{
  return reinterpret_cast<MIMEField *>(reinterpret_cast<char *>(dest_ptr) +
                                       (static_cast<char *>(dest_base) - static_cast<char *>(src_base)));
}

static inline void
relocate(MIMEField *field, MIMEFieldBlockImpl *dest_block, MIMEFieldBlockImpl *src_block)
{
  for (; src_block; src_block = src_block->m_next, dest_block = dest_block->m_next) {
    ink_release_assert(dest_block);

    if (field->m_next_dup >= src_block->m_field_slots && field->m_next_dup < src_block->m_field_slots + src_block->m_freetop) {
      field->m_next_dup = rebase(field->m_next_dup, dest_block->m_field_slots, src_block->m_field_slots);
      return;
    }
  }
}

void
mime_hdr_field_block_list_adjust(int /* block_count ATS_UNUSED */, MIMEFieldBlockImpl *old_list, MIMEFieldBlockImpl *new_list)
{
  for (MIMEFieldBlockImpl *new_blk = new_list; new_blk; new_blk = new_blk->m_next) {
    for (MIMEField *field = new_blk->m_field_slots, *end = field + new_blk->m_freetop; field != end; ++field) {
      if (field->is_live() && field->m_next_dup) {
        relocate(field, new_list, old_list);
      }
    }
  }
}

int
mime_hdr_length_get(MIMEHdrImpl *mh)
{
  unsigned int        length, index;
  MIMEFieldBlockImpl *fblock;
  MIMEField          *field;

  length = 2;

  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    for (index = 0; index < fblock->m_freetop; index++) {
      field = &(fblock->m_field_slots[index]);
      if (field->is_live()) {
        length += mime_field_length_get(field);
      }
    }
  }

  return length;
}

void
mime_hdr_fields_clear(HdrHeap *heap, MIMEHdrImpl *mh)
{
  mime_hdr_destroy_field_block_list(heap, mh->m_first_fblock.m_next);
  mime_hdr_init(mh);
}

MIMEField *
_mime_hdr_field_list_search_by_wks(MIMEHdrImpl *mh, int wks_idx)
{
  MIMEFieldBlockImpl *fblock;
  MIMEField          *field, *too_far_field;

  ink_assert(hdrtoken_is_valid_wks_idx(wks_idx));

  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    field = &(fblock->m_field_slots[0]);

    too_far_field = &(fblock->m_field_slots[fblock->m_freetop]);
    while (field < too_far_field) {
      if (field->is_live() && (field->m_wks_idx == wks_idx)) {
        return field;
      }
      ++field;
    }
  }

  return nullptr;
}

MIMEField *
_mime_hdr_field_list_search_by_string(MIMEHdrImpl *mh, std::string_view field_name)
{
  MIMEFieldBlockImpl *fblock;
  MIMEField          *field, *too_far_field;

  ink_assert(mh);
  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    field = &(fblock->m_field_slots[0]);

    too_far_field = &(fblock->m_field_slots[fblock->m_freetop]);
    while (field < too_far_field) {
      if (field->is_live() &&
          strcasecmp(std::string_view{field->m_ptr_name, static_cast<std::string_view::size_type>(field->m_len_name)},
                     field_name) == 0) {
        return field;
      }
      ++field;
    }
  }

  return nullptr;
}

MIMEField *
_mime_hdr_field_list_search_by_slotnum(MIMEHdrImpl *mh, int slotnum)
{
  unsigned int        block_num, block_index;
  MIMEFieldBlockImpl *fblock;

  if (slotnum < MIME_FIELD_BLOCK_SLOTS) {
    fblock      = &(mh->m_first_fblock);
    block_index = slotnum;
    if (block_index >= fblock->m_freetop) {
      return nullptr;
    } else {
      return &(fblock->m_field_slots[block_index]);
    }
  } else {
    block_num   = slotnum / MIME_FIELD_BLOCK_SLOTS;
    block_index = slotnum % MIME_FIELD_BLOCK_SLOTS;

    fblock = &(mh->m_first_fblock);
    while (block_num-- && fblock) {
      fblock = fblock->m_next;
    }
    if ((fblock == nullptr) || (block_index >= fblock->m_freetop)) {
      return nullptr;
    } else {
      return &(fblock->m_field_slots[block_index]);
    }
  }
}

MIMEField *
mime_hdr_field_find(MIMEHdrImpl *mh, std::string_view field_name)
{
  HdrTokenHeapPrefix *token_info;
  const bool          is_wks = hdrtoken_is_wks(field_name.data());

  ink_assert(!field_name.empty());

  ////////////////////////////////////////////
  // do presence check and slot accelerator //
  ////////////////////////////////////////////

#if TRACK_FIELD_FIND_CALLS
  Dbg(dbg_ctl_http, "mime_hdr_field_find(hdr 0x%X, field %.*s): is_wks = %d", mh, static_cast<int>(field_name.length()),
      field_name.data(), is_wks);
#endif

  if (is_wks) {
    token_info = hdrtoken_wks_to_prefix(field_name.data());
    if ((token_info->wks_info.mask) && ((mh->m_presence_bits & token_info->wks_info.mask) == 0)) {
#if TRACK_FIELD_FIND_CALLS
      Dbg(dbg_ctl_http, "mime_hdr_field_find(hdr 0x%X, field %.*s): MISS (due to presence bits)", mh, field_name_len,
          field_name_str);
#endif
      return nullptr;
    }

    int32_t slot_id = token_info->wks_info.slotid;

    if (slot_id != MIME_SLOTID_NONE) {
      uint32_t slotnum = mime_hdr_get_accelerator_slotnum(mh, slot_id);

      if (slotnum != MIME_FIELD_SLOTNUM_UNKNOWN) {
        MIMEField *f = _mime_hdr_field_list_search_by_slotnum(mh, slotnum);
        ink_assert((f == nullptr) || f->is_live());
#if TRACK_FIELD_FIND_CALLS
        Dbg(dbg_ctl_http, "mime_hdr_field_find(hdr 0x%X, field %.*s): %s (due to slot accelerators)", mh,
            static_cast<int>(field_name.length()), field_name.data(), (f ? "HIT" : "MISS"));
#endif
        return f;
      } else {
#if TRACK_FIELD_FIND_CALLS
        Dbg(dbg_ctl_http, "mime_hdr_field_find(hdr 0x%X, field %.*s): UNKNOWN (slot too big)", mh,
            static_cast<int>(field_name.length()), field_name.data());
#endif
      }
    }

    ///////////////////////////////////////////////////////////////////////////
    // search by well-known string index or by case-insensitive string match //
    ///////////////////////////////////////////////////////////////////////////

    MIMEField *f = _mime_hdr_field_list_search_by_wks(mh, token_info->wks_idx);
    ink_assert((f == nullptr) || f->is_live());
#if TRACK_FIELD_FIND_CALLS
    Dbg(dbg_ctl_http, "mime_hdr_field_find(hdr 0x%X, field %.*s): %s (due to WKS list walk)", mh,
        static_cast<int>(field_name.length()), field_name.data(), (f ? "HIT" : "MISS"));
#endif
    return f;
  } else {
    MIMEField *f = _mime_hdr_field_list_search_by_string(mh, field_name);

    ink_assert((f == nullptr) || f->is_live());
#if TRACK_FIELD_FIND_CALLS
    Dbg(dbg_ctl_http, "mime_hdr_field_find(hdr 0x%X, field %.*s): %s (due to strcmp list walk)", mh,
        static_cast<int>(field_name.length()), field_name.data(), (f ? "HIT" : "MISS"));
#endif
    return f;
  }
}

MIMEField *
mime_hdr_field_get(MIMEHdrImpl *mh, int idx)
{
  unsigned int        index;
  MIMEFieldBlockImpl *fblock;
  MIMEField          *field;
  int                 got_idx;

  got_idx = -1;

  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    for (index = 0; index < fblock->m_freetop; index++) {
      field = &(fblock->m_field_slots[index]);
      if (field->is_live()) {
        ++got_idx;
      }
      if (got_idx == idx) {
        return field;
      }
    }
  }

  return nullptr;
}

MIMEField *
mime_hdr_field_get_slotnum(MIMEHdrImpl *mh, int slotnum)
{
  return _mime_hdr_field_list_search_by_slotnum(mh, slotnum);
}

int
mime_hdr_fields_count(MIMEHdrImpl *mh)
{
  unsigned int        index;
  MIMEFieldBlockImpl *fblock;
  MIMEField          *field;
  int                 count;

  count = 0;

  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    for (index = 0; index < fblock->m_freetop; index++) {
      field = &(fblock->m_field_slots[index]);
      if (field->is_live()) {
        ++count;
      }
    }
  }

  return count;
}

void
mime_field_init(MIMEField *field)
{
  memset(field, 0, sizeof(MIMEField));
  field->m_readiness = MIME_FIELD_SLOT_READINESS_DETACHED;
  field->m_wks_idx   = -1;
}

MIMEField *
mime_field_create(HdrHeap *heap, MIMEHdrImpl *mh)
{
  MIMEField          *field;
  MIMEFieldBlockImpl *tail_fblock, *new_fblock;

  tail_fblock = mh->m_fblock_list_tail;
  if (tail_fblock->m_freetop >= MIME_FIELD_BLOCK_SLOTS) {
    new_fblock = (MIMEFieldBlockImpl *)heap->allocate_obj(sizeof(MIMEFieldBlockImpl), HDR_HEAP_OBJ_FIELD_BLOCK);
    _mime_hdr_field_block_init(new_fblock);
    tail_fblock->m_next    = new_fblock;
    tail_fblock            = new_fblock;
    mh->m_fblock_list_tail = new_fblock;
  }

  field = &(tail_fblock->m_field_slots[tail_fblock->m_freetop]);
  ++tail_fblock->m_freetop;

  mime_field_init(field);

  return field;
}

MIMEField *
mime_field_create_named(HdrHeap *heap, MIMEHdrImpl *mh, std::string_view name)
{
  MIMEField *field              = mime_field_create(heap, mh);
  int        field_name_wks_idx = hdrtoken_tokenize(name.data(), static_cast<int>(name.length()));
  mime_field_name_set(heap, mh, field, field_name_wks_idx, name, true);
  return field;
}

void
mime_hdr_field_attach(MIMEHdrImpl *mh, MIMEField *field, int check_for_dups, MIMEField *prev_dup)
{
  MIME_HDR_SANITY_CHECK(mh);

  if (!field->is_detached()) {
    return;
  }

  ink_assert(field->m_ptr_name != nullptr);

  //////////////////////////////////////////////////
  // if we don't know the head dup, or are given  //
  // a non-head dup, then search for the head dup //
  //////////////////////////////////////////////////

  if (check_for_dups || (prev_dup && (!prev_dup->is_dup_head()))) {
    std::string_view name{field->name_get()};
    prev_dup = mime_hdr_field_find(mh, name);
    ink_assert((prev_dup == nullptr) || (prev_dup->is_dup_head()));
  }

  field->m_readiness = MIME_FIELD_SLOT_READINESS_LIVE;

  ////////////////////////////////////////////////////////////////////
  // now, attach the new field --- if there are dups, make sure the //
  // field is patched into the dup list in increasing slot order to //
  // maintain the invariant that dups are chained in slot order     //
  ////////////////////////////////////////////////////////////////////

  if (prev_dup) {
    MIMEField *next_dup;
    int        field_slotnum, prev_slotnum, next_slotnum;

    /////////////////////////////////////////////////////////////////
    // walk down dup list looking for the last dup in slot-order   //
    // before this field object --- meaning a dup before the field //
    // in slot order who either has no next dup, or whose next dup //
    // is numerically after the field in slot order.               //
    /////////////////////////////////////////////////////////////////

    field_slotnum = mime_hdr_field_slotnum(mh, field);
    prev_slotnum  = mime_hdr_field_slotnum(mh, prev_dup);
    next_dup      = prev_dup->m_next_dup;
    next_slotnum  = (next_dup ? mime_hdr_field_slotnum(mh, next_dup) : -1);

    ink_assert(field_slotnum != prev_slotnum);

    while (prev_slotnum < field_slotnum) // break if prev after field
    {
      if (next_dup == nullptr) {
        break; // no next dup, we're done
      }
      if (next_slotnum > field_slotnum) {
        break; // next dup is after us, we're done
      }
      prev_dup     = next_dup;
      prev_slotnum = next_slotnum;
      next_dup     = prev_dup->m_next_dup;
    }

    /////////////////////////////////////////////////////
    // we get here if the prev_slotnum > field_slotnum //
    // (meaning we're now the first dup in the list),  //
    // or when we've found the correct prev and next   //
    /////////////////////////////////////////////////////

    if (prev_slotnum > field_slotnum) // we are now the head
    {
      /////////////////////////////////////////////////////////////
      // here, it turns out that "prev_dup" is actually after    //
      // "field" in the list of fields --- so, prev_dup is a bit //
      // of a misnomer, it is actually, the NEXT field!          //
      /////////////////////////////////////////////////////////////

      field->m_flags    = (field->m_flags | MIME_FIELD_SLOT_FLAGS_DUP_HEAD);
      field->m_next_dup = prev_dup;
      prev_dup->m_flags = (prev_dup->m_flags & ~MIME_FIELD_SLOT_FLAGS_DUP_HEAD);
      mime_hdr_set_accelerators_and_presence_bits(mh, field);
    } else // patch us after prev, and before next
    {
      ink_assert(prev_slotnum < field_slotnum);
      ink_assert((next_dup == nullptr) || (next_slotnum > field_slotnum));
      field->m_flags = (field->m_flags & ~MIME_FIELD_SLOT_FLAGS_DUP_HEAD);
      ink_assert((next_dup == nullptr) || next_dup->is_live());
      prev_dup->m_next_dup = field;
      field->m_next_dup    = next_dup;
    }
  } else {
    field->m_flags = (field->m_flags | MIME_FIELD_SLOT_FLAGS_DUP_HEAD);
    mime_hdr_set_accelerators_and_presence_bits(mh, field);
  }

  // Now keep the cooked cache consistent
  ink_assert(field->is_live());
  if (field->m_ptr_value && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }

  MIME_HDR_SANITY_CHECK(mh);
}

void
mime_hdr_field_detach(MIMEHdrImpl *mh, MIMEField *field, bool detach_all_dups)
{
  ink_assert(mh);
  MIMEField *next_dup = field->m_next_dup;

  // If this field is already detached, there's nothing to do. There must
  // not be a dup list if we detached correctly.
  if (field->is_detached()) {
    ink_assert(next_dup == nullptr);
    return;
  }

  ink_assert(field->is_live());
  MIME_HDR_SANITY_CHECK(mh);

  // Normally, this function is called with the current dup list head,
  // so, we need to update the accelerators after the patch out.  But, if
  // this function is ever called in the middle of a dup list, we need
  // to walk the list to find the previous dup in the list to patch out
  // the dup being detached.

  if (field->m_flags & MIME_FIELD_SLOT_FLAGS_DUP_HEAD) // head of list?
  {
    if (!next_dup) // only child
    {
      mime_hdr_unset_accelerators_and_presence_bits(mh, field);
    } else // next guy is dup head
    {
      next_dup->m_flags |= MIME_FIELD_SLOT_FLAGS_DUP_HEAD;
      mime_hdr_set_accelerators_and_presence_bits(mh, next_dup);
    }
  } else // need to walk list to find and patch out from predecessor
  {
    std::string_view name{field->name_get()};
    MIMEField       *prev = mime_hdr_field_find(mh, name);

    while (prev && (prev->m_next_dup != field)) {
      prev = prev->m_next_dup;
    }
    ink_assert(prev != nullptr);

    if (prev->m_next_dup == field) {
      prev->m_next_dup = next_dup;
    }
  }

  // Field is now detached and alone
  field->m_readiness = MIME_FIELD_SLOT_READINESS_DETACHED;
  field->m_next_dup  = nullptr;

  // Because we changed the values through detaching,update the cooked cache
  if (field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }

  MIME_HDR_SANITY_CHECK(mh);

  // At this point, the list should be back to a valid state, either the
  // next dup detached and the accelerators set to the next dup (if any),
  // or an interior dup detached and patched around.  If we are requested
  // to delete the whole dup list, we tail-recurse to delete it.

  if (detach_all_dups && next_dup) {
    mime_hdr_field_detach(mh, next_dup, detach_all_dups);
  }
}

void
mime_hdr_field_delete(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, bool delete_all_dups)
{
  if (delete_all_dups) {
    while (field) {
      MIMEField *next = field->m_next_dup;
      mime_hdr_field_delete(heap, mh, field, false);
      field = next;
    }
  } else {
    heap->free_string(field->m_ptr_name, field->m_len_name);
    heap->free_string(field->m_ptr_value, field->m_len_value);

    MIME_HDR_SANITY_CHECK(mh);
    mime_hdr_field_detach(mh, field, false);

    MIME_HDR_SANITY_CHECK(mh);
    mime_field_destroy(mh, field);

    MIMEFieldBlockImpl *prev_block        = nullptr;
    bool                can_destroy_block = true;
    for (auto fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
      if (prev_block != nullptr) {
        if (fblock->m_freetop == MIME_FIELD_BLOCK_SLOTS && fblock->contains(field)) {
          // Check if fields in all slots are deleted
          for (auto &m_field_slot : fblock->m_field_slots) {
            if (m_field_slot.m_readiness != MIME_FIELD_SLOT_READINESS_DELETED) {
              can_destroy_block = false;
              break;
            }
          }
          // Destroy a block and maintain the chain
          if (can_destroy_block) {
            prev_block->m_next = fblock->m_next;
            _mime_field_block_destroy(heap, fblock);
            if (prev_block->m_next == nullptr) {
              mh->m_fblock_list_tail = prev_block;
            }
          }
          break;
        }
      }
      prev_block = fblock;
    }
  }

  MIME_HDR_SANITY_CHECK(mh);
}

auto
MIMEHdrImpl::find(MIMEField const *field) -> iterator
{
  for (MIMEFieldBlockImpl *fblock = &m_first_fblock; fblock != nullptr; fblock = fblock->m_next) {
    if (fblock->contains(field)) {
      return {fblock, unsigned(field - fblock->m_field_slots)};
    }
  }
  return {};
}

// This function needs to be removed - use of it indicates poorly implemented code.
// That code should be updated to use the field iterators, which are much more performant.
int
mime_hdr_field_slotnum(MIMEHdrImpl *mh, MIMEField *field)
{
  int                 slots_so_far;
  MIMEFieldBlockImpl *fblock;

  slots_so_far = 0;
  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    if (fblock->contains(field)) {
      MIMEField *first      = &(fblock->m_field_slots[0]);
      ptrdiff_t  block_slot = field - first; // in units of MIMEField
      return slots_so_far + block_slot;
    }
    slots_so_far += MIME_FIELD_BLOCK_SLOTS;
  }
  return -1;
}

MIMEField *
mime_hdr_prepare_for_value_set(HdrHeap *heap, MIMEHdrImpl *mh, std::string_view name)
{
  int        wks_idx;
  MIMEField *field;

  field = mime_hdr_field_find(mh, name);

  //////////////////////////////////////////////////////////////////////
  // this function returns with exactly one attached field created,   //
  // ready to have its value set.                                     //
  //                                                                  //
  // on return from field_find, there are 3 possibilities:            //
  //   no field found:      create attached, named field              //
  //   field found w/dups:  delete list, create attached, named field //
  //   dupless field found: return the field for mutation             //
  //////////////////////////////////////////////////////////////////////

  if (field == nullptr) // no fields of this name
  {
    wks_idx = hdrtoken_tokenize(name.data(), static_cast<int>(name.length()));
    field   = mime_field_create(heap, mh);
    mime_field_name_set(heap, mh, field, wks_idx, name, true);
    mime_hdr_field_attach(mh, field, 0, nullptr);

  } else if (field->m_next_dup) // list of more than 1 field
  {
    wks_idx = field->m_wks_idx;
    mime_hdr_field_delete(heap, mh, field, true);
    field = mime_field_create(heap, mh);
    mime_field_name_set(heap, mh, field, wks_idx, name, true);
    mime_hdr_field_attach(mh, field, 0, nullptr);
  }
  return field;
}

void
mime_field_destroy(MIMEHdrImpl * /* mh ATS_UNUSED */, MIMEField *field)
{
  ink_assert(field->m_readiness == MIME_FIELD_SLOT_READINESS_DETACHED);
  field->m_readiness = MIME_FIELD_SLOT_READINESS_DELETED;
}

std::string_view
MIMEField::name_get() const
{
  if (m_wks_idx >= 0) {
    return {hdrtoken_index_to_wks(m_wks_idx), m_len_name};
  }
  return {m_ptr_name, m_len_name};
}

void
mime_field_name_set(HdrHeap *heap, MIMEHdrImpl * /* mh ATS_UNUSED */, MIMEField *field, int16_t name_wks_idx_or_neg1,
                    std::string_view name, bool must_copy_string)
{
  ink_assert(field->m_readiness == MIME_FIELD_SLOT_READINESS_DETACHED);

  field->m_wks_idx = name_wks_idx_or_neg1;
  mime_str_u16_set(heap, name, &(field->m_ptr_name), &(field->m_len_name), must_copy_string);

  if ((name_wks_idx_or_neg1 == MIME_WKSIDX_CACHE_CONTROL) || (name_wks_idx_or_neg1 == MIME_WKSIDX_PRAGMA)) {
    field->m_flags |= MIME_FIELD_SLOT_FLAGS_COOKED;
  }
}

int
MIMEField::value_get_index(std::string_view value) const
{
  int  retval = -1;
  auto length{static_cast<int>(value.length())};

  // if field doesn't support commas and there is just one instance, just compare the value
  if (!this->supports_commas() && !this->has_dups()) {
    if (this->m_len_value == static_cast<uint32_t>(length) && strncasecmp(value.data(), this->m_ptr_value, length) == 0) {
      retval = 0;
    }
  } else {
    HdrCsvIter  iter;
    int         tok_len;
    int         index = 0;
    const char *tok   = iter.get_first(this, &tok_len);

    while (tok) {
      if (tok_len == length && strncasecmp(tok, value.data(), length) == 0) {
        retval = index;
        break;
      } else {
        index++;
      }
      tok = iter.get_next(&tok_len);
    }
  }

  return retval;
}

std::string_view
MIMEField::value_get() const
{
  return {m_ptr_value, m_len_value};
}

int32_t
mime_field_value_get_int(const MIMEField *field)
{
  std::string_view value{field->value_get()};

  return mime_parse_int(value.data(), value.data() + value.size());
}

uint32_t
mime_field_value_get_uint(const MIMEField *field)
{
  std::string_view value{field->value_get()};

  return mime_parse_uint(value.data(), value.data() + value.size());
}

int64_t
mime_field_value_get_int64(const MIMEField *field)
{
  std::string_view value{field->value_get()};

  return mime_parse_int64(value.data(), value.data() + value.size());
}

time_t
mime_field_value_get_date(const MIMEField *field)
{
  std::string_view value{field->value_get()};

  return mime_parse_date(value.data(), value.data() + value.size());
}

const char *
mime_field_value_get_comma_val(const MIMEField *field, int *length, int idx)
{
  // some fields (like Date) contain commas but should not be ripped apart
  if (!field->supports_commas()) {
    if (idx == 0) {
      auto value{field->value_get()};
      *length = static_cast<int>(value.size());
      return value.data();
    }
    return nullptr;
  } else {
    Str    *str;
    StrList list(false);

    mime_field_value_get_comma_list(field, &list);
    str = list.get_idx(idx);
    if (str != nullptr) {
      *length = static_cast<int>(str->len);
      return str->str;
    } else {
      *length = 0;
      return nullptr;
    }
  }
}

int
mime_field_value_get_comma_val_count(const MIMEField *field)
{
  // some fields (like Date) contain commas but should not be ripped apart
  if (!field->supports_commas()) {
    return ((field->m_len_value == 0) ? 0 : 1);
  } else {
    StrList list(false);
    int     count = mime_field_value_get_comma_list(field, &list);
    return count;
  }
}

int
mime_field_value_get_comma_list(const MIMEField *field, StrList *list)
{
  std::string_view value{field->value_get()};

  // if field doesn't support commas, don't rip apart.
  if (!field->supports_commas()) {
    list->append_string(value.data(), static_cast<int>(value.size()));
  } else {
    HttpCompat::parse_tok_list(list, 1, value.data(), static_cast<int>(value.size()), ',');
  }

  return list->count;
}

const char *
mime_field_value_str_from_strlist(HdrHeap *heap, int *new_str_len_return, StrList *list)
{
  Str  *cell;
  char *new_value, *dest;
  int   i, new_value_len;
  // This works, because all strings are from the same heap when it is "split" into the list.
  HdrHeap::HeapGuard guard(heap, list->head->str);

  new_value_len = 0;

  // (1) walk the StrList cells, summing each cell's string lengths,
  //     and add 2 bytes for each ", " between cells
  cell = list->head;
  for (i = 0; i < list->count; i++) {
    new_value_len += cell->len;
    cell           = cell->next;
  }
  if (list->count > 1) {
    new_value_len += (2 * (list->count - 1));
  }

  // (2) allocate new heap string
  new_value = heap->allocate_str(new_value_len);

  // (3) copy string pieces into new heap string
  dest = new_value;
  cell = list->head;
  for (i = 0; i < list->count; i++) {
    if (i != 0) {
      *dest++ = ',';
      *dest++ = ' ';
    }
    memcpy(dest, cell->str, cell->len);
    dest += cell->len;
    cell  = cell->next;
  }
  ink_assert(dest - new_value == new_value_len);

  *new_str_len_return = new_value_len;
  return new_value;
}

void
mime_field_value_set_comma_val(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int idx, std::string_view new_piece)
{
  int     len;
  Str    *cell;
  StrList list(false);

  // (1) rip the value into tokens, keeping surrounding quotes, but not whitespace
  HttpCompat::parse_tok_list(&list, 0, field->m_ptr_value, field->m_len_value, ',');

  // (2) if desired index isn't valid, then don't change the field
  if ((idx < 0) || (idx >= list.count)) {
    return;
  }

  // (3) mutate cell idx
  cell = list.get_idx(idx);
  ink_assert(cell != nullptr);
  cell->str = new_piece.data();
  cell->len = new_piece.length();

  // (4) reassemble the new string
  field->m_ptr_value = mime_field_value_str_from_strlist(heap, &len, &list);
  field->m_len_value = len;

  // (5) keep stuff fields consistent
  field->m_n_v_raw_printable = 0;
  if (field->is_live() && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }
}

void
mime_field_value_delete_comma_val(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int idx)
{
  int     len;
  Str    *cell;
  StrList list(false);

  // (1) rip the value into tokens, keeping surrounding quotes, but not whitespace
  HttpCompat::parse_tok_list(&list, 0, field->m_ptr_value, field->m_len_value, ',');

  // (2) if desired index isn't valid, then don't change the field
  if ((idx < 0) || (idx >= list.count)) {
    return;
  }

  // (3) delete cell idx
  cell = list.get_idx(idx);
  list.detach(cell);

  /**********************************************/
  /*   Fix for bug INKqa09752                   */
  /*                                            */
  /*   If this is the last value                */
  /*   in the field, set the m_ptr_val to NULL  */
  /**********************************************/

  if (list.count == 0) {
    field->m_ptr_value = nullptr;
    field->m_len_value = 0;
  } else {
    /************************************/
    /*   End Fix for bug INKqa09752     */
    /************************************/

    // (4) reassemble the new string
    field->m_ptr_value = mime_field_value_str_from_strlist(heap, &len, &list);
    field->m_len_value = len;
  }

  // (5) keep stuff fields consistent
  field->m_n_v_raw_printable = 0;
  if (field->is_live() && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }
}

void
mime_field_value_insert_comma_val(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int idx, std::string_view new_piece)
{
  int     len;
  Str    *cell, *prev;
  StrList list(false);

  // (1) rip the value into tokens, keeping surrounding quotes, but not whitespace
  HttpCompat::parse_tok_list(&list, 0, field->m_ptr_value, field->m_len_value, ',');

  // (2) if desired index isn't valid, then don't change the field
  if (idx < 0) {
    idx = list.count;
  }
  if (idx > list.count) {
    return;
  }

  // (3) create a new cell
  cell = list.new_cell(new_piece.data(), static_cast<int>(new_piece.length()));

  // (4) patch new cell into list at the right place
  if (idx == 0) {
    list.prepend(cell);
  } else {
    prev = list.get_idx(idx - 1);
    list.add_after(prev, cell);
  }

  // (5) reassemble the new string
  field->m_ptr_value = mime_field_value_str_from_strlist(heap, &len, &list);
  field->m_len_value = len;

  // (6) keep stuff fields consistent
  field->m_n_v_raw_printable = 0;
  if (field->is_live() && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }
}

void
mime_field_value_extend_comma_val(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int idx, std::string_view new_piece)
{
  Str    *cell;
  StrList list(false);
  int     trimmed, len;
  size_t  extended_len;
  char   *dest, *temp_ptr, temp_buf[128];

  // (1) rip the value into tokens, keeping surrounding quotes, but not whitespace
  HttpCompat::parse_tok_list(&list, 0, field->m_ptr_value, field->m_len_value, ',');

  // (2) if desired index isn't valid, then don't change the field
  if ((idx < 0) || (idx >= list.count)) {
    return;
  }

  // (3) get the cell we want to modify
  cell = list.get_idx(idx);
  ink_assert(cell != nullptr);

  // (4) trim quotes if any
  if ((cell->len >= 2) && (cell->str[0] == '\"') && (cell->str[cell->len - 1] == '\"')) {
    trimmed    = 1;
    cell->str += 1;
    cell->len -= 2;
  } else {
    trimmed = 0;
  }

  // (5) compute length of extended token
  auto new_piece_len{static_cast<int>(new_piece.length())};
  extended_len = cell->len + new_piece_len + (trimmed ? 2 : 0);

  // (6) allocate temporary space to construct new value
  if (extended_len <= sizeof(temp_buf)) {
    temp_ptr = temp_buf;
  } else {
    temp_ptr = static_cast<char *>(ats_malloc(extended_len));
  }

  // (7) construct new extended token
  dest = temp_ptr;
  if (trimmed) {
    *dest++ = '\"';
  }
  memcpy(dest, cell->str, cell->len);
  dest += cell->len;
  memcpy(dest, new_piece.data(), new_piece_len);
  dest += new_piece_len;
  if (trimmed) {
    *dest++ = '\"';
  }
  ink_assert((size_t)(dest - temp_ptr) == extended_len);

  // (8) assign the new token to the cell
  cell->str = temp_ptr;
  cell->len = extended_len;

  // (9) reassemble the new string
  field->m_ptr_value = mime_field_value_str_from_strlist(heap, &len, &list);
  field->m_len_value = len;

  // (10) keep stuff fields consistent
  field->m_n_v_raw_printable = 0;
  if (field->is_live() && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }

  // (11) free up any temporary storage
  if (extended_len > sizeof(temp_buf)) {
    ats_free(temp_ptr);
  }
}

void
mime_field_value_set(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, std::string_view value, bool must_copy_string)
{
  heap->free_string(field->m_ptr_value, field->m_len_value);

  if (must_copy_string && value.data()) {
    field->m_ptr_value = heap->duplicate_str(value.data(), static_cast<int>(value.length()));
  } else {
    field->m_ptr_value = nullptr;
  }

  field->m_len_value         = static_cast<int>(value.length());
  field->m_n_v_raw_printable = 0;

  // Now keep the cooked cache consistent
  if (field->is_live() && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }
}

void
mime_field_value_set_int(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int32_t value)
{
  char buf[16];
  int  len = mime_format_int(buf, value, sizeof(buf));
  mime_field_value_set(heap, mh, field, std::string_view{buf, static_cast<std::string_view::size_type>(len)}, true);
}

void
mime_field_value_set_uint(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, uint32_t value)
{
  char buf[16];
  int  len = mime_format_uint(buf, value, sizeof(buf));
  mime_field_value_set(heap, mh, field, std::string_view{buf, static_cast<std::string_view::size_type>(len)}, true);
}

void
mime_field_value_set_int64(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int64_t value)
{
  char buf[21];
  int  len = mime_format_int64(buf, value, sizeof(buf));
  mime_field_value_set(heap, mh, field, std::string_view{buf, static_cast<std::string_view::size_type>(len)}, true);
}

void
mime_field_value_set_date(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, time_t value)
{
  char buf[33];
  int  len = mime_format_date(buf, value);
  mime_field_value_set(heap, mh, field, std::string_view{buf, static_cast<std::string_view::size_type>(len)}, true);
}

void
mime_field_name_value_set(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, int16_t name_wks_idx_or_neg1, std::string_view name,
                          std::string_view value, int n_v_raw_printable, int n_v_raw_length, bool must_copy_strings)
{
  auto         name_length{static_cast<int>(name.length())};
  auto         value_length{static_cast<int>(value.length())};
  unsigned int n_v_raw_pad = n_v_raw_length - (name_length + value_length);

  ink_assert(field->m_readiness == MIME_FIELD_SLOT_READINESS_DETACHED);

  if (must_copy_strings) {
    mime_field_name_set(heap, mh, field, name_wks_idx_or_neg1, name, true);
    mime_field_value_set(heap, mh, field, value, true);
  } else {
    field->m_wks_idx   = name_wks_idx_or_neg1;
    field->m_ptr_name  = name.data();
    field->m_ptr_value = value.data();
    field->m_len_name  = name_length;
    field->m_len_value = value_length;
    if (n_v_raw_printable && (n_v_raw_pad <= 7)) {
      field->m_n_v_raw_printable     = n_v_raw_printable;
      field->m_n_v_raw_printable_pad = n_v_raw_pad;
    } else {
      field->m_n_v_raw_printable = 0;
    }

    // Now keep the cooked cache consistent
    if ((name_wks_idx_or_neg1 == MIME_WKSIDX_CACHE_CONTROL) || (name_wks_idx_or_neg1 == MIME_WKSIDX_PRAGMA)) {
      field->m_flags |= MIME_FIELD_SLOT_FLAGS_COOKED;
    }
    if (field->is_live() && field->is_cooked()) {
      mh->recompute_cooked_stuff(field);
    }
  }
}

void
mime_field_value_append(HdrHeap *heap, MIMEHdrImpl *mh, MIMEField *field, std::string_view value, bool prepend_comma,
                        const char separator)
{
  auto length{static_cast<int>(value.length())};
  int  new_length = field->m_len_value + length;
  if (prepend_comma && field->m_len_value) {
    new_length += 2;
  }

  // Start by trying expand the string we already  have
  char *new_str = heap->expand_str(field->m_ptr_value, field->m_len_value, new_length);

  if (new_str == nullptr) {
    // Expansion failed.  Create a new string and copy over the value contents
    new_str = heap->allocate_str(new_length);
    memcpy(new_str, field->m_ptr_value, field->m_len_value);
  }

  char *ptr = new_str + field->m_len_value;
  if (prepend_comma && field->m_len_value) {
    *ptr++ = separator;
    *ptr++ = ' ';
  }

  memcpy(ptr, value.data(), length);

  field->m_ptr_value         = new_str;
  field->m_len_value         = new_length;
  field->m_n_v_raw_printable = 0;

  // Now keep the cooked cache consistent
  if (field->is_live() && field->is_cooked()) {
    mh->recompute_cooked_stuff(field);
  }
}

std::tuple<MIMEField *, std::string_view, std::string_view>
MIMEHdr::get_host_port_values()
{
  MIMEField       *field = this->field_find(static_cast<std::string_view>(MIME_FIELD_HOST));
  std::string_view host, port;

  if (field) {
    swoc::TextView b{field->m_ptr_value, static_cast<size_t>(field->m_len_value)};

    if (b) {
      if ('[' == *b) {
        auto idx = b.find(']');
        if (idx < b.size() - 1 && b[idx + 1] == ':') {
          host = b.take_prefix(idx + 1);
          port = b;
        } else {
          host = b;
        }
      } else {
        host = b.take_prefix_at(':');
        port = b;
      }
    } else {
      field = nullptr; // no value in field, signal fail.
    }
  }
  return std::make_tuple(field, host, port);
}

/***********************************************************************
 *                                                                     *
 *                          P A R S E R                                *
 *                                                                     *
 ***********************************************************************/

void
MIMEScanner::init()
{
  m_state = INITIAL_PARSE_STATE;
  // Ugly, but required because of how proxy allocation works - that leaves the instance in a
  // random state, so even assigning to it can crash. Because this method substitutes for a real
  // constructor in the proxy allocation system, call the CTOR here. Any memory that gets allocated
  // is supposed to be cleaned up by calling @c clear on this object.
  new (&m_line) std::string;
}

MIMEScanner &
MIMEScanner::append(TextView text)
{
  m_line += text;
  return *this;
}

ParseResult
MIMEScanner::get(TextView &input, TextView &output, bool &output_shares_input, bool eof_p, ScanType scan_type)
{
  ParseResult zret = PARSE_RESULT_CONT;
  // Need this for handling dangling CR.
  static const char RAW_CR{ParseRules::CHAR_CR};

  auto text = input;
  while (PARSE_RESULT_CONT == zret && !text.empty()) {
    switch (m_state) {
    case MIME_PARSE_BEFORE: // waiting to find a field.
      m_line.resize(0);     // any caller should already be done with the buffer
      if (ParseRules::is_cr(*text)) {
        ++text;
        if (!text.empty() && ParseRules::is_lf(*text)) {
          // optimize a bit - this happens >99% of the time after a CR.
          ++text;
          zret = PARSE_RESULT_DONE;
        } else {
          m_state = MIME_PARSE_FOUND_CR;
        }
      } else if (ParseRules::is_lf(*text)) {
        ++text;
        zret = PARSE_RESULT_DONE; // Required by regression test.
      } else {
        // consume this character in the next state.
        m_state = MIME_PARSE_INSIDE;
      }
      break;
    case MIME_PARSE_FOUND_CR:
      // Looking for a field and found a CR, which should mean terminating the header.
      if (ParseRules::is_lf(*text)) {
        ++text;
        zret = PARSE_RESULT_DONE;
      } else {
        // This really should be an error (spec doesn't permit lone CR) but the regression tests
        // require it.
        this->append(TextView(&RAW_CR, 1)); // This is to fix a core dump of the icc 19.1 compiler when {&RAW_CR, 1} is used
        m_state = MIME_PARSE_INSIDE;
      }
      break;
    case MIME_PARSE_INSIDE: {
      auto lf_off = text.find(ParseRules::CHAR_LF);
      if (lf_off != TextView::npos) {
        text.remove_prefix(lf_off + 1); // drop up to and including LF
        if (LINE == scan_type) {
          zret    = PARSE_RESULT_OK;
          m_state = MIME_PARSE_BEFORE;
        } else {
          m_state = MIME_PARSE_AFTER; // looking for line folding.
        }
      } else { // no EOL, consume all text without changing state.
        text.remove_prefix(text.size());
      }
    } break;
    case MIME_PARSE_AFTER:
      // After a LF, the next line might be a continuation / folded line. That's indicated by a
      // starting whitespace. If that's the case, back up over the preceding CR/LF with space and
      // pretend it's the same line.
      if (ParseRules::is_ws(*text)) { // folded line.
        char *unfold = const_cast<char *>(text.data() - 1);
        *unfold--    = ' ';
        if (ParseRules::is_cr(*unfold)) {
          *unfold = ' ';
        }
        m_state = MIME_PARSE_INSIDE; // back inside the field.
      } else {
        m_state = MIME_PARSE_BEFORE; // field terminated.
        zret    = PARSE_RESULT_OK;
      }
      break;
    }
  }

  TextView parsed_text{input.data(), text.data()};
  bool     save_parsed_text_p = !parsed_text.empty();

  if (PARSE_RESULT_CONT == zret) {
    // data ran out before we got a clear final result. There a number of things we need to check
    // and possibly adjust that result. It's less complex to do this cleanup than handle all of
    // these checks in the parser state machine.
    if (eof_p) {
      // Should never return PARSE_CONT if we've hit EOF.
      if (parsed_text.empty()) {
        // all input previously consumed. If we're between fields, that's cool.
        if (MIME_PARSE_INSIDE != m_state) {
          m_state = MIME_PARSE_BEFORE; // probably not needed...
          zret    = PARSE_RESULT_DONE;
        } else {
          zret = PARSE_RESULT_ERROR; // unterminated field.
        }
      } else if (MIME_PARSE_AFTER == m_state) {
        // Special case it seems - need to accept the final field even if there's no header
        // terminating CR LF. This is only reasonable after absolute end of input (EOF) because
        // otherwise this might be a multiline field where we haven't seen the next leading space.
        m_state = MIME_PARSE_BEFORE;
        zret    = PARSE_RESULT_OK;
      } else {
        // Partial input, no field / line CR LF
        zret = PARSE_RESULT_ERROR; // Unterminated field.
      }
    } else if (!parsed_text.empty()) {
      if (MIME_PARSE_INSIDE == m_state) {
        // Inside a field but more data is expected. Save what we've got.
        this->append(parsed_text);  // Do this here to force appending.
        save_parsed_text_p = false; // don't double append.
      } else if (MIME_PARSE_AFTER == m_state) {
        // After a field but we still have data. Need to parse it too.
        m_state = MIME_PARSE_BEFORE;
        zret    = PARSE_RESULT_OK;
      }
    }
  }

  if (save_parsed_text_p && !m_line.empty()) {
    // If we're already accumulating, continue to do so if we have data.
    this->append(parsed_text);
  }

  // adjust out arguments.
  output_shares_input = true;
  if (PARSE_RESULT_CONT != zret) {
    if (!m_line.empty()) {
      output              = m_line; // cleared when called with state MIME_PARSE_BEFORE
      output_shares_input = false;
    } else {
      output = parsed_text;
    }
  }

  // Make sure there are no null characters in the input scanned so far
  if (zret != PARSE_RESULT_ERROR && TextView::npos != parsed_text.find('\0')) {
    zret = PARSE_RESULT_ERROR;
  }

  input.remove_prefix(parsed_text.size());
  return zret;
}

void
_mime_parser_init(MIMEParser *parser)
{
  parser->m_field       = 0;
  parser->m_field_flags = 0;
  parser->m_value       = -1;
}
//////////////////////////////////////////////////////
// init     first time structure setup              //
// clear    resets an already-initialized structure //
//////////////////////////////////////////////////////
void
mime_parser_init(MIMEParser *parser)
{
  parser->m_scanner.init();
  _mime_parser_init(parser);
}

void
mime_parser_clear(MIMEParser *parser)
{
  parser->m_scanner.clear();
  _mime_parser_init(parser);
}

ParseResult
mime_parser_parse(MIMEParser *parser, HdrHeap *heap, MIMEHdrImpl *mh, const char **real_s, const char *real_e,
                  bool must_copy_strings, bool eof, bool remove_ws_from_field_name, size_t max_hdr_field_size)
{
  ParseResult err;
  bool        line_is_real;

  MIMEScanner *scanner = &parser->m_scanner;

  while (true) {
    ////////////////////////////////////////////////////////////////////////////
    // get a name:value line, with all continuation lines glued into one line //
    ////////////////////////////////////////////////////////////////////////////

    TextView text{*real_s, real_e};
    TextView parsed;
    err     = scanner->get(text, parsed, line_is_real, eof, MIMEScanner::FIELD);
    *real_s = text.data();
    if (err != PARSE_RESULT_OK) {
      return err;
    }

    //////////////////////////////////////////////////
    // if got a LF or CR on its own, end the header //
    //////////////////////////////////////////////////

    if ((parsed.size() >= 2) && (parsed[0] == ParseRules::CHAR_CR) && (parsed[1] == ParseRules::CHAR_LF)) {
      return PARSE_RESULT_DONE;
    }

    if ((parsed.size() >= 1) && (parsed[0] == ParseRules::CHAR_LF)) {
      return PARSE_RESULT_DONE;
    }

    /////////////////////////////////////////////
    // find pointers into the name:value field //
    /////////////////////////////////////////////

    /**
     * Fix for INKqa09141. The is_token function fails for '@' character.
     * Header names starting with '@' signs are valid headers. Hence we
     * have to add one more check to see if the first parameter is '@'
     * character then, the header name is valid.
     **/
    if ((!ParseRules::is_token(*parsed)) && (*parsed != '@')) {
      continue; // toss away garbage line
    }

    // find name last
    auto field_value = parsed; // need parsed as is later on.
    auto field_name  = field_value.split_prefix_at(':');
    if (field_name.empty()) {
      continue; // toss away garbage line
    }

    // RFC7230 section 3.2.4:
    // No whitespace is allowed between the header field-name and colon.  In
    // the past, differences in the handling of such whitespace have led to
    // security vulnerabilities in request routing and response handling.  A
    // server MUST reject any received request message that contains
    // whitespace between a header field-name and colon with a response code
    // of 400 (Bad Request).
    // A proxy MUST remove any such whitespace from a response message before
    // forwarding the message downstream.
    bool raw_print_field = true;
    if (is_ws(field_name.back())) {
      if (!remove_ws_from_field_name) {
        return PARSE_RESULT_ERROR;
      }
      field_name.rtrim_if(&ParseRules::is_ws);
      raw_print_field = false;
    } else if (parsed.suffix(2) != "\r\n") {
      raw_print_field = false;
    }

    // find value first
    field_value.ltrim_if(&ParseRules::is_ws);
    field_value.rtrim_if(&ParseRules::is_wslfcr);

    // Make sure the name + value is not longer than configured max_hdr_field_size
    if (field_name.size() + field_value.size() > max_hdr_field_size) {
      return PARSE_RESULT_ERROR;
    }

    //    int total_line_length = (int)(field_line_last - field_line_first + 1);

    //////////////////////////////////////////////////////////////////////
    // if we can't leave the name & value in the real buffer, copy them //
    //////////////////////////////////////////////////////////////////////

    if (must_copy_strings || (!line_is_real)) {
      char     *dup   = heap->duplicate_str(parsed.data(), parsed.size());
      ptrdiff_t delta = dup - parsed.data();
      field_name.assign(field_name.data() + delta, field_name.size());
      field_value.assign(field_value.data() + delta, field_value.size());
    }
    ///////////////////////
    // tokenize the name //
    ///////////////////////

    int field_name_wks_idx = hdrtoken_tokenize(field_name.data(), field_name.size());

    if (field_name_wks_idx < 0) {
      for (auto i : field_name) {
        if (!ParseRules::is_http_field_name(i)) {
          return PARSE_RESULT_ERROR;
        }
      }
    }

    // RFC 9110 Section 5.5. Field Values
    for (char i : field_value) {
      // FIXME: ParseRules::is_http_field_value() should be used but the implementation looks wrong
      if (ParseRules::is_control(i)) {
        return PARSE_RESULT_ERROR;
      }
    }

    ///////////////////////////////////////////
    // build and insert the new field object //
    ///////////////////////////////////////////

    MIMEField *field = mime_field_create(heap, mh);
    mime_field_name_value_set(heap, mh, field, field_name_wks_idx, field_name, field_value, raw_print_field, parsed.size(), false);
    mime_hdr_field_attach(mh, field, 1, nullptr);
  }
}

void
mime_hdr_describe(HdrHeapObjImpl *raw, bool recurse)
{
  MIMEFieldBlockImpl *fblock;
  MIMEHdrImpl        *obj = (MIMEHdrImpl *)raw;

  Dbg(dbg_ctl_http, "\t[PBITS: 0x%08X%08X, SLACC: 0x%04X%04X%04X%04X, HEADBLK: %p, TAILBLK: %p]",
      (uint32_t)((obj->m_presence_bits >> 32) & (TOK_64_CONST(0xFFFFFFFF))),
      (uint32_t)((obj->m_presence_bits >> 0) & (TOK_64_CONST(0xFFFFFFFF))), obj->m_slot_accelerators[0],
      obj->m_slot_accelerators[1], obj->m_slot_accelerators[2], obj->m_slot_accelerators[3], &(obj->m_first_fblock),
      obj->m_fblock_list_tail);

  Dbg(dbg_ctl_http, "\t[CBITS: 0x%08X, T_MAXAGE: %d, T_SMAXAGE: %d, T_MAXSTALE: %d, T_MINFRESH: %d, PNO$: %d]",
      obj->m_cooked_stuff.m_cache_control.m_mask, obj->m_cooked_stuff.m_cache_control.m_secs_max_age,
      obj->m_cooked_stuff.m_cache_control.m_secs_s_maxage, obj->m_cooked_stuff.m_cache_control.m_secs_max_stale,
      obj->m_cooked_stuff.m_cache_control.m_secs_min_fresh, obj->m_cooked_stuff.m_pragma.m_no_cache);
  for (fblock = &(obj->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    if (recurse || (fblock == &(obj->m_first_fblock))) {
      obj_describe((HdrHeapObjImpl *)fblock, recurse);
    }
  }
}

void
mime_field_block_describe(HdrHeapObjImpl *raw, bool /* recurse ATS_UNUSED */)
{
  unsigned int       i;
  static const char *readiness_names[] = {"EMPTY", "DETACHED", "LIVE", "DELETED"};

  MIMEFieldBlockImpl *obj = (MIMEFieldBlockImpl *)raw;

  Dbg(dbg_ctl_http, "[FREETOP: %d, NEXTBLK: %p]", obj->m_freetop, obj->m_next);

  for (i = 0; i < obj->m_freetop; i++) {
    MIMEField *f = &(obj->m_field_slots[i]);
    Dbg(dbg_ctl_http, "\tSLOT #%2d (%p), %-8s", i, f, readiness_names[f->m_readiness]);

    switch (f->m_readiness) {
    case MIME_FIELD_SLOT_READINESS_EMPTY:
      break;
    case MIME_FIELD_SLOT_READINESS_DETACHED:
    case MIME_FIELD_SLOT_READINESS_LIVE:
    case MIME_FIELD_SLOT_READINESS_DELETED:
      Dbg(dbg_ctl_http, "[N: \"%.*s\", N_LEN: %d, N_IDX: %d, ", f->m_len_name, (f->m_ptr_name ? f->m_ptr_name : "NULL"),
          f->m_len_name, f->m_wks_idx);
      Dbg(dbg_ctl_http, "V: \"%.*s\", V_LEN: %d, ", f->m_len_value, (f->m_ptr_value ? f->m_ptr_value : "NULL"), f->m_len_value);
      Dbg(dbg_ctl_http, "NEXTDUP: %p, RAW: %d, RAWLEN: %d, F: %d]", f->m_next_dup, f->m_n_v_raw_printable,
          f->m_len_name + f->m_len_value + f->m_n_v_raw_printable_pad, f->m_flags);
      break;
    }
    Dbg(dbg_ctl_http, "\n");
  }
}

int
mime_hdr_print(MIMEHdrImpl const *mh, char *buf_start, int buf_length, int *buf_index_inout, int *buf_chars_to_skip_inout)
{
  MIMEFieldBlockImpl const *fblock;
  MIMEField const          *field;
  uint32_t                  index;

#define SIMPLE_MIME_HDR_PRINT
#ifdef SIMPLE_MIME_HDR_PRINT
  for (fblock = &(mh->m_first_fblock); fblock != nullptr; fblock = fblock->m_next) {
    for (index = 0; index < fblock->m_freetop; index++) {
      field = &(fblock->m_field_slots[index]);
      if (field->is_live()) {
        if (!mime_field_print(field, buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout)) {
          return 0;
        }
      }
    }
  }
#else
  // FIX: if not raw_printable, need to print with mime_field_print,
  //      not mime_mem_print
  for (fblock = &(mh->m_first_fblock); fblock != NULL; fblock = fblock->m_next) {
    const char *contig_start = NULL;
    int         this_length, contig_length = 0;
    for (index = 0; index < fblock->m_freetop; index++) {
      field       = &(fblock->m_field_slots[index]);
      this_length = field->m_len_name + field->m_len_value + field->m_n_v_raw_printable_pad;
      if (field->is_live()) {
        if ((field->m_ptr_name == contig_start + contig_length) && field->m_n_v_raw_printable &&
            ((buf_index_inout == NULL) || (contig_length + this_length <= buf_length - *buf_index_inout))) {
          contig_length += this_length;
        } else {
          if (contig_length > 0) {
            if (!mime_mem_print(std::string_view{contig_start, static_cast<std::string_view::size_type>(contig_length)}, buf_start,
                                buf_length, buf_index_inout, buf_chars_to_skip_inout))
              return 0;
          }
          contig_start  = field->m_ptr_name;
          contig_length = this_length;
        }
      }
    }

    if (contig_length > 0) {
      if (!mime_mem_print(std::string_view{ontig_start, static_cast<std::string_view::size_type>(contig_length)}, buf_start,
                          buf_length, buf_index_inout, buf_chars_to_skip_inout))
        return 0;
    }
  }
#endif

  if (!mime_mem_print("\r\n"sv, buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout)) {
    return 0;
  }

  return 1;
}

namespace
{
int
mime_mem_print_(std::string_view src, char *buf_start, int buf_length, int *buf_index_inout, int *buf_chars_to_skip_inout,
                int (*char_transform)(int char_in))
{
  if (buf_start == nullptr) { // this case should only be used by test_header
    ink_release_assert(buf_index_inout == nullptr);
    ink_release_assert(buf_chars_to_skip_inout == nullptr);
    for (auto c : src) {
      putchar(c);
    }
    return 1;
  }

  ink_assert(src.data() != nullptr);

  if (*buf_chars_to_skip_inout > 0) {
    if (*buf_chars_to_skip_inout >= static_cast<int>(src.length())) {
      *buf_chars_to_skip_inout -= static_cast<int>(src.length());
      return 1;
    } else {
      src.remove_prefix(*buf_chars_to_skip_inout);
      *buf_chars_to_skip_inout = 0;
    }
  }

  auto src_l{static_cast<int>(src.length())};
  int  copy_l = std::min(buf_length - *buf_index_inout, src_l);
  if (copy_l > 0) {
    buf_start += *buf_index_inout;
    auto src_d{src.data()};
    std::transform(src_d, src_d + copy_l, buf_start, char_transform);
    *buf_index_inout += copy_l;
  }
  return (src_l == copy_l);
}

int
to_same_char(int ch)
{
  return ch;
}

} // end anonymous namespace

int
mime_mem_print(std::string_view src, char *buf_start, int buf_length, int *buf_index_inout, int *buf_chars_to_skip_inout)
{
  return mime_mem_print_(src, buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout, to_same_char);
}

int
mime_mem_print_lc(std::string_view src, char *buf_start, int buf_length, int *buf_index_inout, int *buf_chars_to_skip_inout)
{
  return mime_mem_print_(src, buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout, std::tolower);
}

int
mime_field_print(MIMEField const *field, char *buf_start, int buf_length, int *buf_index_inout, int *buf_chars_to_skip_inout)
{
#define TRY(x) \
  if (!x)      \
  return 0

  int total_len;

  // Don't print names that begin with an '@'.
  if (field->m_ptr_name[0] == '@') {
    return 1;
  }

  if (field->m_n_v_raw_printable) {
    total_len = field->m_len_name + field->m_len_value + field->m_n_v_raw_printable_pad;

    if ((buf_start != nullptr) && (*buf_chars_to_skip_inout == 0) && (total_len <= (buf_length - *buf_index_inout))) {
      buf_start += *buf_index_inout;
      memcpy(buf_start, field->m_ptr_name, total_len);
      *buf_index_inout += total_len;

    } else {
      TRY(mime_mem_print(std::string_view{field->m_ptr_name, static_cast<std::string_view::size_type>(total_len)}, buf_start,
                         buf_length, buf_index_inout, buf_chars_to_skip_inout));
    }
  } else {
    total_len = field->m_len_name + field->m_len_value + 2 + 2;

    // try to handle on fast path

    if ((buf_start != nullptr) && (*buf_chars_to_skip_inout == 0) && (total_len <= (buf_length - *buf_index_inout))) {
      buf_start += *buf_index_inout;

      memcpy(buf_start, field->m_ptr_name, field->m_len_name);
      buf_start += field->m_len_name;

      buf_start[0]  = ':';
      buf_start[1]  = ' ';
      buf_start    += 2;

      memcpy(buf_start, field->m_ptr_value, field->m_len_value);
      buf_start += field->m_len_value;

      buf_start[0] = '\r';
      buf_start[1] = '\n';

      *buf_index_inout += total_len;
    } else {
      TRY(mime_mem_print(std::string_view{field->m_ptr_name, static_cast<std::string_view::size_type>(field->m_len_name)},
                         buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout));
      TRY(mime_mem_print(": "sv, buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout));
      TRY(mime_mem_print(std::string_view{field->m_ptr_value, static_cast<std::string_view::size_type>(field->m_len_value)},
                         buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout));
      TRY(mime_mem_print("\r\n"sv, buf_start, buf_length, buf_index_inout, buf_chars_to_skip_inout));
    }
  }

  return 1;

#undef TRY
}

const char *
mime_str_u16_set(HdrHeap *heap, std::string_view src, const char **d_str, uint16_t *d_len, bool must_copy)
{
  auto s_len{static_cast<int>(src.length())};
  ink_assert(s_len >= 0 && s_len < UINT16_MAX);
  // INKqa08287 - keep track of free string space.
  //  INVARIANT: passed in result pointers must be to
  //    either NULL or be valid ptr for a string already
  //    the string heaps
  heap->free_string(*d_str, *d_len);

  auto s_str{src.data()};
  if (must_copy && s_str) {
    s_str = heap->duplicate_str(s_str, s_len);
  }
  *d_str = s_str;
  *d_len = s_len;
  return s_str;
}

int
mime_field_length_get(MIMEField *field)
{
  if (field->m_n_v_raw_printable) {
    return (field->m_len_name + field->m_len_value + field->m_n_v_raw_printable_pad);
  } else {
    return (field->m_len_name + field->m_len_value + 4); // add ": \r\n"
  }
}

int
mime_format_int(char *buf, int32_t val, size_t buf_len)
{
  return ink_fast_itoa(val, buf, buf_len);
}

int
mime_format_uint(char *buf, uint32_t val, size_t buf_len)
{
  return ink_fast_uitoa(val, buf, buf_len);
}

int
mime_format_int64(char *buf, int64_t val, size_t buf_len)
{
  return ink_fast_ltoa(val, buf, buf_len);
}

void
mime_days_since_epoch_to_mdy_slowcase(time_t days_since_jan_1_1970, int *m_return, int *d_return, int *y_return)
{
  static const int DAYS_OFFSET = 25508;

  static const char months[] = {
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  6,
    6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,
    8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,
    9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1};

  static const int days[12] = {305, 336, -1, 30, 60, 91, 121, 152, 183, 213, 244, 274};

  time_t mday, year, month, d, dp;

  mday = days_since_jan_1_1970;

  /* guess the year and refine the guess */
  year = mday / 365 + 69;
  d = dp = (year * 365) + (year / 4) - (year / 100) + (year / 100 + 3) / 4 - DAYS_OFFSET - 1;

  while (dp < mday) {
    d     = dp;
    year += 1;
    dp    = (year * 365) + (year / 4) - (year / 100) + (year / 100 + 3) / 4 - DAYS_OFFSET - 1;
  }

  /* convert the days */
  d = mday - d;
  if ((d < 0) || (d > 366)) {
    ink_assert(!"bad date");
  } else {
    month = months[d];
    if (month > 1) {
      year -= 1;
    }

    mday  = d - days[month] - 1;
    year += 1900;

    // coverity[Y2K38_SAFETY:FALSE]
    *m_return = month;
    // coverity[Y2K38_SAFETY:FALSE]
    *d_return = mday;
    // coverity[Y2K38_SAFETY:FALSE]
    *y_return = year;
  }
}

void
mime_days_since_epoch_to_mdy(time_t days_since_jan_1_1970, int *m_return, int *d_return, int *y_return)
{
  ink_assert(_days_to_mdy_fast_lookup_table != nullptr);

  /////////////////////////////////////////////////////////////
  // if we have a fast lookup entry for this date, return it //
  /////////////////////////////////////////////////////////////

  if ((days_since_jan_1_1970 >= _days_to_mdy_fast_lookup_table_first_day) &&
      (days_since_jan_1_1970 <= _days_to_mdy_fast_lookup_table_last_day)) {
    ////////////////////////////////////////////////////////////////
    // to speed up the days_since_epoch to m/d/y conversion, we   //
    // use a pre-computed lookup table to support the common case //
    // of dates that are +/- one year from today.                 //
    ////////////////////////////////////////////////////////////////

    time_t i  = days_since_jan_1_1970 - _days_to_mdy_fast_lookup_table_first_day;
    *m_return = _days_to_mdy_fast_lookup_table[i].m;
    *d_return = _days_to_mdy_fast_lookup_table[i].d;
    *y_return = _days_to_mdy_fast_lookup_table[i].y;
    return;
  }
  ////////////////////////////////////
  // otherwise, return the slow way //
  ////////////////////////////////////

  mime_days_since_epoch_to_mdy_slowcase(days_since_jan_1_1970, m_return, d_return, y_return);
}

int
mime_format_date(char *buffer, time_t value)
{
  // must be 3 characters!
  static const char *daystrs[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

  // must be 3 characters!
  static const char *monthstrs[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  static const char *digitstrs[] = {
    "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
    "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
    "40", "41", "42", "43", "44", "45", "46", "47", "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
    "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
    "80", "81", "82", "83", "84", "85", "86", "87", "88", "89", "90", "91", "92", "93", "94", "95", "96", "97", "98", "99",
  };

  char *buf;
  int   sec, min, hour, wday, mday = 0, year = 0, month = 0;

  buf = buffer;

  sec    = static_cast<int>(value % 60);
  value /= 60;
  min    = static_cast<int>(value % 60);
  value /= 60;
  hour   = static_cast<int>(value % 24);
  value /= 24;

  /* Jan 1, 1970 was a Thursday */
  wday = static_cast<int>((4 + value) % 7);

/* value is days since Jan 1, 1970 */
#if MIME_FORMAT_DATE_USE_LOOKUP_TABLE
  mime_days_since_epoch_to_mdy(value, &month, &mday, &year);
#else
  mime_days_since_epoch_to_mdy_slowcase(value, &month, &mday, &year);
#endif

  /* arrange the date in the buffer */
  ink_assert((mday >= 0) && (mday <= 99));
  ink_assert((hour >= 0) && (hour <= 99));
  ink_assert((min >= 0) && (min <= 99));
  ink_assert((sec >= 0) && (sec <= 99));

  /* the day string */
  const char *three_char_day  = daystrs[wday];
  buf[0]                      = three_char_day[0];
  buf[1]                      = three_char_day[1];
  buf[2]                      = three_char_day[2];
  buf                        += 3;

  buf[0]  = ',';
  buf[1]  = ' ';
  buf    += 2;

  /* the day of month */
  buf[0]  = digitstrs[mday][0];
  buf[1]  = digitstrs[mday][1];
  buf[2]  = ' ';
  buf    += 3;

  /* the month string */
  const char *three_char_month  = monthstrs[month];
  buf[0]                        = three_char_month[0];
  buf[1]                        = three_char_month[1];
  buf[2]                        = three_char_month[2];
  buf                          += 3;

  /* the year */
  buf[0] = ' ';

  if ((year >= 2000) && (year <= 2009)) {
    buf[1] = '2';
    buf[2] = '0';
    buf[3] = '0';
    buf[4] = (year - 2000) + '0';
  } else if ((year >= 1990) && (year <= 1999)) {
    buf[1] = '1';
    buf[2] = '9';
    buf[3] = '9';
    buf[4] = (year - 1990) + '0';
  } else {
    buf[4]  = (year % 10) + '0';
    year   /= 10;
    buf[3]  = (year % 10) + '0';
    year   /= 10;
    buf[2]  = (year % 10) + '0';
    year   /= 10;
    buf[1]  = (year % 10) + '0';
  }
  buf[5]  = ' ';
  buf    += 6;

  /* the hour */
  buf[0]  = digitstrs[hour][0];
  buf[1]  = digitstrs[hour][1];
  buf[2]  = ':';
  buf    += 3;

  /* the minute */
  buf[0]  = digitstrs[min][0];
  buf[1]  = digitstrs[min][1];
  buf[2]  = ':';
  buf    += 3;

  /* the second */
  buf[0]  = digitstrs[sec][0];
  buf[1]  = digitstrs[sec][1];
  buf[2]  = ' ';
  buf    += 3;

  /* the timezone string */
  buf[0]  = 'G';
  buf[1]  = 'M';
  buf[2]  = 'T';
  buf[3]  = '\0';
  buf    += 3;

  return buf - buffer; // not counting NUL
}

int32_t
mime_parse_int(const char *buf, const char *end)
{
  int32_t num;
  bool    negative;

  if (!buf || (buf == end)) {
    return 0;
  }

  if (is_digit(*buf)) { // fast case
    num = *buf++ - '0';
    while ((buf != end) && is_digit(*buf)) {
      if (num != INT_MAX) {
        int new_num = (num * 10) + (*buf++ - '0');

        num = (new_num < num ? INT_MAX : new_num); // Check for overflow
      } else {
        ++buf; // Skip the remaining (valid) digits since we reached MAX/MIN_INT
      }
    }

    return num;
  } else {
    num      = 0;
    negative = false;

    while ((buf != end) && ParseRules::is_space(*buf)) {
      buf += 1;
    }

    if ((buf != end) && (*buf == '-')) {
      negative  = true;
      buf      += 1;
    }
    // NOTE: we first compute the value as negative then correct the
    // sign back to positive. This enables us to correctly parse MININT.
    while ((buf != end) && is_digit(*buf)) {
      if (num != INT_MIN) {
        int new_num = (num * 10) - (*buf++ - '0');

        num = (new_num > num ? INT_MIN : new_num); // Check for overflow, so to speak, see above re: negative
      } else {
        ++buf; // Skip the remaining (valid) digits since we reached MAX/MIN_INT
      }
    }

    if (!negative) {
      num = -num;
    }

    return num;
  }
}

uint32_t
mime_parse_uint(const char *buf, const char *end)
{
  uint32_t num;

  if (!buf || (buf == end)) {
    return 0;
  }

  if (is_digit(*buf)) // fast case
  {
    num = *buf++ - '0';
    while ((buf != end) && is_digit(*buf)) {
      num = (num * 10) + (*buf++ - '0');
    }
    return num;
  } else {
    num = 0;
    while ((buf != end) && ParseRules::is_space(*buf)) {
      buf += 1;
    }
    while ((buf != end) && is_digit(*buf)) {
      num = (num * 10) + (*buf++ - '0');
    }
    return num;
  }
}

int64_t
mime_parse_int64(const char *buf, const char *end)
{
  int64_t num;
  bool    negative;

  if (!buf || (buf == end)) {
    return 0;
  }

  if (is_digit(*buf)) // fast case
  {
    num = *buf++ - '0';
    while ((buf != end) && is_digit(*buf)) {
      num = (num * 10) + (*buf++ - '0');
    }
    return num;
  } else {
    num      = 0;
    negative = false;

    while ((buf != end) && ParseRules::is_space(*buf)) {
      buf += 1;
    }

    if ((buf != end) && (*buf == '-')) {
      negative  = true;
      buf      += 1;
    }
    // NOTE: we first compute the value as negative then correct the
    // sign back to positive. This enables us to correctly parse MININT.
    while ((buf != end) && is_digit(*buf)) {
      num = (num * 10) - (*buf++ - '0');
    }

    if (!negative) {
      num = -num;
    }

    return num;
  }
}

/*-------------------------------------------------------------------------

  mime_parse_rfc822_date_fastcase (const char *buf, int length, struct tm *tp)

  This routine is a fast case parser for date strings that are guaranteed
  to be in the rfc822/rfc1123 format.  It uses integer binary searches to
  convert daya and month names into integral indices.

  This routine only supports rfc822/rfc1123 dates.  It must be called
  with NO leading whitespace, with a length >= 29 characters, and with
  a comma in buf[3].  The caller MUST ensure these conditions.

        Sun, 06 Nov 1994 08:49:37 GMT
        01234567890123456789012345678

  This function handles the common case dates fast.  The day and month
  are processed as 3 character integers before falling to DFA.  This
  table shows string, hash (24 bit values of 3 characters), and the
  resulting string index.

        Fri 0x467269 5    Apr 0x417072 3
        Mon 0x4D6F6E 1    Aug 0x417567 7
        Sat 0x536174 6    Dec 0x446563 11
        Sun 0x53756E 0    Feb 0x466562 1
        Thu 0x546875 4    Jan 0x4A616E 0
        Tue 0x547565 2    Jul 0x4A756C 6
        Wed 0x576564 3    Jun 0x4A756E 5
                          Mar 0x4D6172 2
                          May 0x4D6179 4
                          Nov 0x4E6F76 10
                          Oct 0x4F6374 9
                          Sep 0x536570 8

  -------------------------------------------------------------------------*/
int
mime_parse_rfc822_date_fastcase(const char *buf, int length, struct tm *tp)
{
  unsigned int     three_char_wday, three_char_mon;
  std::string_view view{buf, size_t(length)};

  ink_assert(length >= 29);
  ink_assert(!is_ws(buf[0]));
  ink_assert(buf[3] == ',');

  ////////////////////////////
  // binary search for wday //
  ////////////////////////////
  tp->tm_wday     = -1;
  three_char_wday = (buf[0] << 16) | (buf[1] << 8) | buf[2];
  if (three_char_wday <= 0x53756E) {
    if (three_char_wday == 0x467269) {
      tp->tm_wday = 5;
    } else if (three_char_wday == 0x4D6F6E) {
      tp->tm_wday = 1;
    } else if (three_char_wday == 0x536174) {
      tp->tm_wday = 6;
    } else if (three_char_wday == 0x53756E) {
      tp->tm_wday = 0;
    }
  } else {
    if (three_char_wday == 0x546875) {
      tp->tm_wday = 4;
    } else if (three_char_wday == 0x547565) {
      tp->tm_wday = 2;
    } else if (three_char_wday == 0x576564) {
      tp->tm_wday = 3;
    }
  }
  if (tp->tm_wday < 0) {
    tp->tm_wday = day_names_dfa->match(view);
    if (tp->tm_wday < 0) {
      return 0;
    }
  }
  //////////////////////////
  // extract day of month //
  //////////////////////////
  tp->tm_mday = (buf[5] - '0') * 10 + (buf[6] - '0');

  /////////////////////////////
  // binary search for month //
  /////////////////////////////
  tp->tm_mon     = -1;
  three_char_mon = (buf[8] << 16) | (buf[9] << 8) | buf[10];
  if (three_char_mon <= 0x4A756C) {
    if (three_char_mon <= 0x446563) {
      if (three_char_mon == 0x417072) {
        tp->tm_mon = 3;
      } else if (three_char_mon == 0x417567) {
        tp->tm_mon = 7;
      } else if (three_char_mon == 0x446563) {
        tp->tm_mon = 11;
      }
    } else {
      if (three_char_mon == 0x466562) {
        tp->tm_mon = 1;
      } else if (three_char_mon == 0x4A616E) {
        tp->tm_mon = 0;
      } else if (three_char_mon == 0x4A756C) {
        tp->tm_mon = 6;
      }
    }
  } else {
    if (three_char_mon <= 0x4D6179) {
      if (three_char_mon == 0x4A756E) {
        tp->tm_mon = 5;
      } else if (three_char_mon == 0x4D6172) {
        tp->tm_mon = 2;
      } else if (three_char_mon == 0x4D6179) {
        tp->tm_mon = 4;
      }
    } else {
      if (three_char_mon == 0x4E6F76) {
        tp->tm_mon = 10;
      } else if (three_char_mon == 0x4F6374) {
        tp->tm_mon = 9;
      } else if (three_char_mon == 0x536570) {
        tp->tm_mon = 8;
      }
    }
  }
  if (tp->tm_mon < 0) {
    tp->tm_mon = month_names_dfa->match(view);
    if (tp->tm_mon < 0) {
      return 0;
    }
  }
  //////////////////
  // extract year //
  //////////////////
  tp->tm_year = ((buf[12] - '0') * 1000 + (buf[13] - '0') * 100 + (buf[14] - '0') * 10 + (buf[15] - '0')) - 1900;

  //////////////////
  // extract time //
  //////////////////
  tp->tm_hour = (buf[17] - '0') * 10 + (buf[18] - '0');
  tp->tm_min  = (buf[20] - '0') * 10 + (buf[21] - '0');
  tp->tm_sec  = (buf[23] - '0') * 10 + (buf[24] - '0');
  if ((buf[19] != ':') || (buf[22] != ':')) {
    return 0;
  }
  return 1;
}

/*-------------------------------------------------------------------------
   Sun, 06 Nov 1994 08:49:37 GMT  ; RFC 822, updated by RFC 1123
   Sunday, 06-Nov-94 08:49:37 GMT ; RFC 850, obsoleted by RFC 1036
   Sun Nov  6 08:49:37 1994       ; ANSI C's asctime() format
   6 Nov 1994 08:49:37 GMT        ; NNTP-style date
  -------------------------------------------------------------------------*/
time_t
mime_parse_date(const char *buf, const char *end)
{
  static const int DAYS_OFFSET = 25508;
  static const int days[12]    = {305, 336, -1, 30, 60, 91, 121, 152, 183, 213, 244, 274};

  struct tm tp;
  time_t    t;
  int       year;
  int       month;
  int       mday;

  if (!buf) {
    return static_cast<time_t>(0);
  }

  while ((buf != end) && is_ws(*buf)) {
    buf += 1;
  }

  if ((buf != end) && is_digit(*buf)) { // NNTP date
    if (!mime_parse_mday(buf, end, &tp.tm_mday)) {
      return static_cast<time_t>(0);
    }
    if (!mime_parse_month(buf, end, &tp.tm_mon)) {
      return static_cast<time_t>(0);
    }
    if (!mime_parse_year(buf, end, &tp.tm_year)) {
      return static_cast<time_t>(0);
    }
    if (!mime_parse_time(buf, end, &tp.tm_hour, &tp.tm_min, &tp.tm_sec)) {
      return static_cast<time_t>(0);
    }
  } else if (end && (end - buf >= 29) && (buf[3] == ',')) {
    if (!mime_parse_rfc822_date_fastcase(buf, end - buf, &tp)) {
      return static_cast<time_t>(0);
    }
  } else {
    if (!mime_parse_day(buf, end, &tp.tm_wday)) {
      return static_cast<time_t>(0);
    }

    while ((buf != end) && is_ws(*buf)) {
      buf += 1;
    }

    if ((buf != end) && ((*buf == ',') || is_digit(*buf))) {
      // RFC 822 or RFC 850 time format
      if (!mime_parse_mday(buf, end, &tp.tm_mday)) {
        return static_cast<time_t>(0);
      }
      if (!mime_parse_month(buf, end, &tp.tm_mon)) {
        return static_cast<time_t>(0);
      }
      if (!mime_parse_year(buf, end, &tp.tm_year)) {
        return static_cast<time_t>(0);
      }
      if (!mime_parse_time(buf, end, &tp.tm_hour, &tp.tm_min, &tp.tm_sec)) {
        return static_cast<time_t>(0);
      }
      // ignore timezone specifier...should always be GMT anyways
    } else {
      // ANSI C's asctime format
      if (!mime_parse_month(buf, end, &tp.tm_mon)) {
        return static_cast<time_t>(0);
      }
      if (!mime_parse_mday(buf, end, &tp.tm_mday)) {
        return static_cast<time_t>(0);
      }
      if (!mime_parse_time(buf, end, &tp.tm_hour, &tp.tm_min, &tp.tm_sec)) {
        return static_cast<time_t>(0);
      }
      if (!mime_parse_year(buf, end, &tp.tm_year)) {
        return static_cast<time_t>(0);
      }
    }
  }

  year  = tp.tm_year;
  month = tp.tm_mon;
  mday  = tp.tm_mday;

  // what should we do?
  if (year > 137) {
    return static_cast<time_t>(INT_MAX);
  }
  if (year < 70) {
    return static_cast<time_t>(0);
  }

  mday += days[month];
  /* month base == march */
  if (month < 2) {
    year -= 1;
  }
  mday += (year * 365) + (year / 4) - (year / 100) + (year / 100 + 3) / 4;
  mday -= DAYS_OFFSET;

  t = ((mday * 24 + tp.tm_hour) * 60 + tp.tm_min) * 60 + tp.tm_sec;

  return t;
}

bool
mime_parse_day(const char *&buf, const char *end, int *day)
{
  const char *e;

  while ((buf != end) && *buf && !ParseRules::is_alpha(*buf)) {
    buf += 1;
  }

  e = buf;
  while ((e != end) && *e && ParseRules::is_alpha(*e)) {
    e += 1;
  }

  *day = day_names_dfa->match({buf, size_t(e - buf)});
  if (*day < 0) {
    return false;
  } else {
    buf = e;
    return true;
  }
}

bool
mime_parse_month(const char *&buf, const char *end, int *month)
{
  const char *e;

  while ((buf != end) && *buf && !ParseRules::is_alpha(*buf)) {
    buf += 1;
  }

  e = buf;
  while ((e != end) && *e && ParseRules::is_alpha(*e)) {
    e += 1;
  }

  *month = month_names_dfa->match({buf, size_t(e - buf)});
  if (*month < 0) {
    return false;
  } else {
    buf = e;
    return true;
  }
}

bool
mime_parse_mday(const char *&buf, const char *end, int *mday)
{
  return mime_parse_integer(buf, end, mday);
}

bool
mime_parse_year(const char *&buf, const char *end, int *year)
{
  int val;

  while ((buf != end) && *buf && !is_digit(*buf)) {
    buf += 1;
  }

  if ((buf == end) || (*buf == '\0')) {
    return false;
  }

  val = 0;

  while ((buf != end) && *buf && is_digit(*buf)) {
    val = (val * 10) + (*buf++ - '0');
  }

  if (val >= 1900) {
    val -= 1900;
  } else if (val < 70) {
    val += 100;
  }

  *year = val;

  return true;
}

bool
mime_parse_time(const char *&buf, const char *end, int *hour, int *min, int *sec)
{
  if (!mime_parse_integer(buf, end, hour)) {
    return false;
  }
  if (!mime_parse_integer(buf, end, min)) {
    return false;
  }
  if (!mime_parse_integer(buf, end, sec)) {
    return false;
  }
  return true;
}

// This behaves slightly different than mime_parse_int(), int that we actually
// return a "bool" for success / failure on "reasonable" parsing. This kinda
// dumb, because we have two interfaces, where one does not move along the
// buf pointer, but this one does (and the ones using this function do).
bool
mime_parse_integer(const char *&buf, const char *end, int *integer)
{
  while ((buf != end) && *buf && !is_digit(*buf) && (*buf != '-')) {
    buf += 1;
  }

  if ((buf == end) || (*buf == '\0')) {
    return false;
  }

  int32_t num;
  bool    negative;

  // This code is copied verbatim from mime_parse_int ... Sigh. Maybe amc is right, and
  // we really need to clean this up. But, as such, we should redo all these interfaces,
  // and that's a big undertaking (and we'd want to move these strings all to string_view's).
  if (is_digit(*buf)) { // fast case
    num = *buf++ - '0';
    while ((buf != end) && is_digit(*buf)) {
      if (num != INT_MAX) {
        int new_num = (num * 10) + (*buf++ - '0');

        num = (new_num < num ? INT_MAX : new_num); // Check for overflow
      } else {
        ++buf; // Skip the remaining (valid) digits since we reached MAX/MIN_INT
      }
    }
  } else {
    num      = 0;
    negative = false;

    while ((buf != end) && ParseRules::is_space(*buf)) {
      buf += 1;
    }

    if ((buf != end) && (*buf == '-')) {
      negative  = true;
      buf      += 1;
    }
    // NOTE: we first compute the value as negative then correct the
    // sign back to positive. This enables us to correctly parse MININT.
    while ((buf != end) && is_digit(*buf)) {
      if (num != INT_MIN) {
        int new_num = (num * 10) - (*buf++ - '0');

        num = (new_num > num ? INT_MIN : new_num); // Check for overflow, so to speak, see above re: negative
      } else {
        ++buf; // Skip the remaining (valid) digits since we reached MAX/MIN_INT
      }
    }

    if (!negative) {
      num = -num;
    }
  }

  *integer = num;

  return true;
}

/***********************************************************************
 *                                                                     *
 *                        M A R S H A L I N G                          *
 *                                                                     *
 ***********************************************************************/
int
MIMEFieldBlockImpl::marshal(MarshalXlate *ptr_xlate, int num_ptr, MarshalXlate *str_xlate, int num_str)
{
  // printf("FieldBlockImpl:marshal  num_ptr = %d  num_str = %d\n", num_ptr, num_str);
  HDR_MARSHAL_PTR(m_next, MIMEFieldBlockImpl, ptr_xlate, num_ptr);

  if ((num_str == 1) && (num_ptr == 1)) {
    for (uint32_t index = 0; index < m_freetop; index++) {
      MIMEField *field = &(m_field_slots[index]);

      if (field->is_live()) {
        HDR_MARSHAL_STR_1(field->m_ptr_name, str_xlate);
        HDR_MARSHAL_STR_1(field->m_ptr_value, str_xlate);
        if (field->m_next_dup) {
          HDR_MARSHAL_PTR_1(field->m_next_dup, MIMEField, ptr_xlate);
        }
      }
    }
  } else {
    for (uint32_t index = 0; index < m_freetop; index++) {
      MIMEField *field = &(m_field_slots[index]);

      if (field->is_live()) {
        HDR_MARSHAL_STR(field->m_ptr_name, str_xlate, num_str);
        HDR_MARSHAL_STR(field->m_ptr_value, str_xlate, num_str);
        if (field->m_next_dup) {
          HDR_MARSHAL_PTR(field->m_next_dup, MIMEField, ptr_xlate, num_ptr);
        }
      }
    }
  }
  return 0;
}

void
MIMEFieldBlockImpl::unmarshal(intptr_t offset)
{
  HDR_UNMARSHAL_PTR(m_next, MIMEFieldBlockImpl, offset);

  for (uint32_t index = 0; index < m_freetop; index++) {
    MIMEField *field = &(m_field_slots[index]);

    if (field->is_live()) {
      HDR_UNMARSHAL_STR(field->m_ptr_name, offset);
      HDR_UNMARSHAL_STR(field->m_ptr_value, offset);
      if (field->m_next_dup) {
        HDR_UNMARSHAL_PTR(field->m_next_dup, MIMEField, offset);
      }
    } else {
      // Clear out other types of slots
      field->m_readiness = MIME_FIELD_SLOT_READINESS_EMPTY;
    }
  }
}

void
MIMEFieldBlockImpl::move_strings(HdrStrHeap *new_heap)
{
  for (uint32_t index = 0; index < m_freetop; index++) {
    MIMEField *field = &(m_field_slots[index]);

    if (field->is_live() || field->is_detached()) {
      // FIX ME - Should do the field in one shot and preserve
      //   raw_printable if it's set
      field->m_n_v_raw_printable = 0;

      HDR_MOVE_STR(field->m_ptr_name, field->m_len_name);
      HDR_MOVE_STR(field->m_ptr_value, field->m_len_value);
    }
  }
}

size_t
MIMEFieldBlockImpl::strings_length()
{
  size_t ret = 0;

  for (uint32_t index = 0; index < m_freetop; index++) {
    MIMEField *field = &(m_field_slots[index]);

    if (field->m_readiness == MIME_FIELD_SLOT_READINESS_LIVE || field->m_readiness == MIME_FIELD_SLOT_READINESS_DETACHED) {
      ret += field->m_len_name;
      ret += field->m_len_value;
    }
  }
  return ret;
}

bool
MIMEFieldBlockImpl::contains(const MIMEField *field)
{
  MIMEField *first = &(m_field_slots[0]);
  MIMEField *last  = &(m_field_slots[MIME_FIELD_BLOCK_SLOTS - 1]);
  return (field >= first) && (field <= last);
}

void
MIMEFieldBlockImpl::check_strings(HeapCheck *heaps, int num_heaps)
{
  for (uint32_t index = 0; index < m_freetop; index++) {
    MIMEField *field = &(m_field_slots[index]);

    if (field->is_live() || field->is_detached()) {
      // FIX ME - Should check raw printing characters as well
      CHECK_STR(field->m_ptr_name, field->m_len_name, heaps, num_heaps);
      CHECK_STR(field->m_ptr_value, field->m_len_value, heaps, num_heaps);
    }
  }
}

int
MIMEHdrImpl::marshal(MarshalXlate *ptr_xlate, int num_ptr, MarshalXlate *str_xlate, int num_str)
{
  // printf("MIMEHdrImpl:marshal  num_ptr = %d  num_str = %d\n", num_ptr, num_str);
  HDR_MARSHAL_PTR(m_fblock_list_tail, MIMEFieldBlockImpl, ptr_xlate, num_ptr);
  return m_first_fblock.marshal(ptr_xlate, num_ptr, str_xlate, num_str);
}

void
MIMEHdrImpl::unmarshal(intptr_t offset)
{
  HDR_UNMARSHAL_PTR(m_fblock_list_tail, MIMEFieldBlockImpl, offset);
  m_first_fblock.unmarshal(offset);
}

void
MIMEHdrImpl::move_strings(HdrStrHeap *new_heap)
{
  m_first_fblock.move_strings(new_heap);
}

size_t
MIMEHdrImpl::strings_length()
{
  return m_first_fblock.strings_length();
}

void
MIMEHdrImpl::check_strings(HeapCheck *heaps, int num_heaps)
{
  m_first_fblock.check_strings(heaps, num_heaps);
}

void
MIMEHdrImpl::recompute_accelerators_and_presence_bits()
{
  mime_hdr_reset_accelerators_and_presence_bits(this);
}

/***********************************************************************
 *                                                                     *
 *                 C O O K E D    V A L U E    C A C H E               *
 *                                                                     *
 ***********************************************************************/

////////////////////////////////////////////////////////
// we need to recook the cooked values cache when:    //
//                                                    //
//      setting the value and the field is live       //
//      attaching the field and the field isn't empty //
//      detaching the field                           //
////////////////////////////////////////////////////////

void
MIMEHdrImpl::recompute_cooked_stuff(MIMEField *changing_field_or_null)
{
  int         len, tlen;
  const char *s;
  const char *c;
  const char *e;
  const char *token_wks;
  MIMEField  *field;
  uint32_t    mask = 0;

  mime_hdr_cooked_stuff_init(this, changing_field_or_null);

  //////////////////////////////////////////////////
  // (1) cook the Cache-Control header if present //
  //////////////////////////////////////////////////

  // to be safe, recompute unless you know this call is for other cooked field
  if ((changing_field_or_null == nullptr) || (changing_field_or_null->m_wks_idx != MIME_WKSIDX_PRAGMA)) {
    field = mime_hdr_field_find(this, static_cast<std::string_view>(MIME_FIELD_CACHE_CONTROL));

    if (field) {
      // try pathpaths first -- unlike most other fastpaths, this one
      // is probably more useful for polygraph than for the real world
      if (!field->has_dups()) {
        auto val{field->value_get()};
        if (ptr_len_casecmp(val.data(), val.length(), "public", 6) == 0) {
          mask                                   = MIME_COOKED_MASK_CC_PUBLIC;
          m_cooked_stuff.m_cache_control.m_mask |= mask;
        } else if (ptr_len_casecmp(val.data(), val.length(), "private,no-cache", 16) == 0) {
          mask                                   = MIME_COOKED_MASK_CC_PRIVATE | MIME_COOKED_MASK_CC_NO_CACHE;
          m_cooked_stuff.m_cache_control.m_mask |= mask;
        }
      }

      if (mask == 0) {
        HdrCsvIter csv_iter;

        for (s = csv_iter.get_first(field, &len); s != nullptr; s = csv_iter.get_next(&len)) {
          e = s + len;
          for (c = s; (c < e) && (ParseRules::is_token(*c)); c++) {
            ;
          }
          tlen = c - s;

          // If >= 0 then this is a well known token
          if (hdrtoken_tokenize(s, tlen, &token_wks) >= 0) {
#if TRACK_COOKING
            Dbg(dbg_ctl_http, "recompute_cooked_stuff: got field '%s'", token_wks);
#endif

            HdrTokenHeapPrefix *p                  = hdrtoken_wks_to_prefix(token_wks);
            mask                                   = p->wks_type_specific.u.cache_control.cc_mask;
            m_cooked_stuff.m_cache_control.m_mask |= mask;

#if TRACK_COOKING
            Dbg(dbg_ctl_http, "                        set mask 0x%0X", mask);
#endif

            if (mask & (MIME_COOKED_MASK_CC_MAX_AGE | MIME_COOKED_MASK_CC_S_MAXAGE | MIME_COOKED_MASK_CC_MAX_STALE |
                        MIME_COOKED_MASK_CC_MIN_FRESH)) {
              int value;

              if (mime_parse_integer(c, e, &value)) {
#if TRACK_COOKING
                Dbg(dbg_ctl_http, "                        set integer value %d", value);
#endif
                if (token_wks == MIME_VALUE_MAX_AGE.c_str()) {
                  m_cooked_stuff.m_cache_control.m_secs_max_age = value;
                } else if (token_wks == MIME_VALUE_MIN_FRESH.c_str()) {
                  m_cooked_stuff.m_cache_control.m_secs_min_fresh = value;
                } else if (token_wks == MIME_VALUE_MAX_STALE.c_str()) {
                  m_cooked_stuff.m_cache_control.m_secs_max_stale = value;
                } else if (token_wks == MIME_VALUE_S_MAXAGE.c_str()) {
                  m_cooked_stuff.m_cache_control.m_secs_s_maxage = value;
                }
              } else {
#if TRACK_COOKING
                Dbg(dbg_ctl_http, "                        set integer value %d", INT_MAX);
#endif
                if (token_wks == MIME_VALUE_MAX_STALE.c_str()) {
                  m_cooked_stuff.m_cache_control.m_secs_max_stale = INT_MAX;
                }
              }
            }
          }
        }
      }
    }
  }
  ///////////////////////////////////////////
  // (2) cook the Pragma header if present //
  ///////////////////////////////////////////

  if ((changing_field_or_null == nullptr) || (changing_field_or_null->m_wks_idx != MIME_WKSIDX_CACHE_CONTROL)) {
    field = mime_hdr_field_find(this, static_cast<std::string_view>(MIME_FIELD_PRAGMA));
    if (field) {
      if (!field->has_dups()) { // try fastpath first
        auto val{field->value_get()};
        if (ptr_len_casecmp(val.data(), val.length(), "no-cache", 8) == 0) {
          m_cooked_stuff.m_pragma.m_no_cache = true;
          return;
        }
      }

      {
        HdrCsvIter csv_iter;

        for (s = csv_iter.get_first(field, &len); s != nullptr; s = csv_iter.get_next(&len)) {
          e = s + len;
          for (c = s; (c < e) && (ParseRules::is_token(*c)); c++) {
            ;
          }
          tlen = c - s;

          if (hdrtoken_tokenize(s, tlen, &token_wks) >= 0) {
            if (token_wks == MIME_VALUE_NO_CACHE.c_str()) {
              m_cooked_stuff.m_pragma.m_no_cache = true;
            }
          }
        }
      }
    }
  }
}

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
#include "I_EventSystem.h"

struct ChunkedHandler {
  enum ChunkedState {
    CHUNK_READ_CHUNK = 0,
    CHUNK_READ_SIZE_START,
    CHUNK_READ_SIZE,
    CHUNK_READ_SIZE_CRLF,
    CHUNK_READ_TRAILER_BLANK,
    CHUNK_READ_TRAILER_CR,
    CHUNK_READ_TRAILER_LINE,
    CHUNK_READ_ERROR,
    CHUNK_READ_DONE,
    CHUNK_WRITE_CHUNK,
    CHUNK_WRITE_DONE,
    CHUNK_FLOW_CONTROL
  };

  static int const DEFAULT_MAX_CHUNK_SIZE = 4096;

  enum Action { ACTION_DOCHUNK = 0, ACTION_DECHUNK, ACTION_PASSTHRU, ACTION_UNSET };

  Action action = ACTION_UNSET;

  IOBufferReader *chunked_reader = nullptr;
  MIOBuffer *dechunked_buffer    = nullptr;
  int64_t dechunked_size         = 0;

  IOBufferReader *dechunked_reader = nullptr;
  MIOBuffer *chunked_buffer        = nullptr;
  int64_t chunked_size             = 0;

  bool truncation    = false;
  int64_t skip_bytes = 0;

  ChunkedState state     = CHUNK_READ_CHUNK;
  int64_t cur_chunk_size = 0;
  int64_t bytes_left     = 0;
  int last_server_event  = VC_EVENT_NONE;

  // Parsing Info
  int running_sum = 0;
  int num_digits  = 0;

  /// @name Output data.
  //@{
  /// The maximum chunk size.
  /// This is the preferred size as well, used whenever possible.
  int64_t max_chunk_size;
  /// Caching members to avoid using printf on every chunk.
  /// It holds the header for a maximal sized chunk which will cover
  /// almost all output chunks.
  char max_chunk_header[16];
  int max_chunk_header_len = 0;
  //@}
  ChunkedHandler();

  void init(IOBufferReader *buffer_in, bool chunking, bool dechunking);
  void init_by_action(IOBufferReader *buffer_in, Action action);
  void clear();

  /// Set the max chunk @a size.
  /// If @a size is zero it is set to @c DEFAULT_MAX_CHUNK_SIZE.
  void set_max_chunk_size(int64_t size);

  // Returns true if complete, false otherwise
  bool process_chunked_content();
  bool generate_chunked_content();

private:
  void read_size();
  void read_chunk();
  void read_trailer();
  int64_t transfer_bytes();
};

/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_snoop"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "hci/include/btsnoop.h"
#include "hci/include/btsnoop_mem.h"
#include "bt_types.h"
#include "hci_layer.h"
#include "osi/include/log.h"
#include "stack_config.h"

typedef enum {
  kCommandPacket = 1,
  kAclPacket = 2,
  kScoPacket = 3,
  kEventPacket = 4
} packet_type_t;

// Epoch in microseconds since 01/01/0000.
static const uint64_t BTSNOOP_EPOCH_DELTA = 0x00dcddb30f2f8000ULL;

static const stack_config_t *stack_config;

static int logfile_fd = INVALID_FD;
static bool module_started;
static bool is_logging;
static bool logging_enabled_via_api;

// TODO(zachoverflow): merge btsnoop and btsnoop_net together
void btsnoop_net_open();
void btsnoop_net_close();
void btsnoop_net_write(const void *data, size_t length);

static void btsnoop_write_packet(packet_type_t type, const uint8_t *packet, bool is_received);
static void update_logging();

// Module lifecycle functions

static future_t *start_up(void) {
  module_started = true;
  update_logging();

  return NULL;
}

static future_t *shut_down(void) {
  module_started = false;
  update_logging();

  return NULL;
}

const module_t btsnoop_module = {
  .name = BTSNOOP_MODULE,
  .init = NULL,
  .start_up = start_up,
  .shut_down = shut_down,
  .clean_up = NULL,
  .dependencies = {
    STACK_CONFIG_MODULE,
    NULL
  }
};

// Interface functions

static void set_api_wants_to_log(bool value) {
  logging_enabled_via_api = value;
  update_logging();
}

static void capture(const BT_HDR *buffer, bool is_received) {
  const uint8_t *p = buffer->data + buffer->offset;

  btsnoop_mem_capture(buffer);

  if (logfile_fd == INVALID_FD)
    return;

  switch (buffer->event & MSG_EVT_MASK) {
    case MSG_HC_TO_STACK_HCI_EVT:
      btsnoop_write_packet(kEventPacket, p, false);
      break;
    case MSG_HC_TO_STACK_HCI_ACL:
    case MSG_STACK_TO_HC_HCI_ACL:
      btsnoop_write_packet(kAclPacket, p, is_received);
      break;
    case MSG_HC_TO_STACK_HCI_SCO:
    case MSG_STACK_TO_HC_HCI_SCO:
      btsnoop_write_packet(kScoPacket, p, is_received);
      break;
    case MSG_STACK_TO_HC_HCI_CMD:
      btsnoop_write_packet(kCommandPacket, p, true);
      break;
  }
}

static const btsnoop_t interface = {
  set_api_wants_to_log,
  capture
};

const btsnoop_t *btsnoop_get_interface() {
  stack_config = stack_config_get_interface();
  return &interface;
}

// Internal functions

static uint64_t btsnoop_timestamp(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);

  // Timestamp is in microseconds.
  uint64_t timestamp = tv.tv_sec * 1000 * 1000LL;
  timestamp += tv.tv_usec;
  timestamp += BTSNOOP_EPOCH_DELTA;
  return timestamp;
}

static void update_logging() {
  bool should_log = module_started &&
    (logging_enabled_via_api || stack_config->get_btsnoop_turned_on());

  if (should_log == is_logging)
    return;

  is_logging = should_log;
  if (should_log) {
    btsnoop_net_open();

    const char *log_path = stack_config->get_btsnoop_log_path();

    // Save the old log if configured to do so
    if (stack_config->get_btsnoop_should_save_last()) {
      char last_log_path[PATH_MAX];
      snprintf(last_log_path, PATH_MAX, "%s.%llu", log_path, btsnoop_timestamp());
      if (!rename(log_path, last_log_path) && errno != ENOENT)
        LOG_ERROR("%s unable to rename '%s' to '%s': %s", __func__, log_path, last_log_path, strerror(errno));
    }

    logfile_fd = TEMP_FAILURE_RETRY(open(log_path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH));
    if (logfile_fd == INVALID_FD) {
      LOG_ERROR("%s unable to open '%s': %s", __func__, log_path, strerror(errno));
      is_logging = false;
      return;
    }

    TEMP_FAILURE_RETRY(write(logfile_fd, "btsnoop\0\0\0\0\1\0\0\x3\xea", 16));
  } else {
    if (logfile_fd != INVALID_FD)
      close(logfile_fd);

    logfile_fd = INVALID_FD;
    btsnoop_net_close();
  }
}

static void btsnoop_write(const void *data, size_t length) {
  if (logfile_fd != INVALID_FD)
    TEMP_FAILURE_RETRY(write(logfile_fd, data, length));

  btsnoop_net_write(data, length);
}

static void btsnoop_write_packet(packet_type_t type, const uint8_t *packet, bool is_received) {
  int length_he = 0;
  int length;
  int flags;
  int drops = 0;
  switch (type) {
    case kCommandPacket:
      length_he = packet[2] + 4;
      flags = 2;
      break;
    case kAclPacket:
      length_he = (packet[3] << 8) + packet[2] + 5;
      flags = is_received;
      break;
    case kScoPacket:
      length_he = packet[2] + 4;
      flags = is_received;
      break;
    case kEventPacket:
      length_he = packet[1] + 3;
      flags = 3;
      break;
  }

  uint64_t timestamp = btsnoop_timestamp();
  uint32_t time_hi = timestamp >> 32;
  uint32_t time_lo = timestamp & 0xFFFFFFFF;

  length = htonl(length_he);
  flags = htonl(flags);
  drops = htonl(drops);
  time_hi = htonl(time_hi);
  time_lo = htonl(time_lo);

  btsnoop_write(&length, 4);
  btsnoop_write(&length, 4);
  btsnoop_write(&flags, 4);
  btsnoop_write(&drops, 4);
  btsnoop_write(&time_hi, 4);
  btsnoop_write(&time_lo, 4);
  btsnoop_write(&type, 1);
  btsnoop_write(packet, length_he - 1);
}

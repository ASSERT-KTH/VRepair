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

#define LOG_TAG "bt_hci"

#include <assert.h>
#include <cutils/properties.h>
#include <string.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>

#include "buffer_allocator.h"
#include "btsnoop.h"
#include "osi/include/fixed_queue.h"
#include "osi/include/future.h"
#include "hcidefs.h"
#include "hcimsgs.h"
#include "hci_hal.h"
#include "hci_internals.h"
#include "hci_inject.h"
#include "hci_layer.h"
#include "osi/include/list.h"
#include "low_power_manager.h"
#include "btcore/include/module.h"
#include "osi/include/non_repeating_timer.h"
#include "osi/include/osi.h"
#include "osi/include/log.h"
#include "packet_fragmenter.h"
#include "osi/include/reactor.h"
#include "vendor.h"

// TODO(zachoverflow): remove this hack extern
#include <hardware/bluetooth.h>
bt_bdaddr_t btif_local_bd_addr;

#define INBOUND_PACKET_TYPE_COUNT 3
#define PACKET_TYPE_TO_INBOUND_INDEX(type) ((type) - 2)
#define PACKET_TYPE_TO_INDEX(type) ((type) - 1)

#define PREAMBLE_BUFFER_SIZE 4 // max preamble size, ACL
#define RETRIEVE_ACL_LENGTH(preamble) ((((preamble)[3]) << 8) | (preamble)[2])

static const uint8_t preamble_sizes[] = {
  HCI_COMMAND_PREAMBLE_SIZE,
  HCI_ACL_PREAMBLE_SIZE,
  HCI_SCO_PREAMBLE_SIZE,
  HCI_EVENT_PREAMBLE_SIZE
};

static const uint16_t outbound_event_types[] =
{
  MSG_HC_TO_STACK_HCI_ERR,
  MSG_HC_TO_STACK_HCI_ACL,
  MSG_HC_TO_STACK_HCI_SCO,
  MSG_HC_TO_STACK_HCI_EVT
};

typedef enum {
  BRAND_NEW,
  PREAMBLE,
  BODY,
  IGNORE,
  FINISHED
} receive_state_t;

typedef struct {
  receive_state_t state;
  uint16_t bytes_remaining;
  uint8_t preamble[PREAMBLE_BUFFER_SIZE];
  uint16_t index;
  BT_HDR *buffer;
} packet_receive_data_t;

typedef struct {
  uint16_t opcode;
  future_t *complete_future;
  command_complete_cb complete_callback;
  command_status_cb status_callback;
  void *context;
  BT_HDR *command;
} waiting_command_t;

// Using a define here, because it can be stringified for the property lookup
#define DEFAULT_STARTUP_TIMEOUT_MS 8000
#define STRING_VALUE_OF(x) #x

static const uint32_t EPILOG_TIMEOUT_MS = 3000;
static const uint32_t COMMAND_PENDING_TIMEOUT = 8000;

// Our interface
static bool interface_created;
static hci_t interface;

// Modules we import and callbacks we export
static const allocator_t *buffer_allocator;
static const btsnoop_t *btsnoop;
static const hci_hal_t *hal;
static const hci_hal_callbacks_t hal_callbacks;
static const hci_inject_t *hci_inject;
static const low_power_manager_t *low_power_manager;
static const packet_fragmenter_t *packet_fragmenter;
static const packet_fragmenter_callbacks_t packet_fragmenter_callbacks;
static const vendor_t *vendor;

static future_t *startup_future;
static thread_t *thread; // We own this

static volatile bool firmware_is_configured = false;
static non_repeating_timer_t *epilog_timer;
static non_repeating_timer_t *startup_timer;

// Outbound-related
static int command_credits = 1;
static fixed_queue_t *command_queue;
static fixed_queue_t *packet_queue;

// Inbound-related
static non_repeating_timer_t *command_response_timer;
static list_t *commands_pending_response;
static pthread_mutex_t commands_pending_response_lock;
static packet_receive_data_t incoming_packets[INBOUND_PACKET_TYPE_COUNT];

// The hand-off point for data going to a higher layer, set by the higher layer
static fixed_queue_t *upwards_data_queue;

static future_t *shut_down();

static void event_finish_startup(void *context);
static void firmware_config_callback(bool success);
static void startup_timer_expired(void *context);

static void event_postload(void *context);
static void sco_config_callback(bool success);

static void event_epilog(void *context);
static void epilog_finished_callback(bool success);
static void epilog_timer_expired(void *context);

static void event_command_ready(fixed_queue_t *queue, void *context);
static void event_packet_ready(fixed_queue_t *queue, void *context);
static void command_timed_out(void *context);

static void hal_says_data_ready(serial_data_type_t type);
static bool filter_incoming_event(BT_HDR *packet);

static serial_data_type_t event_to_data_type(uint16_t event);
static waiting_command_t *get_waiting_command(command_opcode_t opcode);

// Module lifecycle functions

static future_t *start_up(void) {
  LOG_INFO("%s", __func__);

  // The host is only allowed to send at most one command initially,
  // as per the Bluetooth spec, Volume 2, Part E, 4.4 (Command Flow Control)
  // This value can change when you get a command complete or command status event.
  command_credits = 1;
  firmware_is_configured = false;

  pthread_mutex_init(&commands_pending_response_lock, NULL);

  // Grab the override startup timeout ms, if present.
  period_ms_t startup_timeout_ms;
  char timeout_prop[PROPERTY_VALUE_MAX];
  if (!property_get("bluetooth.enable_timeout_ms", timeout_prop, STRING_VALUE_OF(DEFAULT_STARTUP_TIMEOUT_MS))
      || (startup_timeout_ms = atoi(timeout_prop)) < 100)
    startup_timeout_ms = DEFAULT_STARTUP_TIMEOUT_MS;

  startup_timer = non_repeating_timer_new(startup_timeout_ms, startup_timer_expired, NULL);
  if (!startup_timer) {
    LOG_ERROR("%s unable to create startup timer.", __func__);
    goto error;
  }

  // Make sure we run in a bounded amount of time
  non_repeating_timer_restart(startup_timer);

  epilog_timer = non_repeating_timer_new(EPILOG_TIMEOUT_MS, epilog_timer_expired, NULL);
  if (!epilog_timer) {
    LOG_ERROR("%s unable to create epilog timer.", __func__);
    goto error;
  }

  command_response_timer = non_repeating_timer_new(COMMAND_PENDING_TIMEOUT, command_timed_out, NULL);
  if (!command_response_timer) {
    LOG_ERROR("%s unable to create command response timer.", __func__);
    goto error;
  }

  command_queue = fixed_queue_new(SIZE_MAX);
  if (!command_queue) {
    LOG_ERROR("%s unable to create pending command queue.", __func__);
    goto error;
  }

  packet_queue = fixed_queue_new(SIZE_MAX);
  if (!packet_queue) {
    LOG_ERROR("%s unable to create pending packet queue.", __func__);
    goto error;
  }

  thread = thread_new("hci_thread");
  if (!thread) {
    LOG_ERROR("%s unable to create thread.", __func__);
    goto error;
  }

  commands_pending_response = list_new(NULL);
  if (!commands_pending_response) {
    LOG_ERROR("%s unable to create list for commands pending response.", __func__);
    goto error;
  }

  memset(incoming_packets, 0, sizeof(incoming_packets));

  packet_fragmenter->init(&packet_fragmenter_callbacks);

  fixed_queue_register_dequeue(command_queue, thread_get_reactor(thread), event_command_ready, NULL);
  fixed_queue_register_dequeue(packet_queue, thread_get_reactor(thread), event_packet_ready, NULL);

  vendor->open(btif_local_bd_addr.address, &interface);
  hal->init(&hal_callbacks, thread);
  low_power_manager->init(thread);

  vendor->set_callback(VENDOR_CONFIGURE_FIRMWARE, firmware_config_callback);
  vendor->set_callback(VENDOR_CONFIGURE_SCO, sco_config_callback);
  vendor->set_callback(VENDOR_DO_EPILOG, epilog_finished_callback);

  if (!hci_inject->open(&interface)) {
    // TODO(sharvil): gracefully propagate failures from this layer.
  }

  int power_state = BT_VND_PWR_OFF;
#if (defined (BT_CLEAN_TURN_ON_DISABLED) && BT_CLEAN_TURN_ON_DISABLED == TRUE)
  LOG_WARN("%s not turning off the chip before turning on.", __func__);
  // So apparently this hack was needed in the past because a Wingray kernel driver
  // didn't handle power off commands in a powered off state correctly.

  // The comment in the old code said the workaround should be removed when the
  // problem was fixed. Sadly, I have no idea if said bug was fixed or if said
  // kernel is still in use, so we must leave this here for posterity. #sadpanda
#else
  // cycle power on the chip to ensure it has been reset
  vendor->send_command(VENDOR_CHIP_POWER_CONTROL, &power_state);
#endif
  power_state = BT_VND_PWR_ON;
  vendor->send_command(VENDOR_CHIP_POWER_CONTROL, &power_state);

  startup_future = future_new();
  LOG_DEBUG("%s starting async portion", __func__);
  thread_post(thread, event_finish_startup, NULL);
  return startup_future;
error:;
  shut_down(); // returns NULL so no need to wait for it
  return future_new_immediate(FUTURE_FAIL);
}

static future_t *shut_down() {
  LOG_INFO("%s", __func__);

  hci_inject->close();

  if (thread) {
    if (firmware_is_configured) {
      non_repeating_timer_restart(epilog_timer);
      thread_post(thread, event_epilog, NULL);
    } else {
      thread_stop(thread);
    }

    thread_join(thread);
  }

  fixed_queue_free(command_queue, osi_free);
  fixed_queue_free(packet_queue, buffer_allocator->free);
  list_free(commands_pending_response);

  pthread_mutex_destroy(&commands_pending_response_lock);

  packet_fragmenter->cleanup();

  non_repeating_timer_free(epilog_timer);
  non_repeating_timer_free(command_response_timer);
  non_repeating_timer_free(startup_timer);

  epilog_timer = NULL;
  command_response_timer = NULL;

  low_power_manager->cleanup();
  hal->close();

  // Turn off the chip
  int power_state = BT_VND_PWR_OFF;
  vendor->send_command(VENDOR_CHIP_POWER_CONTROL, &power_state);
  vendor->close();

  thread_free(thread);
  thread = NULL;
  firmware_is_configured = false;

  return NULL;
}

const module_t hci_module = {
  .name = HCI_MODULE,
  .init = NULL,
  .start_up = start_up,
  .shut_down = shut_down,
  .clean_up = NULL,
  .dependencies = {
    BTSNOOP_MODULE,
    NULL
  }
};

// Interface functions

static void do_postload() {
  LOG_DEBUG("%s posting postload work item", __func__);
  thread_post(thread, event_postload, NULL);
}

static void set_data_queue(fixed_queue_t *queue) {
  upwards_data_queue = queue;
}

static void transmit_command(
    BT_HDR *command,
    command_complete_cb complete_callback,
    command_status_cb status_callback,
    void *context) {
  waiting_command_t *wait_entry = osi_calloc(sizeof(waiting_command_t));
  if (!wait_entry) {
    LOG_ERROR("%s couldn't allocate space for wait entry.", __func__);
    return;
  }

  uint8_t *stream = command->data + command->offset;
  STREAM_TO_UINT16(wait_entry->opcode, stream);
  wait_entry->complete_callback = complete_callback;
  wait_entry->status_callback = status_callback;
  wait_entry->command = command;
  wait_entry->context = context;

  // Store the command message type in the event field
  // in case the upper layer didn't already
  command->event = MSG_STACK_TO_HC_HCI_CMD;

  fixed_queue_enqueue(command_queue, wait_entry);
}

static future_t *transmit_command_futured(BT_HDR *command) {
  waiting_command_t *wait_entry = osi_calloc(sizeof(waiting_command_t));
  assert(wait_entry != NULL);

  future_t *future = future_new();

  uint8_t *stream = command->data + command->offset;
  STREAM_TO_UINT16(wait_entry->opcode, stream);
  wait_entry->complete_future = future;
  wait_entry->command = command;

  // Store the command message type in the event field
  // in case the upper layer didn't already
  command->event = MSG_STACK_TO_HC_HCI_CMD;

  fixed_queue_enqueue(command_queue, wait_entry);
  return future;
}

static void transmit_downward(data_dispatcher_type_t type, void *data) {
  if (type == MSG_STACK_TO_HC_HCI_CMD) {
    // TODO(zachoverflow): eliminate this call
    transmit_command((BT_HDR *)data, NULL, NULL, NULL);
    LOG_WARN("%s legacy transmit of command. Use transmit_command instead.", __func__);
  } else {
    fixed_queue_enqueue(packet_queue, data);
  }
}

// Start up functions

static void event_finish_startup(UNUSED_ATTR void *context) {
  LOG_INFO("%s", __func__);
  hal->open();
  vendor->send_async_command(VENDOR_CONFIGURE_FIRMWARE, NULL);
}

static void firmware_config_callback(UNUSED_ATTR bool success) {
  LOG_INFO("%s", __func__);
  firmware_is_configured = true;
  non_repeating_timer_cancel(startup_timer);

  future_ready(startup_future, FUTURE_SUCCESS);
  startup_future = NULL;
}

static void startup_timer_expired(UNUSED_ATTR void *context) {
  LOG_ERROR("%s", __func__);
  future_ready(startup_future, FUTURE_FAIL);
  startup_future = NULL;
}

// Postload functions

static void event_postload(UNUSED_ATTR void *context) {
  LOG_INFO("%s", __func__);
  if(vendor->send_async_command(VENDOR_CONFIGURE_SCO, NULL) == -1) {
    // If couldn't configure sco, we won't get the sco configuration callback
    // so go pretend to do it now
    sco_config_callback(false);

  }
}

static void sco_config_callback(UNUSED_ATTR bool success) {
  LOG_INFO("%s postload finished.", __func__);
}

// Epilog functions

static void event_epilog(UNUSED_ATTR void *context) {
  vendor->send_async_command(VENDOR_DO_EPILOG, NULL);
}

static void epilog_finished_callback(UNUSED_ATTR bool success) {
  LOG_INFO("%s", __func__);
  thread_stop(thread);
}

static void epilog_timer_expired(UNUSED_ATTR void *context) {
  LOG_INFO("%s", __func__);
  thread_stop(thread);
}

// Command/packet transmitting functions

static void event_command_ready(fixed_queue_t *queue, UNUSED_ATTR void *context) {
  if (command_credits > 0) {
    waiting_command_t *wait_entry = fixed_queue_dequeue(queue);
    command_credits--;

    // Move it to the list of commands awaiting response
    pthread_mutex_lock(&commands_pending_response_lock);
    list_append(commands_pending_response, wait_entry);
    pthread_mutex_unlock(&commands_pending_response_lock);

    // Send it off
    low_power_manager->wake_assert();
    packet_fragmenter->fragment_and_dispatch(wait_entry->command);
    low_power_manager->transmit_done();

    non_repeating_timer_restart_if(command_response_timer, !list_is_empty(commands_pending_response));
  }
}

static void event_packet_ready(fixed_queue_t *queue, UNUSED_ATTR void *context) {
  // The queue may be the command queue or the packet queue, we don't care
  BT_HDR *packet = (BT_HDR *)fixed_queue_dequeue(queue);

  low_power_manager->wake_assert();
  packet_fragmenter->fragment_and_dispatch(packet);
  low_power_manager->transmit_done();
}

// Callback for the fragmenter to send a fragment
static void transmit_fragment(BT_HDR *packet, bool send_transmit_finished) {
  uint16_t event = packet->event & MSG_EVT_MASK;
  serial_data_type_t type = event_to_data_type(event);

  btsnoop->capture(packet, false);
  hal->transmit_data(type, packet->data + packet->offset, packet->len);

  if (event != MSG_STACK_TO_HC_HCI_CMD && send_transmit_finished)
    buffer_allocator->free(packet);
}

static void fragmenter_transmit_finished(BT_HDR *packet, bool all_fragments_sent) {
  if (all_fragments_sent) {
    buffer_allocator->free(packet);
  } else {
    // This is kind of a weird case, since we're dispatching a partially sent packet
    // up to a higher layer.
    // TODO(zachoverflow): rework upper layer so this isn't necessary.
    data_dispatcher_dispatch(interface.event_dispatcher, packet->event & MSG_EVT_MASK, packet);
  }
}

static void command_timed_out(UNUSED_ATTR void *context) {
  pthread_mutex_lock(&commands_pending_response_lock);

  if (list_is_empty(commands_pending_response)) {
    LOG_ERROR("%s with no commands pending response", __func__);
  } else {
    waiting_command_t *wait_entry = list_front(commands_pending_response);
    pthread_mutex_unlock(&commands_pending_response_lock);

    // We shouldn't try to recover the stack from this command timeout.
    // If it's caused by a software bug, fix it. If it's a hardware bug, fix it.
    LOG_ERROR("%s hci layer timeout waiting for response to a command. opcode: 0x%x", __func__, wait_entry->opcode);
  }

  LOG_ERROR("%s restarting the bluetooth process.", __func__);
  usleep(10000);
  kill(getpid(), SIGKILL);
}

// Event/packet receiving functions

// This function is not required to read all of a packet in one go, so
// be wary of reentry. But this function must return after finishing a packet.
static void hal_says_data_ready(serial_data_type_t type) {
  packet_receive_data_t *incoming = &incoming_packets[PACKET_TYPE_TO_INBOUND_INDEX(type)];

  uint8_t byte;
  while (hal->read_data(type, &byte, 1, false) != 0) {
    switch (incoming->state) {
      case BRAND_NEW:
        // Initialize and prepare to jump to the preamble reading state
        incoming->bytes_remaining = preamble_sizes[PACKET_TYPE_TO_INDEX(type)];
        memset(incoming->preamble, 0, PREAMBLE_BUFFER_SIZE);
        incoming->index = 0;
        incoming->state = PREAMBLE;
        // INTENTIONAL FALLTHROUGH
      case PREAMBLE:
        incoming->preamble[incoming->index] = byte;
        incoming->index++;
        incoming->bytes_remaining--;

        if (incoming->bytes_remaining == 0) {
          // For event and sco preambles, the last byte we read is the length
          incoming->bytes_remaining = (type == DATA_TYPE_ACL) ? RETRIEVE_ACL_LENGTH(incoming->preamble) : byte;

          size_t buffer_size = BT_HDR_SIZE + incoming->index + incoming->bytes_remaining;
          incoming->buffer = (BT_HDR *)buffer_allocator->alloc(buffer_size);

          if (!incoming->buffer) {
            LOG_ERROR("%s error getting buffer for incoming packet of type %d and size %zd", __func__, type, buffer_size);
            // Can't read any more of this current packet, so jump out
            incoming->state = incoming->bytes_remaining == 0 ? BRAND_NEW : IGNORE;
            break;
          }

          // Initialize the buffer
          incoming->buffer->offset = 0;
          incoming->buffer->layer_specific = 0;
          incoming->buffer->event = outbound_event_types[PACKET_TYPE_TO_INDEX(type)];
          memcpy(incoming->buffer->data, incoming->preamble, incoming->index);

          incoming->state = incoming->bytes_remaining > 0 ? BODY : FINISHED;
        }

        break;
      case BODY:
        incoming->buffer->data[incoming->index] = byte;
        incoming->index++;
        incoming->bytes_remaining--;

        size_t bytes_read = hal->read_data(type, (incoming->buffer->data + incoming->index), incoming->bytes_remaining, false);
        incoming->index += bytes_read;
        incoming->bytes_remaining -= bytes_read;

        incoming->state = incoming->bytes_remaining == 0 ? FINISHED : incoming->state;
        break;
      case IGNORE:
        incoming->bytes_remaining--;
        if (incoming->bytes_remaining == 0) {
          incoming->state = BRAND_NEW;
          // Don't forget to let the hal know we finished the packet we were ignoring.
          // Otherwise we'll get out of sync with hals that embed extra information
          // in the uart stream (like H4). #badnewsbears
          hal->packet_finished(type);
          return;
        }

        break;
      case FINISHED:
        LOG_ERROR("%s the state machine should not have been left in the finished state.", __func__);
        break;
    }

    if (incoming->state == FINISHED) {
      incoming->buffer->len = incoming->index;
      btsnoop->capture(incoming->buffer, true);

      if (type != DATA_TYPE_EVENT) {
        packet_fragmenter->reassemble_and_dispatch(incoming->buffer);
      } else if (!filter_incoming_event(incoming->buffer)) {
        // Dispatch the event by event code
        uint8_t *stream = incoming->buffer->data;
        uint8_t event_code;
        STREAM_TO_UINT8(event_code, stream);

        data_dispatcher_dispatch(
          interface.event_dispatcher,
          event_code,
          incoming->buffer
        );
      }

      // We don't control the buffer anymore
      incoming->buffer = NULL;
      incoming->state = BRAND_NEW;
      hal->packet_finished(type);

      // We return after a packet is finished for two reasons:
      // 1. The type of the next packet could be different.
      // 2. We don't want to hog cpu time.
      return;
    }
  }
}

// Returns true if the event was intercepted and should not proceed to
// higher layers. Also inspects an incoming event for interesting
// information, like how many commands are now able to be sent.
static bool filter_incoming_event(BT_HDR *packet) {
  waiting_command_t *wait_entry = NULL;
  uint8_t *stream = packet->data;
  uint8_t event_code;
  command_opcode_t opcode;

  STREAM_TO_UINT8(event_code, stream);
  STREAM_SKIP_UINT8(stream); // Skip the parameter total length field

  if (event_code == HCI_COMMAND_COMPLETE_EVT) {
    STREAM_TO_UINT8(command_credits, stream);
    STREAM_TO_UINT16(opcode, stream);

    wait_entry = get_waiting_command(opcode);
    if (!wait_entry)
      LOG_WARN("%s command complete event with no matching command. opcode: 0x%x.", __func__, opcode);
    else if (wait_entry->complete_callback)
      wait_entry->complete_callback(packet, wait_entry->context);
    else if (wait_entry->complete_future)
      future_ready(wait_entry->complete_future, packet);

    goto intercepted;
  } else if (event_code == HCI_COMMAND_STATUS_EVT) {
    uint8_t status;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(command_credits, stream);
    STREAM_TO_UINT16(opcode, stream);

    // If a command generates a command status event, it won't be getting a command complete event

    wait_entry = get_waiting_command(opcode);
    if (!wait_entry)
      LOG_WARN("%s command status event with no matching command. opcode: 0x%x", __func__, opcode);
    else if (wait_entry->status_callback)
      wait_entry->status_callback(status, wait_entry->command, wait_entry->context);

    goto intercepted;
  }

  return false;
intercepted:;
  non_repeating_timer_restart_if(command_response_timer, !list_is_empty(commands_pending_response));

  if (wait_entry) {
    // If it has a callback, it's responsible for freeing the packet
    if (event_code == HCI_COMMAND_STATUS_EVT || (!wait_entry->complete_callback && !wait_entry->complete_future))
      buffer_allocator->free(packet);

    // If it has a callback, it's responsible for freeing the command
    if (event_code == HCI_COMMAND_COMPLETE_EVT || !wait_entry->status_callback)
      buffer_allocator->free(wait_entry->command);

    osi_free(wait_entry);
  } else {
    buffer_allocator->free(packet);
  }

  return true;
}

// Callback for the fragmenter to dispatch up a completely reassembled packet
static void dispatch_reassembled(BT_HDR *packet) {
  // Events should already have been dispatched before this point
  assert((packet->event & MSG_EVT_MASK) != MSG_HC_TO_STACK_HCI_EVT);
  assert(upwards_data_queue != NULL);

  if (upwards_data_queue) {
    fixed_queue_enqueue(upwards_data_queue, packet);
  } else {
    LOG_ERROR("%s had no queue to place upwards data packet in. Dropping it on the floor.", __func__);
    buffer_allocator->free(packet);
  }
}

// Misc internal functions

// TODO(zachoverflow): we seem to do this a couple places, like the HCI inject module. #centralize
static serial_data_type_t event_to_data_type(uint16_t event) {
  if (event == MSG_STACK_TO_HC_HCI_ACL)
    return DATA_TYPE_ACL;
  else if (event == MSG_STACK_TO_HC_HCI_SCO)
    return DATA_TYPE_SCO;
  else if (event == MSG_STACK_TO_HC_HCI_CMD)
    return DATA_TYPE_COMMAND;
  else
    LOG_ERROR("%s invalid event type, could not translate 0x%x", __func__, event);

  return 0;
}

static waiting_command_t *get_waiting_command(command_opcode_t opcode) {
  pthread_mutex_lock(&commands_pending_response_lock);

  for (const list_node_t *node = list_begin(commands_pending_response);
      node != list_end(commands_pending_response);
      node = list_next(node)) {
    waiting_command_t *wait_entry = list_node(node);

    if (!wait_entry || wait_entry->opcode != opcode)
      continue;

    list_remove(commands_pending_response, wait_entry);

    pthread_mutex_unlock(&commands_pending_response_lock);
    return wait_entry;
  }

  pthread_mutex_unlock(&commands_pending_response_lock);
  return NULL;
}

static void init_layer_interface() {
  if (!interface_created) {
    interface.send_low_power_command = low_power_manager->post_command;
    interface.do_postload = do_postload;

    // It's probably ok for this to live forever. It's small and
    // there's only one instance of the hci interface.
    interface.event_dispatcher = data_dispatcher_new("hci_layer");
    if (!interface.event_dispatcher) {
      LOG_ERROR("%s could not create upward dispatcher.", __func__);
      return;
    }

    interface.set_data_queue = set_data_queue;
    interface.transmit_command = transmit_command;
    interface.transmit_command_futured = transmit_command_futured;
    interface.transmit_downward = transmit_downward;
    interface_created = true;
  }
}

static const hci_hal_callbacks_t hal_callbacks = {
  hal_says_data_ready
};

static const packet_fragmenter_callbacks_t packet_fragmenter_callbacks = {
  transmit_fragment,
  dispatch_reassembled,
  fragmenter_transmit_finished
};

const hci_t *hci_layer_get_interface() {
  buffer_allocator = buffer_allocator_get_interface();
  hal = hci_hal_get_interface();
  btsnoop = btsnoop_get_interface();
  hci_inject = hci_inject_get_interface();
  packet_fragmenter = packet_fragmenter_get_interface();
  vendor = vendor_get_interface();
  low_power_manager = low_power_manager_get_interface();

  init_layer_interface();
  return &interface;
}

const hci_t *hci_layer_get_test_interface(
    const allocator_t *buffer_allocator_interface,
    const hci_hal_t *hal_interface,
    const btsnoop_t *btsnoop_interface,
    const hci_inject_t *hci_inject_interface,
    const packet_fragmenter_t *packet_fragmenter_interface,
    const vendor_t *vendor_interface,
    const low_power_manager_t *low_power_manager_interface) {

  buffer_allocator = buffer_allocator_interface;
  hal = hal_interface;
  btsnoop = btsnoop_interface;
  hci_inject = hci_inject_interface;
  packet_fragmenter = packet_fragmenter_interface;
  vendor = vendor_interface;
  low_power_manager = low_power_manager_interface;

  init_layer_interface();
  return &interface;
}

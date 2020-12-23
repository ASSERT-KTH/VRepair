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

#include "base.h"
#include "support/adapter.h"
#include "support/callbacks.h"
#include "btcore/include/property.h"

bool adapter_enable_disable() {
  int error;

  CALL_AND_WAIT(error = bt_interface->enable(false), adapter_state_changed);
  TASSERT(error == BT_STATUS_SUCCESS, "Error enabling Bluetooth: %d", error);
  TASSERT(adapter_get_state() == BT_STATE_ON, "Adapter did not turn on.");

  CALL_AND_WAIT(error = bt_interface->disable(), adapter_state_changed);
  TASSERT(error == BT_STATUS_SUCCESS, "Error disabling Bluetooth: %d", error);
  TASSERT(adapter_get_state() == BT_STATE_OFF, "Adapter did not turn off.");

  return true;
}

bool adapter_repeated_enable_disable() {
  for (int i = 0; i < 10; ++i) {
    if (!adapter_enable_disable()) {
      return false;
    }
  }
  return true;
}

bool adapter_set_name() {
  int error;
  bt_property_t *name = property_new_name("set_name");

  CALL_AND_WAIT(error = bt_interface->set_adapter_property(name), adapter_properties);
  TASSERT(error == BT_STATUS_SUCCESS, "Error setting device name.");
  TASSERT(adapter_get_property_count() == 1, "Expected 1 adapter property change, found %d instead.", adapter_get_property_count());
  TASSERT(adapter_get_property(BT_PROPERTY_BDNAME), "The Bluetooth name property did not change.");
  TASSERT(property_equals(adapter_get_property(BT_PROPERTY_BDNAME), name), "Bluetooth name '%s' does not match test value", property_as_name(adapter_get_property(BT_PROPERTY_BDNAME))->name);

  property_free(name);

  return true;
}

bool adapter_get_name() {
  int error;
  bt_property_t *name = property_new_name("get_name");

  CALL_AND_WAIT(bt_interface->set_adapter_property(name), adapter_properties);
  CALL_AND_WAIT(error = bt_interface->get_adapter_property(BT_PROPERTY_BDNAME), adapter_properties);
  TASSERT(error == BT_STATUS_SUCCESS, "Error getting device name.");
  TASSERT(adapter_get_property_count() == 1, "Expected 1 adapter property change, found %d instead.", adapter_get_property_count());
  TASSERT(adapter_get_property(BT_PROPERTY_BDNAME), "The Bluetooth name property did not change.");
  TASSERT(property_equals(adapter_get_property(BT_PROPERTY_BDNAME), name), "Bluetooth name '%s' does not match test value", property_as_name(adapter_get_property(BT_PROPERTY_BDNAME))->name);

  property_free(name);
  return true;
}

bool adapter_start_discovery() {
  int error;

  CALL_AND_WAIT(error = bt_interface->start_discovery(), discovery_state_changed);
  TASSERT(error == BT_STATUS_SUCCESS, "Error calling start_discovery: %d", error);
  TASSERT(adapter_get_discovery_state() == BT_DISCOVERY_STARTED, "Unable to start discovery.");

  return true;
}

bool adapter_cancel_discovery() {
  int error;

  CALL_AND_WAIT(bt_interface->start_discovery(), discovery_state_changed);
  CALL_AND_WAIT(error = bt_interface->cancel_discovery(), discovery_state_changed);
  TASSERT(error == BT_STATUS_SUCCESS, "Error calling cancel_discovery: %d", error);
  TASSERT(adapter_get_discovery_state() == BT_DISCOVERY_STOPPED, "Unable to stop discovery.");

  return true;
}

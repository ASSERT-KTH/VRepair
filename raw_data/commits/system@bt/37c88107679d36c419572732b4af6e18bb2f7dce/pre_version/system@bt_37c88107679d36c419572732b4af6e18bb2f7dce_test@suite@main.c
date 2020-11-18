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

#include <cutils/properties.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "base.h"
#include "btcore/include/bdaddr.h"
#include "cases/cases.h"
#include "osi/include/config.h"
#include "support/callbacks.h"
#include "support/hal.h"
#include "support/gatt.h"
#include "support/pan.h"
#include "support/rfcomm.h"

// How long the watchdog thread should wait before checking if a test has completed.
// Any individual test will have at least WATCHDOG_PERIOD_SEC and at most
// 2 * WATCHDOG_PERIOD_SEC seconds to complete.
static const int WATCHDOG_PERIOD_SEC = 1 * 60;
static const char *CONFIG_FILE_PATH = "/data/misc/bluedroid/bt_config.conf";

const bt_interface_t *bt_interface;
bt_bdaddr_t bt_remote_bdaddr;

static pthread_t watchdog_thread;
static int watchdog_id;
static bool watchdog_running;

static void *watchdog_fn(void *arg) {
  int current_id = 0;
  for (;;) {
    // Check every second whether this thread should exit and check
    // every WATCHDOG_PERIOD_SEC whether we should terminate the process.
    for (int i = 0; watchdog_running && i < WATCHDOG_PERIOD_SEC; ++i) {
      sleep(1);
    }

    if (!watchdog_running)
      break;

    if (current_id == watchdog_id) {
      printf("Watchdog detected hanging test suite, aborting...\n");
      exit(-1);
    }
    current_id = watchdog_id;
  }
  return NULL;
}

static bool is_shell_running(void) {
  char property_str[100];
  property_get("init.svc.zygote", property_str, NULL);
  if (!strcmp("running", property_str)) {
    return true;
  }
  return false;
}

static void print_usage(const char *program_name) {
  printf("Usage: %s [options] [test name]\n", program_name);
  printf("\n");
  printf("Options:\n");
  printf("  %-20sdisplay this help text.\n", "--help");
  printf("  %-20sdo not run sanity suite.\n", "--insanity");
  printf("\n");
  printf("Valid test names are:\n");
  for (size_t i = 0; i < sanity_suite_size; ++i) {
    printf("  %s\n", sanity_suite[i].function_name);
  }
  for (size_t i = 0; i < test_suite_size; ++i) {
    printf("  %s\n", test_suite[i].function_name);
  }
}

static bool is_valid(const char *test_name) {
  for (size_t i = 0; i < sanity_suite_size; ++i) {
    if (!strcmp(test_name, sanity_suite[i].function_name)) {
      return true;
    }
  }
  for (size_t i = 0; i < test_suite_size; ++i) {
    if (!strcmp(test_name, test_suite[i].function_name)) {
      return true;
    }
  }
  return false;
}

int main(int argc, char **argv) {
  const char *test_name = NULL;
  bool skip_sanity_suite = false;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp("--help", argv[i])) {
      print_usage(argv[0]);
      return 0;
    }

    if (!strcmp("--insanity", argv[i])) {
      skip_sanity_suite = true;
      continue;
    }

    if (!is_valid(argv[i])) {
      printf("Error: invalid test name.\n");
      print_usage(argv[0]);
      return -1;
    }

    if (test_name != NULL) {
      printf("Error: invalid arguments.\n");
      print_usage(argv[0]);
      return -1;
    }

    test_name = argv[i];
  }

  if (is_shell_running()) {
    printf("Run 'adb shell stop' before running %s.\n", argv[0]);
    return -1;
  }

  config_t *config = config_new(CONFIG_FILE_PATH);
  if (!config) {
    printf("Error: unable to open stack config file.\n");
    print_usage(argv[0]);
    return -1;
  }

  for (const config_section_node_t *node = config_section_begin(config); node != config_section_end(config); node = config_section_next(node)) {
    const char *name = config_section_name(node);
    if (config_has_key(config, name, "LinkKey") && string_to_bdaddr(name, &bt_remote_bdaddr)) {
      break;
    }
  }

  config_free(config);

  if (bdaddr_is_empty(&bt_remote_bdaddr)) {
    printf("Error: unable to find paired device in config file.\n");
    print_usage(argv[0]);
    return -1;
  }

  if (!hal_open(callbacks_get_adapter_struct())) {
    printf("Unable to open Bluetooth HAL.\n");
    return 1;
  }

  if (!btsocket_init()) {
    printf("Unable to initialize Bluetooth sockets.\n");
    return 2;
  }

  if (!pan_init()) {
    printf("Unable to initialize PAN.\n");
    return 3;
  }

  if (!gatt_init()) {
    printf("Unable to initialize GATT.\n");
    return 4;
  }

  watchdog_running = true;
  pthread_create(&watchdog_thread, NULL, watchdog_fn, NULL);

  static const char *DEFAULT  = "\x1b[0m";
  static const char *GREEN = "\x1b[0;32m";
  static const char *RED   = "\x1b[0;31m";

  // If the output is not a TTY device, don't colorize output.
  if (!isatty(fileno(stdout))) {
    DEFAULT = GREEN = RED = "";
  }

  int pass = 0;
  int fail = 0;
  int case_num = 0;

  // If test name is specified, run that specific test.
  // Otherwise run through the sanity suite.
  if (!skip_sanity_suite) {
    for (size_t i = 0; i < sanity_suite_size; ++i) {
      if (!test_name || !strcmp(test_name, sanity_suite[i].function_name)) {
        callbacks_init();
        if (sanity_suite[i].function()) {
          printf("[%4d] %-64s [%sPASS%s]\n", ++case_num, sanity_suite[i].function_name, GREEN, DEFAULT);
          ++pass;
        } else {
          printf("[%4d] %-64s [%sFAIL%s]\n", ++case_num, sanity_suite[i].function_name, RED, DEFAULT);
          ++fail;
        }
        callbacks_cleanup();
        ++watchdog_id;
      }
    }
  }

  // If there was a failure in the sanity suite, don't bother running the rest of the tests.
  if (fail) {
    printf("\n%sSanity suite failed with %d errors.%s\n", RED, fail, DEFAULT);
    hal_close();
    return 4;
  }

  // If test name is specified, run that specific test.
  // Otherwise run through the full test suite.
  for (size_t i = 0; i < test_suite_size; ++i) {
    if (!test_name || !strcmp(test_name, test_suite[i].function_name)) {
      callbacks_init();
      CALL_AND_WAIT(bt_interface->enable(), adapter_state_changed);
      if (test_suite[i].function()) {
        printf("[%4d] %-64s [%sPASS%s]\n", ++case_num, test_suite[i].function_name, GREEN, DEFAULT);
        ++pass;
      } else {
        printf("[%4d] %-64s [%sFAIL%s]\n", ++case_num, test_suite[i].function_name, RED, DEFAULT);
        ++fail;
      }
      CALL_AND_WAIT(bt_interface->disable(), adapter_state_changed);
      callbacks_cleanup();
      ++watchdog_id;
    }
  }

  printf("\n");

  if (fail) {
    printf("%d/%d tests failed. See above for failed test cases.\n", fail, sanity_suite_size + test_suite_size);
  } else {
    printf("All tests passed!\n");
  }

  watchdog_running = false;
  pthread_join(watchdog_thread, NULL);

  hal_close();

  return 0;
}

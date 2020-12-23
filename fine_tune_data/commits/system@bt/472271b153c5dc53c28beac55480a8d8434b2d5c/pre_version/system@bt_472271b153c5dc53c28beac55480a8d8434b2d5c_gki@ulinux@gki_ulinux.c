/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
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

#define LOG_TAG "bt_gki"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "btcore/include/module.h"
#include "gki/ulinux/gki_int.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"

tGKI_CB gki_cb;

static future_t *init(void) {
  memset(&gki_cb, 0, sizeof(gki_cb));

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
  pthread_mutex_init(&gki_cb.lock, &attr);

  gki_buffer_init();
  return NULL;
}

static future_t *clean_up(void) {
  gki_buffer_cleanup();

  pthread_mutex_destroy(&gki_cb.lock);
  return NULL;
}

// Temp module until GKI dies
const module_t gki_module = {
  .name = GKI_MODULE,
  .init = init,
  .start_up = NULL,
  .shut_down = NULL,
  .clean_up = clean_up,
  .dependencies = {
    NULL
  }
};

UINT32 GKI_get_os_tick_count(void) {
  struct timespec timespec;
  clock_gettime(CLOCK_BOOTTIME, &timespec);
  return (timespec.tv_sec * 1000) + (timespec.tv_nsec / 1000000);
}

// Sleep the calling thread unconditionally for |timeout_ms| milliseconds.
void GKI_delay(UINT32 timeout_ms) {
  struct timespec delay;
  delay.tv_sec = timeout_ms / 1000;
  delay.tv_nsec = 1000 * 1000 * (timeout_ms % 1000);

  int err;
  do {
    err = nanosleep(&delay, &delay);
  } while (err == -1 && errno == EINTR);
}

void GKI_enable(void) {
  pthread_mutex_unlock(&gki_cb.lock);
}

void GKI_disable(void) {
  pthread_mutex_lock(&gki_cb.lock);
}

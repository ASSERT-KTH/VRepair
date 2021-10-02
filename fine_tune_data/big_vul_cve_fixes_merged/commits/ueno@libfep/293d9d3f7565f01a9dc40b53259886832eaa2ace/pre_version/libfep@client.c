/*
 * Copyright (C) 2012 Daiki Ueno <ueno@unixuser.org>
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libfep/private.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>		/* offsetof */

#ifndef _LIBC
char *program_name = "libfep";
#endif

/**
 * SECTION:client
 * @short_description: Client connection to FEP server
 */

struct _FepClient
{
  int control;
  FepEventFilter filter;
  void *filter_data;
  bool filter_running;
  FepList *messages;
};

static const FepAttribute empty_attr =
  {
    .type = FEP_ATTR_TYPE_NONE,
    .value = 0,
  };

/**
 * fep_client_open:
 * @address: (allow-none): socket address of the FEP server
 *
 * Connect to the FEP server running at @address.  If @address is
 * %NULL, it gets the address from the environment variable
 * `LIBFEP_CONTROL_SOCK`.
 *
 * Returns: a new #FepClient.
 */
FepClient *
fep_client_open (const char *address)
{
  FepClient *client;
  struct sockaddr_un sun;
  ssize_t sun_len;
  int retval;

  if (!address)
    address = getenv ("LIBFEP_CONTROL_SOCK");
  if (!address)
    return NULL;

  if (strlen (address) + 1 >= sizeof(sun.sun_path))
    {
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "unix domain socket path too long: %d + 1 >= %d",
	       strlen (address),
	       sizeof (sun.sun_path));
      free (address);
      return NULL;
    }

  client = xzalloc (sizeof(FepClient));
  client->filter_running = false;
  client->messages = NULL;

  memset (&sun, 0, sizeof(struct sockaddr_un));
  sun.sun_family = AF_UNIX;

#ifdef __linux__
  sun.sun_path[0] = '\0';
  memcpy (sun.sun_path + 1, address, strlen (address));
  sun_len = offsetof (struct sockaddr_un, sun_path) + strlen (address) + 1;
#else
  memcpy (sun.sun_path, address, strlen (address));
  sun_len = sizeof (struct sockaddr_un);
#endif

  client->control = socket (AF_UNIX, SOCK_STREAM, 0);
  if (client->control < 0)
    {
      free (client);
      return NULL;
    }

  retval = connect (client->control,
		    (const struct sockaddr *) &sun,
		    sun_len);
  if (retval < 0)
    {
      close (client->control);
      free (client);
      return NULL;
    }

  return client;
}

/**
 * fep_client_set_cursor_text:
 * @client: a #FepClient
 * @text: a cursor text
 * @attr: a #FepAttribute
 *
 * Request to display @text at the cursor position on the terminal.
 */
void
fep_client_set_cursor_text (FepClient    *client,
                            const char   *text,
                            FepAttribute *attr)
{
  FepControlMessage message;

  message.command = FEP_CONTROL_SET_CURSOR_TEXT;
  _fep_control_message_alloc_args (&message, 2);
  _fep_control_message_write_string_arg (&message, 0, text, strlen (text) + 1);
  _fep_control_message_write_attribute_arg (&message, 1, attr ? attr : &empty_attr);

  if (client->filter_running)
    client->messages = _fep_append_control_message (client->messages, &message);
  else
    _fep_write_control_message (client->control, &message);
  _fep_control_message_free_args (&message);
}

/**
 * fep_client_set_status_text:
 * @client: a #FepClient
 * @text: a status text
 * @attr: a #FepAttribute
 *
 * Request to display @text at the bottom of the terminal.
 */
void
fep_client_set_status_text (FepClient    *client,
                            const char   *text,
                            FepAttribute *attr)
{
  FepControlMessage message;

  message.command = FEP_CONTROL_SET_STATUS_TEXT;
  _fep_control_message_alloc_args (&message, 2);
  _fep_control_message_write_string_arg (&message, 0, text, strlen (text) + 1);
  _fep_control_message_write_attribute_arg (&message, 1, attr ? attr : &empty_attr);

  if (client->filter_running)
    client->messages = _fep_append_control_message (client->messages, &message);
  else
    _fep_write_control_message (client->control, &message);
  _fep_control_message_free_args (&message);
}

/**
 * fep_client_send_text:
 * @client: a #FepClient
 * @text: text to be sent
 *
 * Request to send @text to the child process of the FEP server.
 * @text will be converted from UTF-8 to the local encoding in the
 * server.
 */
void
fep_client_send_text (FepClient *client, const char *text)
{
  FepControlMessage message;

  message.command = FEP_CONTROL_SEND_TEXT;
  _fep_control_message_alloc_args (&message, 1);
  _fep_control_message_write_string_arg (&message, 0, text, strlen (text) + 1);

  if (client->filter_running)
    client->messages = _fep_append_control_message (client->messages, &message);
  else
    _fep_write_control_message (client->control, &message);
  _fep_control_message_free_args (&message);
}

/**
 * fep_client_send_data:
 * @client: a #FepClient
 * @data: data to be sent
 * @length: length of @data
 *
 * Request to send @data to the child process of the FEP server.
 */
void
fep_client_send_data (FepClient *client, const char *data, size_t length)
{
  FepControlMessage message;

  message.command = FEP_CONTROL_SEND_DATA;
  _fep_control_message_alloc_args (&message, 1);
  _fep_control_message_write_string_arg (&message, 0, data, length);

  if (client->filter_running)
    client->messages = _fep_append_control_message (client->messages, &message);
  else
    _fep_write_control_message (client->control, &message);
  _fep_control_message_free_args (&message);
}

/**
 * fep_client_forward_key_event:
 * @client: a #FepClient
 * @keyval: keysym value
 * @modifiers: modifiers
 *
 * Request to send key event to the child process of the FEP server.
 */
void
fep_client_forward_key_event (FepClient      *client,
                              unsigned int    keyval,
                              FepModifierType modifiers)
{
  FepControlMessage message;

  message.command = FEP_CONTROL_FORWARD_KEY_EVENT;
  _fep_control_message_alloc_args (&message, 2);
  _fep_control_message_write_uint32_arg (&message, 0, keyval);
  _fep_control_message_write_uint32_arg (&message, 1, modifiers);

  if (client->filter_running)
    client->messages = _fep_append_control_message (client->messages, &message);
  else
    _fep_write_control_message (client->control, &message);
  _fep_control_message_free_args (&message);
}

/**
 * fep_client_set_event_filter:
 * @client: a #FepClient
 * @filter: a filter function
 * @data: user supplied data
 *
 * Set a key event filter which will be called when client receives
 * key events.
 */
void
fep_client_set_event_filter (FepClient *client,
			     FepEventFilter filter,
			     void *data)
{
  client->filter = filter;
  client->filter_data = data;
}

/**
 * fep_client_get_poll_fd:
 * @client: a #FepClient
 *
 * Get the file descriptor of the control socket which can be used by poll().
 *
 * Returns: a file descriptor
 */
int
fep_client_get_poll_fd (FepClient *client)
{
  return client->control;
}

static void
command_key_event (FepClient *client,
		   FepControlMessage *request,
		   FepControlMessage *response)
{
  FepEventKey event;
  int retval;
  uint32_t intval;

  retval = _fep_control_message_read_uint32_arg (request, 0, &intval);
  if (retval < 0)
    {
      fep_log (FEP_LOG_LEVEL_WARNING, "can't read keyval");
      goto out;
    }
  event.keyval = intval;

  retval = _fep_control_message_read_uint32_arg (request, 1, &intval);
  if (retval < 0)
    {
      fep_log (FEP_LOG_LEVEL_WARNING, "can't read modifiers");
      goto out;
    }
  event.modifiers = intval;

 out:
  response->command = FEP_CONTROL_RESPONSE;
  _fep_control_message_alloc_args (response, 2);
  _fep_control_message_write_uint8_arg (response, 0, FEP_CONTROL_KEY_EVENT);

  intval = retval;
  if (retval == 0 && client->filter)
    {
      event.event.type = FEP_KEY_PRESS;
      event.source = request->args[2].str;
      event.source_length = request->args[2].len;
      intval = client->filter ((FepEvent *) &event, client->filter_data);
      _fep_control_message_write_uint32_arg (response, 1, intval);
    }

  /* If key is not handled, send back the original input to the server. */
  if (intval == 0)
    fep_client_send_data (client, request->args[2].str, request->args[2].len);
}

static void
command_resize_event (FepClient         *client,
                      FepControlMessage *request,
                      FepControlMessage *response)
{
  FepEventResize event;
  int retval;
  uint32_t intval;

  retval = _fep_control_message_read_uint32_arg (request, 0, &intval);
  if (retval < 0)
    {
      fep_log (FEP_LOG_LEVEL_WARNING, "can't read keyval");
      goto out;
    }
  event.cols = intval;

  retval = _fep_control_message_read_uint32_arg (request, 1, &intval);
  if (retval < 0)
    {
      fep_log (FEP_LOG_LEVEL_WARNING, "can't read modifiers");
      goto out;
    }
  event.rows = intval;

 out:
  response->command = FEP_CONTROL_RESPONSE;
  _fep_control_message_alloc_args (response, 2);
  _fep_control_message_write_uint8_arg (response, 0, FEP_CONTROL_RESIZE_EVENT);

  intval = retval;
  if (retval == 0 && client->filter)
    {
      event.event.type = FEP_RESIZED;
      intval = client->filter ((FepEvent *) &event, client->filter_data);
      _fep_control_message_write_uint32_arg (response, 1, intval);
    }
}

/**
 * fep_client_dispatch:
 * @client: a #FepClient
 *
 * Dispatch a request from server.
 *
 * Returns: 0 on success, -1 on failure.
 */
int
fep_client_dispatch (FepClient *client)
{
  static const struct
  {
    FepControlCommand command;
    void (*handler) (FepClient *client,
		     FepControlMessage *request,
		     FepControlMessage *response);
  } handlers[] =
      {
	{ FEP_CONTROL_KEY_EVENT, command_key_event },
	{ FEP_CONTROL_RESIZE_EVENT, command_resize_event },
      };
  FepControlMessage request, response;
  int retval;
  int i;

  retval = _fep_read_control_message (client->control, &request);
  if (retval < 0)
    return -1;

  for (i = 0;
       i < SIZEOF (handlers) && handlers[i].command != request.command;
       i++)
    ;
  if (i == SIZEOF (handlers))
    {
      _fep_control_message_free_args (&request);
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "no handler defined for %d", request.command);
      return -1;
    }

  client->filter_running = true;
  handlers[i].handler (client, &request, &response);
  _fep_control_message_free_args (&request);
  _fep_write_control_message (client->control, &response);
  _fep_control_message_free_args (&response);
  client->filter_running = false;

  /* flush queued messages during handler is executed */
  while (client->messages)
    {
      FepList *_head = client->messages;
      FepControlMessage *_message = _head->data;

      client->messages = _head->next;

      _fep_write_control_message (client->control, _message);
      _fep_control_message_free (_message);
      free (_head);
    }

  return retval;
}

/**
 * fep_client_close:
 * @client: a FepClient
 *
 * Close the control socket and release the memory allocated for @client.
 */
void
fep_client_close (FepClient *client)
{
  close (client->control);
  free (client);
}

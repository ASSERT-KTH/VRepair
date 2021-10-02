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

#include "private.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>		/* offsetof */
#include <assert.h>

static char *
create_socket_name (const char *template)
{
  char *name, *p;
  size_t len;

  /* Prepend the tmp directory to the template.  */
  p = getenv ("TMPDIR");
  if (!p || !*p)
    p = "/tmp";

  len = strlen (p) + strlen (template) + 2;
  name = xcharalloc (len);
  memset (name, 0, len);
  memcpy (name, p, strlen (p));
  if (p[strlen (p) - 1] != '/')
    name = strcat (name, "/");
  name = strcat (name, template);

  p = strrchr (name, '/');
  *p = '\0';
  if (!mkdtemp (name))
    {
      free (name);
      return NULL;
    }
  *p = '/';
  return name;
}

static void
remove_control_socket (const char *path)
{
  char *_path = xstrdup (path), *p;
  unlink (_path);
  p = strrchr (_path, '/');
  assert (p != NULL);
  *p = '\0';

  rmdir (_path);
  free (_path);
}

int
_fep_open_control_socket (Fep *fep)
{
  struct sockaddr_un sun;
  char *path;
  int fd;
  ssize_t sun_len;

  fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    {
      perror ("socket");
      return -1;
    }

  path = create_socket_name ("fep-XXXXXX/control");
  if (strlen (path) + 1 >= sizeof(sun.sun_path))
    {
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "unix domain socket path too long: %d + 1 >= %d",
	       strlen (path),
	       sizeof (sun.sun_path));
      free (path);
      return -1;
    }

  memset (&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;

#ifdef __linux__
  sun.sun_path[0] = '\0';
  memcpy (sun.sun_path + 1, path, strlen (path));
  sun_len = offsetof (struct sockaddr_un, sun_path) + strlen (path) + 1;
  remove_control_socket (path);
#else
  memcpy (sun.sun_path, path, strlen (path));
  sun_len = sizeof (struct sockaddr_un);
#endif

  if (bind (fd, (const struct sockaddr *) &sun, sun_len) < 0)
    {
      perror ("bind");
      free (path);
      close (fd);
      return -1;
    }

  if (listen (fd, 5) < 0)
    {
      perror ("listen");
      free (path);
      close (fd);
      return -1;
    }

  fep->server = fd;
  fep->control_socket_path = path;
  return 0;
}

void
_fep_close_control_socket (Fep *fep)
{
  if (fep->server >= 0)
    close (fep->server);
  remove_control_socket (fep->control_socket_path);
  free (fep->control_socket_path);
}

static void
command_set_cursor_text (Fep *fep,
			 FepControlMessage *request)
{
  FepAttribute attr;
  if (_fep_control_message_read_attribute_arg (request, 1, &attr) == 0)
    _fep_output_cursor_text (fep, request->args[0].str, &attr);
}

static void
command_set_status_text (Fep *fep,
			 FepControlMessage *request)
{
  FepAttribute attr;
  if (_fep_control_message_read_attribute_arg (request, 1, &attr) == 0)
    _fep_output_status_text (fep, request->args[0].str, &attr);
}

static void
command_send_text (Fep *fep,
		   FepControlMessage *request)
{
  _fep_output_send_text (fep, request->args[0].str);
}

static void
command_send_data (Fep *fep,
		   FepControlMessage *request)
{
  ssize_t total = 0;

  while (total < request->args[0].len)
    {
      ssize_t bytes_sent = _fep_output_send_data (fep,
						  request->args[0].str + total,
						  request->args[0].len - total);
      if (bytes_sent < 0)
	break;
      total += bytes_sent;
    }
}

static void
command_forward_key_event (Fep *fep,
			   FepControlMessage *request)
{
  uint32_t keyval, modifiers;
  if (_fep_control_message_read_uint32_arg (request, 0, &keyval) == 0
      && _fep_control_message_read_uint32_arg (request, 1, &modifiers) == 0)
    {
      size_t length;
      char *data = _fep_key_to_string (keyval, modifiers, &length);
      if (data)
	{
	  _fep_output_send_data (fep, data, length);
	  free (data);
	}
    }
}

int
_fep_read_control_message_from_fd (Fep               *fep,
                                   int                fd,
                                   FepControlMessage *message)
{
  int i;

  if (_fep_read_control_message (fd, message) < 0)
    {
      for (i = 0; i < fep->n_clients; i++)
	if (fep->clients[i] == fd)
	  {
	    close (fd);
	    if (i + 1 < fep->n_clients)
	      memmove (&fep->clients[i],
		       &fep->clients[i + 1],
		       fep->n_clients - (i + 1));
	    fep->clients[--fep->n_clients] = -1;
	    break;
	  }
      return -1;
    }

  return 0;
}


int
_fep_dispatch_control_message (Fep *fep, FepControlMessage *message)
{
  static const struct
  {
    int command;
    void (*handler) (Fep *fep,
		     FepControlMessage *request);
  } handlers[] =
      {
	{ FEP_CONTROL_SET_CURSOR_TEXT, command_set_cursor_text },
	{ FEP_CONTROL_SET_STATUS_TEXT, command_set_status_text },
	{ FEP_CONTROL_SEND_TEXT, command_send_text },
	{ FEP_CONTROL_SEND_DATA, command_send_data },
	{ FEP_CONTROL_FORWARD_KEY_EVENT, command_forward_key_event }
      };
  int i;

  for (i = 0;
       i < SIZEOF (handlers) && handlers[i].command != message->command;
       i++)
    ;
  if (i == SIZEOF (handlers))
    {
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "no handler defined for %d", message->command);
      return -1;
    }

  handlers[i].handler (fep, message);
  return 0;
}

int
_fep_transceive_control_message (Fep               *fep,
                                 int                fd,
                                 FepControlMessage *request,
                                 FepControlMessage *response)
{
  FepList *messages = NULL;
  int retval = 0;

  retval = _fep_write_control_message (fd, request);
  if (retval < 0)
    return retval;

  while (true)
    {
      FepControlMessage message;

      retval = _fep_read_control_message (fd, &message);
      if (retval < 0)
	goto out;

      if (message.command == FEP_CONTROL_RESPONSE)
	{
	  memcpy (response, &message, sizeof (FepControlMessage));
	  break;
	}

      fep_log (FEP_LOG_LEVEL_DEBUG,
	       "not a control response %d",
	       message.command);

      messages = _fep_append_control_message (messages, &message);
    }

  if (response->n_args == 0)
    {
      _fep_control_message_free_args (response);
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "too few arguments for RESPONSE");
      retval = -1;
      goto out;
    }

  if (response->args[0].len != 1)
    {
      _fep_control_message_free_args (response);
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "can't extract command from RESPONSE");
      retval = -1;
      goto out;
    }

  if (*response->args[0].str != request->command)
    {
      _fep_control_message_free_args (response);
      fep_log (FEP_LOG_LEVEL_WARNING,
	       "commands do not match (%d != %d)",
	       *response->args[0].str,
	       request->command);
      retval = -1;
      goto out;
    }

 out:
  /* flush queued messages received during waiting for response */
  while (messages)
    {
      FepList *_head = messages;
      FepControlMessage *_message = _head->data;

      messages = _head->next;

      _fep_dispatch_control_message (fep, _message);
      _fep_control_message_free (_message);
      free (_head);
    }
  return retval;
}

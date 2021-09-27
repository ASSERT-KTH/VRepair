/* Copyright (C) 2014 Daniel Dressler and contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include "options.h"
#include "logging.h"
#include "http.h"
#include "tcp.h"
#include "usb.h"

struct service_thread_param {
	struct tcp_conn_t *tcp;
	struct usb_sock_t *usb_sock;
	pthread_t thread_handle;
};
static void *service_connection(void *arg_void)
{
	struct service_thread_param *arg =
		(struct service_thread_param *)arg_void;

	// clasify priority
	while (!arg->tcp->is_closed) {
		struct usb_conn_t *usb = NULL;
		struct http_message_t *server_msg = NULL;
		struct http_message_t *client_msg = NULL;

		// Client's request
		client_msg = http_message_new();
		if (client_msg == NULL) {
			ERR("Failed to create client message");
			break;
		}
		NOTE("M %p: Client msg starting", client_msg);

		while (!client_msg->is_completed) {
			struct http_packet_t *pkt;
			pkt = tcp_packet_get(arg->tcp, client_msg);
			if (pkt == NULL) {
				if (arg->tcp->is_closed) {
					NOTE("M %p: Client closed connection\n", client_msg);
					goto cleanup_subconn;
				}
				ERR("M %p: Got null packet from tcp", client_msg);
				goto cleanup_subconn;
			}
			if (usb == NULL && arg->usb_sock != NULL) {
				usb = usb_conn_acquire(arg->usb_sock, 1);
				if (usb == NULL) {
					ERR("M %p: Failed to acquire usb interface", client_msg);
					packet_free(pkt);
					goto cleanup_subconn;
				}
				NOTE("M %p: Interface #%d: acquired usb conn",
				     client_msg,
				     usb->interface_index);
			}

			NOTE("M %p P %p: Pkt from tcp (buffer size: %d)\n===\n%.*s\n===", client_msg, pkt, pkt->filled_size, (int)pkt->filled_size, pkt->buffer);
			// In no-printer mode we simply ignore passing the
			// client message on to the printer
			if (arg->usb_sock != NULL) {
				usb_conn_packet_send(usb, pkt);
				NOTE("M %p P %p: Interface #%d: Client pkt done",
				     client_msg, pkt, usb->interface_index);
			}
			packet_free(pkt);
		}
		if (usb != NULL)
			NOTE("M %p: Interface #%d: Client msg completed\n", client_msg,
			     usb->interface_index);
		else
			NOTE("M %p: Client msg completed\n", client_msg);
		message_free(client_msg);
		client_msg = NULL;


		// Server's response
		server_msg = http_message_new();
		if (server_msg == NULL) {
			ERR("Failed to create server message");
			goto cleanup_subconn;
		}
		if (usb != NULL)
			NOTE("M %p: Interface #%d: Server msg starting", server_msg,
			     usb->interface_index);
		else
			NOTE("M %p: Server msg starting", server_msg);
		while (!server_msg->is_completed) {
			struct http_packet_t *pkt;
			if (arg->usb_sock != NULL) {
				pkt = usb_conn_packet_get(usb, server_msg);
				if (pkt == NULL)
					break;
			} else {
				// In no-printer mode we "invent" the answer
				// of the printer, a simple HTML message as
				// a pseudo web interface
				pkt = packet_new(server_msg);
				snprintf((char*)(pkt->buffer),
					 pkt->buffer_capacity - 1,
					 "HTTP/1.1 200 OK\r\nContent-Type: text/html; name=ippusbxd.html; charset=UTF-8\r\n\r\n<html><h2>ippusbxd</h2><p>Debug/development mode without connection to IPP-over-USB printer</p></html>\r\n");
				pkt->filled_size = 183;
				// End the TCP connection, so that a
				// web browser does not wait for more data
				server_msg->is_completed = 1;
				arg->tcp->is_closed = 1;
			}

			NOTE("M %p P %p: Pkt from usb (buffer size: %d)\n===\n%.*s\n===",
			     server_msg, pkt, pkt->filled_size,
			     (int)pkt->filled_size, pkt->buffer);
			tcp_packet_send(arg->tcp, pkt);
			if (usb != NULL)
				NOTE("M %p P %p: Interface #%d: Server pkt done",
				     server_msg, pkt, usb->interface_index);
			else
				NOTE("M %p P %p: Server pkt done",
				     server_msg, pkt);
			packet_free(pkt);
		}
		if (usb != NULL)
			NOTE("M %p: Interface #%d: Server msg completed\n", server_msg,
			     usb->interface_index);
		else
			NOTE("M %p: Server msg completed\n", server_msg);

cleanup_subconn:
		if (client_msg != NULL)
			message_free(client_msg);
		if (server_msg != NULL)
			message_free(server_msg);
		if (usb != NULL)
			usb_conn_release(usb);
	}



	tcp_conn_close(arg->tcp);
	free(arg);
	return NULL;
}

static void start_daemon()
{
	// Capture USB device if not in no-printer mode
	struct usb_sock_t *usb_sock;
	if (g_options.noprinter_mode == 0) {
		usb_sock = usb_open();
		if (usb_sock == NULL)
			goto cleanup_usb;
	} else
		usb_sock = NULL;

	// Capture a socket
	uint16_t desired_port = g_options.desired_port;
	struct tcp_sock_t *tcp_socket = NULL, *tcp6_socket = NULL;
	for (;;) {
		tcp_socket = tcp_open(desired_port);
		tcp6_socket = tcp6_open(desired_port);
		if (tcp_socket || tcp6_socket || g_options.only_desired_port)
			break;
		// Search for a free port
		desired_port ++;
		// We failed with 0 as port number or we reached the max
		// port number
		if (desired_port == 1 || desired_port == 0)
			// IANA recommendation of 49152 to 65535 for ephemeral
			// ports
			// https://en.wikipedia.org/wiki/Ephemeral_port
			desired_port = 49152;
		NOTE("Access to desired port failed, trying alternative port %d", desired_port);
	}
	if (tcp_socket == NULL && tcp6_socket == NULL)
		goto cleanup_tcp;

	uint16_t real_port;
	if (tcp_socket)
	  real_port = tcp_port_number_get(tcp_socket);
	else
	  real_port = tcp_port_number_get(tcp6_socket);
	if (desired_port != 0 && g_options.only_desired_port == 1 &&
	    desired_port != real_port) {
		ERR("Received port number did not match requested port number."
		    " The requested port number may be too high.");
		goto cleanup_tcp;
	}
	printf("%u|", real_port);
	fflush(stdout);

	NOTE("Port: %d, IPv4 %savailable, IPv6 %savailable",
	     real_port, tcp_socket ? "" : "not ", tcp6_socket ? "" : "not ");

	// Lose connection to caller
	uint16_t pid;
	if (!g_options.nofork_mode && (pid = fork()) > 0) {
		printf("%u|", pid);
		exit(0);
	}

	// Register for unplug event
	if (usb_can_callback(usb_sock))
		usb_register_callback(usb_sock);

	for (;;) {
		struct service_thread_param *args = calloc(1, sizeof(*args));
		if (args == NULL) {
			ERR("Failed to alloc space for thread args");
			goto cleanup_thread;
		}

		args->usb_sock = usb_sock;

		// For each request/response round we use the socket (IPv4 or
		// IPv6) which receives data first
		args->tcp = tcp_conn_select(tcp_socket, tcp6_socket);
		if (args->tcp == NULL) {
			ERR("Failed to open tcp connection");
			goto cleanup_thread;
		}

		int status = pthread_create(&args->thread_handle, NULL,
		                            &service_connection, args);
		if (status) {
			ERR("Failed to spawn thread, error %d", status);
			goto cleanup_thread;
		}

		continue;

	cleanup_thread:
		if (args != NULL) {
			if (args->tcp != NULL)
				tcp_conn_close(args->tcp);
			free(args);
		}
		break;
	}

cleanup_tcp:
	if (tcp_socket!= NULL)
		tcp_close(tcp_socket);
	if (tcp6_socket!= NULL)
		tcp_close(tcp6_socket);
cleanup_usb:
	if (usb_sock != NULL)
		usb_close(usb_sock);
	return;
}

static uint16_t strto16(const char *str)
{
	unsigned long val = strtoul(str, NULL, 16);
	if (val > UINT16_MAX)
		exit(1);
	return (uint16_t)val;
}

int main(int argc, char *argv[])
{
	int c;
	g_options.log_destination = LOGGING_STDERR;
	g_options.only_desired_port = 1;

	while ((c = getopt(argc, argv, "qnhdp:P:s:lv:m:N")) != -1) {
		switch (c) {
		case '?':
		case 'h':
			g_options.help_mode = 1;
			break;
		case 'p':
		case 'P':
		{
			long long port = 0;
			// Request specific port
			port = atoi(optarg);
			if (port < 0) {
				ERR("Port number must be non-negative");
				return 1;
			}
			if (port > UINT16_MAX) {
				ERR("Port number must be %u or less, "
				    "but not negative", UINT16_MAX);
				return 2;
			}
			g_options.desired_port = (uint16_t)port;
			if (c == 'p')
			  g_options.only_desired_port = 1;
			else
			  g_options.only_desired_port = 0;
			break;
		}
		case 'l':
			g_options.log_destination = LOGGING_SYSLOG;
			break;
		case 'd':
			g_options.nofork_mode = 1;
			g_options.verbose_mode = 1;
			break;
		case 'q':
			g_options.verbose_mode = 1;
			break;
		case 'n':
			g_options.nofork_mode = 1;
			break;
		case 'v':
			g_options.vendor_id = strto16(optarg);
			break;
		case 'm':
			g_options.product_id = strto16(optarg);
			break;
		case 's':
			g_options.serial_num = (unsigned char *)optarg;
			break;
		case 'N':
			g_options.noprinter_mode = 1;
			break;
		}
	}

	if (g_options.help_mode) {
		printf(
		"Usage: %s -v <vendorid> -m <productid> -p <port>\n"
		"Options:\n"
		"  -h           Show this help message\n"
		"  -v <vid>     Vendor ID of desired printer\n"
		"  -m <pid>     Product ID of desired printer\n"
		"  -s <serial>  Serial number of desired printer\n"
		"  -p <portnum> Port number to bind against, error out if port already taken\n"
		"  -P <portnum> Port number to bind against, use another port if port already\n"
		"               taken\n"
		"  -l           Redirect logging to syslog\n"
		"  -q           Enable verbose tracing\n"
		"  -d           Debug mode for verbose output and no fork\n"
		"  -n           No-fork mode\n"
		"  -N           No-printer mode, debug/developer mode which makes ippusbxd\n"
		"               run without IPP-over-USB printer\n"
		, argv[0]);
		return 0;
	}

	start_daemon();
	return 0;
}

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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "options.h"
#include "logging.h"
#include "tcp.h"


struct tcp_sock_t *tcp_open(uint16_t port)
{
	struct tcp_sock_t *this = calloc(1, sizeof *this);
	if (this == NULL) {
		ERR("callocing this failed");
		goto error;
	}

	// Open [S]ocket [D]escriptor
	this->sd = -1;
	this->sd = socket(AF_INET6, SOCK_STREAM, 0);
	if (this->sd < 0) {
		ERR("sockect open failed");
		goto error;
	}

	// Configure socket params
	struct sockaddr_in6 addr;
	memset(&addr, 0, sizeof addr);
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;

	// Bind to localhost
	if (bind(this->sd,
	        (struct sockaddr *)&addr,
	        sizeof addr) < 0) {
		if (g_options.only_desired_port == 1)
			ERR("Bind on port failed. "
			    "Requested port may be taken or require root permissions.");
		goto error;
	}

	// Let kernel over-accept max number of connections
	if (listen(this->sd, HTTP_MAX_PENDING_CONNS) < 0) {
		ERR("listen failed on socket");
		goto error;
	}

	return this;

error:
	if (this != NULL) {
		if (this->sd != -1) {
			close(this->sd);
		}
		free(this);
	}
	return NULL;
}

void tcp_close(struct tcp_sock_t *this)
{
	close(this->sd);
	free(this);
}

uint16_t tcp_port_number_get(struct tcp_sock_t *sock)
{
	sock->info_size = sizeof sock->info;
	int query_status = getsockname(
	                               sock->sd,
	                               (struct sockaddr *) &(sock->info),
	                               &(sock->info_size));
	if (query_status == -1) {
		ERR("query on socket port number failed");
		goto error;
	}

	return ntohs(sock->info.sin6_port);

error:
	return 0;
}

struct http_packet_t *tcp_packet_get(struct tcp_conn_t *tcp,
                                     struct http_message_t *msg)
{
	// Alloc packet ==---------------------------------------------------==
	struct http_packet_t *pkt = packet_new(msg);
	if (pkt == NULL) {
		ERR("failed to create packet for incoming tcp message");
		goto error;
	}

	size_t want_size = packet_pending_bytes(pkt);
	if (want_size == 0) {
		NOTE("TCP: Got %lu from spare buffer", pkt->filled_size);
		return pkt;
	}

	while (want_size != 0 && !msg->is_completed) {
		NOTE("TCP: Getting %d bytes", want_size);
		uint8_t *subbuffer = pkt->buffer + pkt->filled_size;
		ssize_t gotten_size = recv(tcp->sd, subbuffer, want_size, 0);
		if (gotten_size < 0) {
			int errno_saved = errno;
			ERR("recv failed with err %d:%s", errno_saved,
				strerror(errno_saved));
			goto error;
		}
		NOTE("TCP: Got %d bytes", gotten_size);
		if (gotten_size == 0) {
			tcp->is_closed = 1;
			if (pkt->filled_size == 0) {
				// Client closed TCP conn
				goto error;
			} else {
				break;
			}
		}

		packet_mark_received(pkt, (unsigned) gotten_size);
		want_size = packet_pending_bytes(pkt);
		NOTE("TCP: Want more %d bytes; Message %scompleted", want_size, msg->is_completed ? "" : "not ");
	}

	NOTE("TCP: Received %lu bytes", pkt->filled_size);
	return pkt;	
	 
error:
	if (pkt != NULL)
		packet_free(pkt);
	return NULL;
}

void tcp_packet_send(struct tcp_conn_t *conn, struct http_packet_t *pkt)
{
	size_t remaining = pkt->filled_size;
	size_t total = 0;
	while (remaining > 0) {
		ssize_t sent = send(conn->sd, pkt->buffer + total,
		                    remaining, MSG_NOSIGNAL);
		if (sent < 0) {
			if (errno == EPIPE) {
				conn->is_closed = 1;
				return;
			}
			ERR_AND_EXIT("Failed to sent data over TCP");
		}

		size_t sent_ulong = (unsigned) sent;
		total += sent_ulong;
		if (sent_ulong >= remaining)
			remaining = 0;
		else
			remaining -= sent_ulong;
	}
	NOTE("TCP: sent %lu bytes", total);
}


struct tcp_conn_t *tcp_conn_accept(struct tcp_sock_t *sock)
{
	struct tcp_conn_t *conn = calloc(1, sizeof *conn);
	if (conn == NULL) {
		ERR("Calloc for connection struct failed");
		goto error;
	}

	conn->sd = accept(sock->sd, NULL, NULL);
	if (conn->sd < 0) {
		ERR("accept failed");
		goto error;
	}

	return conn;

error:
	if (conn != NULL)
		free(conn);
	return NULL;
}

void tcp_conn_close(struct tcp_conn_t *conn)
{
	close(conn->sd);
	free(conn);
}


/*
 *   libndp.c - Neighbour discovery library
 *   Copyright (C) 2013-2015 Jiri Pirko <jiri@resnulli.us>
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <assert.h>
#include <ndp.h>

#include "ndp_private.h"
#include "list.h"

/**
 * SECTION: logging
 * @short_description: libndp logging facility
 */
void ndp_log(struct ndp *ndp, int priority,
	     const char *file, int line, const char *fn,
	     const char *format, ...)
{
	va_list args;

	va_start(args, format);
	ndp->log_fn(ndp, priority, file, line, fn, format, args);
	va_end(args);
}

static void log_stderr(struct ndp *ndp, int priority,
		       const char *file, int line, const char *fn,
		       const char *format, va_list args)
{
	fprintf(stderr, "libndp: %s: ", fn);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static int log_priority(const char *priority)
{
	char *endptr;
	int prio;

	prio = strtol(priority, &endptr, 10);
	if (endptr[0] == '\0' || isspace(endptr[0]))
		return prio;
	if (strncmp(priority, "err", 3) == 0)
		return LOG_ERR;
	if (strncmp(priority, "info", 4) == 0)
		return LOG_INFO;
	if (strncmp(priority, "debug", 5) == 0)
		return LOG_DEBUG;
	return 0;
}

/**
 * ndp_set_log_fn:
 * @ndp: libndp library context
 * @log_fn: function to be called for logging messages
 *
 * The built-in logging writes to stderr. It can be
 * overridden by a custom function, to plug log messages
 * into the user's logging functionality.
 **/
NDP_EXPORT
void ndp_set_log_fn(struct ndp *ndp,
		    void (*log_fn)(struct ndp *ndp, int priority,
				   const char *file, int line, const char *fn,
				   const char *format, va_list args))
{
	ndp->log_fn = log_fn;
	dbg(ndp, "Custom logging function %p registered.", log_fn);
}

/**
 * ndp_get_log_priority:
 * @ndp: libndp library context
 *
 * Returns: the current logging priority.
 **/
NDP_EXPORT
int ndp_get_log_priority(struct ndp *ndp)
{
	return ndp->log_priority;
}

/**
 * ndp_set_log_priority:
 * @ndp: libndp library context
 * @priority: the new logging priority
 *
 * Set the current logging priority. The value controls which messages
 * are logged.
 **/
NDP_EXPORT
void ndp_set_log_priority(struct ndp *ndp, int priority)
{
	ndp->log_priority = priority;
}


/**
 * SECTION: helpers
 * @short_description: various internal helper functions
 */

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BUG_ON(expr) { if (expr) assert(0); }

static void *myzalloc(size_t size)
{
	return calloc(1, size);
}

static int myrecvfrom6(int sockfd, void *buf, size_t *buflen, int flags,
		       struct in6_addr *addr, uint32_t *ifindex, int *hoplimit)
{
	struct sockaddr_in6 sin6;
	unsigned char cbuf[2 * CMSG_SPACE(sizeof(struct in6_pktinfo))];
	struct iovec iovec;
	struct msghdr msghdr;
	struct cmsghdr *cmsghdr;
	ssize_t len;

	iovec.iov_len = *buflen;
	iovec.iov_base = buf;
	memset(&msghdr, 0, sizeof(msghdr));
	msghdr.msg_name = &sin6;
	msghdr.msg_namelen = sizeof(sin6);
	msghdr.msg_iov = &iovec;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = cbuf;
	msghdr.msg_controllen = sizeof(cbuf);

	len = recvmsg(sockfd, &msghdr, flags);
	if (len == -1)
		return -errno;
	*buflen = len;

	/* Set ifindex to scope_id now. But since scope_id gets not
	 * set by kernel for linklocal addresses, use pktinfo to obtain that
	 * value right after.
	 */
	*ifindex = sin6.sin6_scope_id;
        for (cmsghdr = CMSG_FIRSTHDR(&msghdr); cmsghdr;
	     cmsghdr = CMSG_NXTHDR(&msghdr, cmsghdr)) {
		if (cmsghdr->cmsg_level != IPPROTO_IPV6)
			continue;

		switch(cmsghdr->cmsg_type) {
		case IPV6_PKTINFO:
			if (cmsghdr->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo))) {
				struct in6_pktinfo *pktinfo;

				pktinfo = (struct in6_pktinfo *) CMSG_DATA(cmsghdr);
				*ifindex = pktinfo->ipi6_ifindex;
			}
			break;
		case IPV6_HOPLIMIT:
			if (cmsghdr->cmsg_len == CMSG_LEN(sizeof(int))) {
				int *val;

				val = (int *) CMSG_DATA(cmsghdr);
				*hoplimit = *val;
			}
			break;
		}
	}
	*addr = sin6.sin6_addr;

	return 0;
}

static int mysendto6(int sockfd, void *buf, size_t buflen, int flags,
		     struct in6_addr *addr, uint32_t ifindex)
{
	struct sockaddr_in6 sin6;
	ssize_t ret;

	memset(&sin6, 0, sizeof(sin6));
	memcpy(&sin6.sin6_addr, addr, sizeof(sin6.sin6_addr));
	sin6.sin6_scope_id = ifindex;
resend:
	ret = sendto(sockfd, buf, buflen, flags, &sin6, sizeof(sin6));
	if (ret == -1) {
		switch(errno) {
		case EINTR:
			goto resend;
		default:
			return -errno;
		}
	}
	return 0;
}

static const char *str_in6_addr(struct in6_addr *addr)
{
	static char buf[INET6_ADDRSTRLEN];

	return inet_ntop(AF_INET6, addr, buf, sizeof(buf));
}


/**
 * SECTION: NDP implementation
 * @short_description: functions that actually implements NDP
 */

static int ndp_sock_open(struct ndp *ndp)
{
	int sock;
	//struct icmp6_filter flt;
	int ret;
	int err;
	int val;

	sock = socket(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (sock == -1) {
		err(ndp, "Failed to create ICMP6 socket.");
		return -errno;
	}

	val = 1;
	ret = setsockopt(sock, IPPROTO_IPV6, IPV6_RECVPKTINFO,
			 &val, sizeof(val));
	if (ret == -1) {
		err(ndp, "Failed to setsockopt IPV6_RECVPKTINFO.");
		err = -errno;
		goto close_sock;
	}

	val = 255;
	ret = setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
			 &val, sizeof(val));
	if (ret == -1) {
		err(ndp, "Failed to setsockopt IPV6_MULTICAST_HOPS.");
		err = -errno;
		goto close_sock;
	}

	val = 1;
	ret = setsockopt(sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT,
			 &val, sizeof(val));
	if (ret == -1) {
		err(ndp, "Failed to setsockopt IPV6_RECVHOPLIMIT,.");
		err = -errno;
		goto close_sock;
	}

	ndp->sock = sock;
	return 0;
close_sock:
	close(sock);
	return err;
}

static void ndp_sock_close(struct ndp *ndp)
{
	close(ndp->sock);
}

struct ndp_msggeneric {
	void *dataptr; /* must be first */
};

struct ndp_msgrs {
	struct nd_router_solicit *rs; /* must be first */
};

struct ndp_msgra {
	struct nd_router_advert *ra; /* must be first */
};

struct ndp_msgns {
	struct nd_neighbor_solicit *ns; /* must be first */
};

struct ndp_msgna {
	struct nd_neighbor_advert *na; /* must be first */
};

struct ndp_msgr {
	struct nd_redirect *r; /* must be first */
};

struct ndp_msg {
#define NDP_MSG_BUFLEN 1500
	unsigned char			buf[NDP_MSG_BUFLEN];
	size_t				len;
	struct in6_addr			addrto;
	uint32_t			ifindex;
	int				hoplimit;
	struct icmp6_hdr *		icmp6_hdr;
	unsigned char *			opts_start; /* pointer to buf at the
						       place where opts start */
	union {
		struct ndp_msggeneric	generic;
		struct ndp_msgrs	rs;
		struct ndp_msgra	ra;
		struct ndp_msgns	ns;
		struct ndp_msgna	na;
		struct ndp_msgr		r;
	} nd_msg;
};

struct ndp_msg_type_info {
#define NDP_STRABBR_SIZE 4
	char strabbr[NDP_STRABBR_SIZE];
	uint8_t raw_type;
	size_t raw_struct_size;
	void (*addrto_adjust)(struct in6_addr *addr);
};

static void ndp_msg_addrto_adjust_all_nodes(struct in6_addr *addr)
{
	struct in6_addr any = IN6ADDR_ANY_INIT;

	if (memcmp(addr, &any, sizeof(any)))
		return;
	addr->s6_addr32[0] = htonl(0xFF020000);
	addr->s6_addr32[1] = 0;
	addr->s6_addr32[2] = 0;
	addr->s6_addr32[3] = htonl(0x1);
}

static void ndp_msg_addrto_adjust_all_routers(struct in6_addr *addr)
{
	struct in6_addr any = IN6ADDR_ANY_INIT;

	if (memcmp(addr, &any, sizeof(any)))
		return;
	addr->s6_addr32[0] = htonl(0xFF020000);
	addr->s6_addr32[1] = 0;
	addr->s6_addr32[2] = 0;
	addr->s6_addr32[3] = htonl(0x2);
}

static struct ndp_msg_type_info ndp_msg_type_info_list[] =
{
	[NDP_MSG_RS] = {
		.strabbr = "RS",
		.raw_type = ND_ROUTER_SOLICIT,
		.raw_struct_size = sizeof(struct nd_router_solicit),
		.addrto_adjust = ndp_msg_addrto_adjust_all_routers,
	},
	[NDP_MSG_RA] = {
		.strabbr = "RA",
		.raw_type = ND_ROUTER_ADVERT,
		.raw_struct_size = sizeof(struct nd_router_advert),
	},
	[NDP_MSG_NS] = {
		.strabbr = "NS",
		.raw_type = ND_NEIGHBOR_SOLICIT,
		.raw_struct_size = sizeof(struct nd_neighbor_solicit),
		.addrto_adjust = ndp_msg_addrto_adjust_all_nodes,
	},
	[NDP_MSG_NA] = {
		.strabbr = "NA",
		.raw_type = ND_NEIGHBOR_ADVERT,
		.raw_struct_size = sizeof(struct nd_neighbor_advert),
	},
	[NDP_MSG_R] = {
		.strabbr = "R",
		.raw_type = ND_REDIRECT,
		.raw_struct_size = sizeof(struct nd_redirect),
	},
};

#define NDP_MSG_TYPE_LIST_SIZE ARRAY_SIZE(ndp_msg_type_info_list)

struct ndp_msg_type_info *ndp_msg_type_info(enum ndp_msg_type msg_type)
{
	return &ndp_msg_type_info_list[msg_type];
}

static int ndp_msg_type_by_raw_type(enum ndp_msg_type *p_msg_type,
				    uint8_t raw_type)
{
	int i;

	for (i = 0; i < NDP_MSG_TYPE_LIST_SIZE; i++) {
		if (ndp_msg_type_info(i)->raw_type == raw_type) {
			*p_msg_type = i;
			return 0;
		}
	}
	return -ENOENT;
}

static bool ndp_msg_check_valid(struct ndp_msg *msg)
{
	size_t len = ndp_msg_payload_len(msg);
	enum ndp_msg_type msg_type = ndp_msg_type(msg);

	if (len < ndp_msg_type_info(msg_type)->raw_struct_size)
		return false;
	return true;
}

static struct ndp_msg *ndp_msg_alloc(void)
{
	struct ndp_msg *msg;

	msg = myzalloc(sizeof(*msg));
	if (!msg)
		return NULL;
	msg->icmp6_hdr = (struct icmp6_hdr *) msg->buf;
	return msg;
}

static void ndp_msg_type_set(struct ndp_msg *msg, enum ndp_msg_type msg_type);

static void ndp_msg_init(struct ndp_msg *msg, enum ndp_msg_type msg_type)
{
	size_t raw_struct_size = ndp_msg_type_info(msg_type)->raw_struct_size;

	ndp_msg_type_set(msg, msg_type);
	msg->len = raw_struct_size;
	msg->opts_start = msg->buf + raw_struct_size;

	/* Set-up "first pointers" in all ndp_msgrs, ndp_msgra, ndp_msgns,
	 * ndp_msgna, ndp_msgr structures.
	 */
	msg->nd_msg.generic.dataptr = ndp_msg_payload(msg);
}

/**
 * ndp_msg_new:
 * @p_msg: pointer where new message structure address will be stored
 * @msg_type: message type
 *
 * Allocate new message structure of a specified type and initialize it.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_msg_new(struct ndp_msg **p_msg, enum ndp_msg_type msg_type)
{
	struct ndp_msg *msg;

	if (msg_type == NDP_MSG_ALL)
		return -EINVAL;
	msg = ndp_msg_alloc();
	if (!msg)
		return -ENOMEM;
	ndp_msg_init(msg, msg_type);
	*p_msg = msg;
	return 0;
}

/**
 * ndp_msg_destroy:
 *
 * Destroy message structure.
 **/
NDP_EXPORT
void ndp_msg_destroy(struct ndp_msg *msg)
{
	free(msg);
}

/**
 * ndp_msg_payload:
 * @msg: message structure
 *
 * Get raw Neighbour discovery packet data.
 *
 * Returns: pointer to raw data.
 **/
NDP_EXPORT
void *ndp_msg_payload(struct ndp_msg *msg)
{
	return msg->buf;
}

/**
 * ndp_msg_payload_maxlen:
 * @msg: message structure
 *
 * Get raw Neighbour discovery packet data maximum length.
 *
 * Returns: length in bytes.
 **/
NDP_EXPORT
size_t ndp_msg_payload_maxlen(struct ndp_msg *msg)
{
	return sizeof(msg->buf);
}

/**
 * ndp_msg_payload_len:
 * @msg: message structure
 *
 * Get raw Neighbour discovery packet data length.
 *
 * Returns: length in bytes.
 **/
NDP_EXPORT
size_t ndp_msg_payload_len(struct ndp_msg *msg)
{
	return msg->len;
}

/**
 * ndp_msg_payload_len_set:
 * @msg: message structure
 *
 * Set raw Neighbour discovery packet data length.
 **/
NDP_EXPORT
void ndp_msg_payload_len_set(struct ndp_msg *msg, size_t len)
{
	if (len > sizeof(msg->buf))
		len = sizeof(msg->buf);
	msg->len = len;
}

/**
 * ndp_msg_payload_opts:
 * @msg: message structure
 *
 * Get raw Neighbour discovery packet options part data.
 *
 * Returns: pointer to raw data.
 **/
NDP_EXPORT
void *ndp_msg_payload_opts(struct ndp_msg *msg)
{
	return msg->opts_start;
}

static void *ndp_msg_payload_opts_offset(struct ndp_msg *msg, int offset)
{
	unsigned char *ptr = ndp_msg_payload_opts(msg);

	return ptr + offset;
}

/**
 * ndp_msg_payload_opts_len:
 * @msg: message structure
 *
 * Get raw Neighbour discovery packet options part data length.
 *
 * Returns: length in bytes.
 **/
NDP_EXPORT
size_t ndp_msg_payload_opts_len(struct ndp_msg *msg)
{
	return msg->len - (msg->opts_start - msg->buf);
}

/**
 * ndp_msgrs:
 * @msg: message structure
 *
 * Get RS message structure by passed @msg.
 *
 * Returns: RS message structure or NULL in case the message is not of type RS.
 **/
NDP_EXPORT
struct ndp_msgrs *ndp_msgrs(struct ndp_msg *msg)
{
	if (ndp_msg_type(msg) != NDP_MSG_RS)
		return NULL;
	return &msg->nd_msg.rs;
}

/**
 * ndp_msgra:
 * @msg: message structure
 *
 * Get RA message structure by passed @msg.
 *
 * Returns: RA message structure or NULL in case the message is not of type RA.
 **/
NDP_EXPORT
struct ndp_msgra *ndp_msgra(struct ndp_msg *msg)
{
	if (ndp_msg_type(msg) != NDP_MSG_RA)
		return NULL;
	return &msg->nd_msg.ra;
}

/**
 * ndp_msgns:
 * @msg: message structure
 *
 * Get NS message structure by passed @msg.
 *
 * Returns: NS message structure or NULL in case the message is not of type NS.
 **/
NDP_EXPORT
struct ndp_msgns *ndp_msgns(struct ndp_msg *msg)
{
	if (ndp_msg_type(msg) != NDP_MSG_NS)
		return NULL;
	return &msg->nd_msg.ns;
}

/**
 * ndp_msgna:
 * @msg: message structure
 *
 * Get NA message structure by passed @msg.
 *
 * Returns: NA message structure or NULL in case the message is not of type NA.
 **/
NDP_EXPORT
struct ndp_msgna *ndp_msgna(struct ndp_msg *msg)
{
	if (ndp_msg_type(msg) != NDP_MSG_NA)
		return NULL;
	return &msg->nd_msg.na;
}

/**
 * ndp_msgr:
 * @msg: message structure
 *
 * Get R message structure by passed @msg.
 *
 * Returns: R message structure or NULL in case the message is not of type R.
 **/
NDP_EXPORT
struct ndp_msgr *ndp_msgr(struct ndp_msg *msg)
{
	if (ndp_msg_type(msg) != NDP_MSG_R)
		return NULL;
	return &msg->nd_msg.r;
}

/**
 * ndp_msg_type:
 * @msg: message structure
 *
 * Get type of message.
 *
 * Returns: Message type
 **/
NDP_EXPORT
enum ndp_msg_type ndp_msg_type(struct ndp_msg *msg)
{
	enum ndp_msg_type msg_type;
	int err;

	err = ndp_msg_type_by_raw_type(&msg_type, msg->icmp6_hdr->icmp6_type);
	/* Type should be always set correctly (ensured by ndp_msg_init) */
	BUG_ON(err);
	return msg_type;
}

static void ndp_msg_type_set(struct ndp_msg *msg, enum ndp_msg_type msg_type)
{
	msg->icmp6_hdr->icmp6_type = ndp_msg_type_info(msg_type)->raw_type;
}

/**
 * ndp_msg_addrto:
 * @msg: message structure
 *
 * Get "to address" of message.
 *
 * Returns: pointer to address.
 **/
NDP_EXPORT
struct in6_addr *ndp_msg_addrto(struct ndp_msg *msg)
{
	return &msg->addrto;
}

/**
 * ndp_msg_ifindex:
 * @msg: message structure
 *
 * Get interface index of message.
 *
 * Returns: Interface index
 **/
NDP_EXPORT
uint32_t ndp_msg_ifindex(struct ndp_msg *msg)
{
	return msg->ifindex;
}

/**
 * ndp_msg_ifindex_set:
 * @msg: message structure
 *
 * Set raw interface index of message.
 **/
NDP_EXPORT
void ndp_msg_ifindex_set(struct ndp_msg *msg, uint32_t ifindex)
{
	msg->ifindex = ifindex;
}

/**
 * ndp_msg_send:
 * @ndp: libndp library context
 * @msg: message structure
 *
 * Send message.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_msg_send(struct ndp *ndp, struct ndp_msg *msg)
{
	return ndp_msg_send_with_flags(ndp, msg, ND_OPT_NORMAL);
}

/**
 * ndp_msg_send_with_flags:
 * @ndp: libndp library context
 * @msg: message structure
 * @flags: option flags within message type
 *
 * Send message.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_msg_send_with_flags(struct ndp *ndp, struct ndp_msg *msg, uint8_t flags)
{
	enum ndp_msg_type msg_type = ndp_msg_type(msg);

	if (ndp_msg_type_info(msg_type)->addrto_adjust)
		ndp_msg_type_info(msg_type)->addrto_adjust(&msg->addrto);

	switch (msg_type) {
		case NDP_MSG_NA:
			if (flags & ND_OPT_NA_UNSOL) {
				ndp_msgna_flag_override_set((struct ndp_msgna*)&msg->nd_msg, true);
				ndp_msgna_flag_solicited_set((struct ndp_msgna*)&msg->nd_msg, false);
				ndp_msg_addrto_adjust_all_nodes(&msg->addrto);
			} else {
				ndp_msgna_flag_solicited_set((struct ndp_msgna*)&msg->nd_msg, true);
			}
			break;
		default:
			break;
	}

	return mysendto6(ndp->sock, msg->buf, msg->len, 0,
			 &msg->addrto, msg->ifindex);
}


/**
 * SECTION: msgra getters/setters
 * @short_description: Getters and setters for RA message
 */

/**
 * ndp_msgra_curhoplimit:
 * @msgra: RA message structure
 *
 * Get RA curhoplimit.
 *
 * Returns: curhoplimit.
 **/
NDP_EXPORT
uint8_t ndp_msgra_curhoplimit(struct ndp_msgra *msgra)
{
	return msgra->ra->nd_ra_curhoplimit;
}

/**
 * ndp_msgra_curhoplimit_set:
 * @msgra: RA message structure
 *
 * Set RA curhoplimit.
 **/
NDP_EXPORT
void ndp_msgra_curhoplimit_set(struct ndp_msgra *msgra, uint8_t curhoplimit)
{
	msgra->ra->nd_ra_curhoplimit = curhoplimit;
}

/**
 * ndp_msgra_flag_managed:
 * @msgra: RA message structure
 *
 * Get RA managed flag.
 *
 * Returns: managed flag.
 **/
NDP_EXPORT
bool ndp_msgra_flag_managed(struct ndp_msgra *msgra)
{
	return msgra->ra->nd_ra_flags_reserved & ND_RA_FLAG_MANAGED;
}

/**
 * ndp_msgra_flag_managed_set:
 * @msgra: RA message structure
 *
 * Set RA managed flag.
 **/
NDP_EXPORT
void ndp_msgra_flag_managed_set(struct ndp_msgra *msgra, bool flag_managed)
{
	if (flag_managed)
		msgra->ra->nd_ra_flags_reserved |= ND_RA_FLAG_MANAGED;
	else
		msgra->ra->nd_ra_flags_reserved &= ~ND_RA_FLAG_MANAGED;
}

/**
 * ndp_msgra_flag_other:
 * @msgra: RA message structure
 *
 * Get RA other flag.
 *
 * Returns: other flag.
 **/
NDP_EXPORT
bool ndp_msgra_flag_other(struct ndp_msgra *msgra)
{
	return msgra->ra->nd_ra_flags_reserved & ND_RA_FLAG_OTHER;
}

/**
 * ndp_msgra_flag_other_set:
 * @msgra: RA message structure
 *
 * Set RA other flag.
 **/
NDP_EXPORT
void ndp_msgra_flag_other_set(struct ndp_msgra *msgra, bool flag_other)
{
	if (flag_other)
		msgra->ra->nd_ra_flags_reserved |= ND_RA_FLAG_OTHER;
	else
		msgra->ra->nd_ra_flags_reserved &= ~ND_RA_FLAG_OTHER;
}

/**
 * ndp_msgra_flag_home_agent:
 * @msgra: RA message structure
 *
 * Get RA home_agent flag.
 *
 * Returns: home_agent flag.
 **/
NDP_EXPORT
bool ndp_msgra_flag_home_agent(struct ndp_msgra *msgra)
{
	return msgra->ra->nd_ra_flags_reserved & ND_RA_FLAG_HOME_AGENT;
}

/**
 * ndp_msgra_flag_home_agent_set:
 * @msgra: RA message structure
 *
 * Set RA home_agent flag.
 **/
NDP_EXPORT
void ndp_msgra_flag_home_agent_set(struct ndp_msgra *msgra,
				   bool flag_home_agent)
{
	if (flag_home_agent)
		msgra->ra->nd_ra_flags_reserved |= ND_RA_FLAG_HOME_AGENT;
	else
		msgra->ra->nd_ra_flags_reserved &= ~ND_RA_FLAG_HOME_AGENT;
}

/**
 * ndp_msgra_route_preference:
 * @msgra: RA message structure
 *
 * Get route preference.
 *
 * Returns: route preference.
 **/
NDP_EXPORT
enum ndp_route_preference ndp_msgra_route_preference(struct ndp_msgra *msgra)
{
	uint8_t prf = (msgra->ra->nd_ra_flags_reserved >> 3) & 3;

	/* rfc4191 says:
	 * If the Router Lifetime is zero, the preference value MUST be set to
	 * (00) by the sender and MUST be ignored by the receiver.
	 * If the Reserved (10) value is received, the receiver MUST treat the
	 * value as if it were (00).
	 */
	if (prf == 2 || !ndp_msgra_router_lifetime(msgra))
		prf = 0;
	return prf;
}

/**
 * ndp_msgra_route_preference_set:
 * @msgra: RA message structure
 * @pref: preference
 *
 * Set route preference.
 **/
NDP_EXPORT
void ndp_msgra_route_preference_set(struct ndp_msgra *msgra,
				    enum ndp_route_preference pref)
{
	msgra->ra->nd_ra_flags_reserved &= ~(3 << 3);
	msgra->ra->nd_ra_flags_reserved |= (pref << 3);
}

/**
 * ndp_msgra_router_lifetime:
 * @msgra: RA message structure
 *
 * Get RA router lifetime.
 *
 * Returns: router lifetime in seconds.
 **/
NDP_EXPORT
uint16_t ndp_msgra_router_lifetime(struct ndp_msgra *msgra)
{
	return ntohs(msgra->ra->nd_ra_router_lifetime);
}

/**
 * ndp_msgra_router_lifetime_set:
 * @msgra: RA message structure
 *
 * Set RA router lifetime.
 **/
NDP_EXPORT
void ndp_msgra_router_lifetime_set(struct ndp_msgra *msgra,
				   uint16_t router_lifetime)
{
	msgra->ra->nd_ra_router_lifetime = htons(router_lifetime);
}

/**
 * ndp_msgra_reachable_time:
 * @msgra: RA message structure
 *
 * Get RA reachable time.
 *
 * Returns: reachable time in milliseconds.
 **/
NDP_EXPORT
uint32_t ndp_msgra_reachable_time(struct ndp_msgra *msgra)
{
	return ntohl(msgra->ra->nd_ra_reachable);
}

/**
 * ndp_msgra_reachable_time_set:
 * @msgra: RA message structure
 *
 * Set RA reachable time.
 **/
NDP_EXPORT
void ndp_msgra_reachable_time_set(struct ndp_msgra *msgra,
				  uint32_t reachable_time)
{
	msgra->ra->nd_ra_reachable = htonl(reachable_time);
}

/**
 * ndp_msgra_retransmit_time:
 * @msgra: RA message structure
 *
 * Get RA retransmit time.
 *
 * Returns: retransmit time in milliseconds.
 **/
NDP_EXPORT
uint32_t ndp_msgra_retransmit_time(struct ndp_msgra *msgra)
{
	return ntohl(msgra->ra->nd_ra_retransmit);
}

/**
 * ndp_msgra_retransmit_time_set:
 * @msgra: RA message structure
 *
 * Set RA retransmit time.
 **/
NDP_EXPORT
void ndp_msgra_retransmit_time_set(struct ndp_msgra *msgra,
				   uint32_t retransmit_time)
{
	msgra->ra->nd_ra_retransmit = htonl(retransmit_time);
}


/**
 * SECTION: msgna getters/setters
 * @short_description: Getters and setters for NA message
 */

/**
 * ndp_msgna_flag_router:
 * @msgna: NA message structure
 *
 * Get NA router flag.
 *
 * Returns: router flag.
 **/
NDP_EXPORT
bool ndp_msgna_flag_router(struct ndp_msgna *msgna)
{
	return msgna->na->nd_na_flags_reserved & ND_NA_FLAG_ROUTER;
}

/**
 * ndp_msgna_flag_router_set:
 * @msgna: NA message structure
 *
 * Set NA router flag.
 **/
NDP_EXPORT
void ndp_msgna_flag_router_set(struct ndp_msgna *msgna, bool flag_router)
{
	if (flag_router)
		msgna->na->nd_na_flags_reserved |= ND_NA_FLAG_ROUTER;
	else
		msgna->na->nd_na_flags_reserved &= ~ND_NA_FLAG_ROUTER;
}

/**
 * ndp_msgna_flag_solicited:
 * @msgna: NA message structure
 *
 * Get NA solicited flag.
 *
 * Returns: solicited flag.
 **/
NDP_EXPORT
bool ndp_msgna_flag_solicited(struct ndp_msgna *msgna)
{
	return msgna->na->nd_na_flags_reserved & ND_NA_FLAG_SOLICITED;
}

/**
 * ndp_msgna_flag_solicited_set:
 * @msgna: NA message structure
 *
 * Set NA managed flag.
 **/
NDP_EXPORT
void ndp_msgna_flag_solicited_set(struct ndp_msgna *msgna, bool flag_solicited)
{
	if (flag_solicited)
		msgna->na->nd_na_flags_reserved |= ND_NA_FLAG_SOLICITED;
	else
		msgna->na->nd_na_flags_reserved &= ~ND_NA_FLAG_SOLICITED;
}

/**
 * ndp_msgna_flag_override:
 * @msgna: NA message structure
 *
 * Get NA override flag.
 *
 * Returns: override flag.
 **/
NDP_EXPORT
bool ndp_msgna_flag_override(struct ndp_msgna *msgna)
{
	return msgna->na->nd_na_flags_reserved & ND_NA_FLAG_OVERRIDE;
}

/**
 * ndp_msgna_flag_override_set:
 * @msgra: NA message structure
 *
 * Set NA override flag.
 */

NDP_EXPORT
void ndp_msgna_flag_override_set(struct ndp_msgna *msgna, bool flag_override)
{
	if (flag_override)
		msgna->na->nd_na_flags_reserved |= ND_NA_FLAG_OVERRIDE;
	else
		msgna->na->nd_na_flags_reserved &= ~ND_NA_FLAG_OVERRIDE;
}


/**
 * SECTION: msg_opt infrastructure
 * @short_description: Infrastructure for options
 */

struct ndp_msg_opt_type_info {
	uint8_t raw_type;
	size_t raw_struct_size;
	bool (*check_valid)(void *opt_data);
};

static bool ndp_msg_opt_route_check_valid(void *opt_data)
{
	struct __nd_opt_route_info *ri = opt_data;

	/* rfc4191 says:
	 * If the Reserved (10) value is received, the Route Information Option
	 * MUST be ignored.
	 */
	if (((ri->nd_opt_ri_prf_reserved >> 3) & 3) == 2)
		return false;
	return true;
}

static struct ndp_msg_opt_type_info ndp_msg_opt_type_info_list[] =
{
	[NDP_MSG_OPT_SLLADDR] = {
		.raw_type = ND_OPT_SOURCE_LINKADDR,
	},
	[NDP_MSG_OPT_TLLADDR] = {
		.raw_type = ND_OPT_TARGET_LINKADDR,
	},
	[NDP_MSG_OPT_PREFIX] = {
		.raw_type = ND_OPT_PREFIX_INFORMATION,
		.raw_struct_size = sizeof(struct nd_opt_prefix_info),
	},
	[NDP_MSG_OPT_REDIR] = {
		.raw_type = ND_OPT_REDIRECTED_HEADER,
	},
	[NDP_MSG_OPT_MTU] = {
		.raw_type = ND_OPT_MTU,
		.raw_struct_size = sizeof(struct nd_opt_mtu),
	},
	[NDP_MSG_OPT_ROUTE] = {
		.raw_type = __ND_OPT_ROUTE_INFO,
		.raw_struct_size = sizeof(struct __nd_opt_route_info),
		.check_valid = ndp_msg_opt_route_check_valid,
	},
	[NDP_MSG_OPT_RDNSS] = {
		.raw_type = __ND_OPT_RDNSS,
		.raw_struct_size = sizeof(struct __nd_opt_rdnss),
	},
	[NDP_MSG_OPT_DNSSL] = {
		.raw_type = __ND_OPT_DNSSL,
		.raw_struct_size = sizeof(struct __nd_opt_dnssl),
	},
};

#define NDP_MSG_OPT_TYPE_LIST_SIZE ARRAY_SIZE(ndp_msg_opt_type_info_list)

struct ndp_msg_opt_type_info *ndp_msg_opt_type_info(enum ndp_msg_opt_type msg_opt_type)
{
	return &ndp_msg_opt_type_info_list[msg_opt_type];
}

struct ndp_msg_opt_type_info *ndp_msg_opt_type_info_by_raw_type(uint8_t raw_type)
{
	struct ndp_msg_opt_type_info *info;
	int i;

	for (i = 0; i < NDP_MSG_OPT_TYPE_LIST_SIZE; i++) {
		info = &ndp_msg_opt_type_info_list[i];
		if (info->raw_type == raw_type)
			return info;
	}
	return NULL;
}

/**
 * ndp_msg_next_opt_offset:
 * @msg: message structure
 * @offset: option payload offset
 * @opt_type: option type
 *
 * Find next offset of option of given type. If offset is -1, start from
 * beginning, otherwise start from the given offset.
 * This funstion is internally used by ndp_msg_opt_for_each_offset() macro.
 *
 * Returns: offset in opt payload of found opt of -1 in case it was not found.
 **/
NDP_EXPORT
int ndp_msg_next_opt_offset(struct ndp_msg *msg, int offset,
			    enum ndp_msg_opt_type opt_type)
{
	unsigned char *opts_start = ndp_msg_payload_opts(msg);
	unsigned char *ptr = opts_start;
	size_t len = ndp_msg_payload_opts_len(msg);
	uint8_t opt_raw_type = ndp_msg_opt_type_info(opt_type)->raw_type;
	bool ignore = true;

	if (offset == -1) {
		offset = 0;
		ignore = false;
	}

	ptr += offset;
	len -= offset;
	while (len > 0) {
		uint8_t cur_opt_raw_type = ptr[0];
		unsigned int cur_opt_len = ptr[1] << 3; /* convert to bytes */

		if (!cur_opt_len || len < cur_opt_len)
			break;
		if (cur_opt_raw_type == opt_raw_type && !ignore)
			return ptr - opts_start;
		ptr += cur_opt_len;
		len -= cur_opt_len;
		ignore = false;
	}
	return -1;
}

#define __INVALID_OPT_TYPE_MAGIC 0xff

/*
 * Check for validity of options and mark by magic opt type in case it is not
 * so ndp_msg_next_opt_offset() will ignore it.
 */
static bool ndp_msg_check_opts(struct ndp_msg *msg)
{
	unsigned char *ptr = ndp_msg_payload_opts(msg);
	size_t len = ndp_msg_payload_opts_len(msg);
	struct ndp_msg_opt_type_info *info;

	while (len > 0) {
		uint8_t cur_opt_raw_type = ptr[0];
		unsigned int cur_opt_len = ptr[1] << 3; /* convert to bytes */

		if (!cur_opt_len)
			return false;
		if (len < cur_opt_len)
			break;
		info = ndp_msg_opt_type_info_by_raw_type(cur_opt_raw_type);
		if (info) {
			if (cur_opt_len < info->raw_struct_size ||
			    (info->check_valid && !info->check_valid(ptr)))
				ptr[0] = __INVALID_OPT_TYPE_MAGIC;
		}
		ptr += cur_opt_len;
		len -= cur_opt_len;
	}

	return true;
}

/**
 * SECTION: msg_opt getters/setters
 * @short_description: Getters and setters for options
 */

/**
 * ndp_msg_opt_slladdr:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get source linkaddr.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: pointer to source linkaddr.
 **/
NDP_EXPORT
unsigned char *ndp_msg_opt_slladdr(struct ndp_msg *msg, int offset)
{
	unsigned char *opt_data = ndp_msg_payload_opts_offset(msg, offset);

	return &opt_data[2];
}

/**
 * ndp_msg_opt_slladdr_len:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get source linkaddr length.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: source linkaddr length.
 **/
NDP_EXPORT
size_t ndp_msg_opt_slladdr_len(struct ndp_msg *msg, int offset)
{
	return ETH_ALEN;
}

/**
 * ndp_msg_opt_tlladdr:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get target linkaddr.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: pointer to target linkaddr.
 **/
NDP_EXPORT
unsigned char *ndp_msg_opt_tlladdr(struct ndp_msg *msg, int offset)
{
	unsigned char *opt_data = ndp_msg_payload_opts_offset(msg, offset);

	return &opt_data[2];
}

/**
 * ndp_msg_opt_tlladdr_len:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get target linkaddr length.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: target linkaddr length.
 **/
NDP_EXPORT
size_t ndp_msg_opt_tlladdr_len(struct ndp_msg *msg, int offset)
{
	return ETH_ALEN;
}

/**
 * ndp_msg_opt_prefix:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get prefix addr.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: pointer to address.
 **/
NDP_EXPORT
struct in6_addr *ndp_msg_opt_prefix(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return &pi->nd_opt_pi_prefix;
}

/**
 * ndp_msg_opt_prefix_len:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get prefix length.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: length of prefix.
 **/
NDP_EXPORT
uint8_t ndp_msg_opt_prefix_len(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return pi->nd_opt_pi_prefix_len;
}

/**
 * ndp_msg_opt_prefix_valid_time:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get prefix valid time.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: valid time in seconds, (uint32_t) -1 means infinity.
 **/
NDP_EXPORT
uint32_t ndp_msg_opt_prefix_valid_time(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return ntohl(pi->nd_opt_pi_valid_time);
}

/**
 * ndp_msg_opt_prefix_preferred_time:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get prefix preferred time.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: preferred time in seconds, (uint32_t) -1 means infinity.
 **/
NDP_EXPORT
uint32_t ndp_msg_opt_prefix_preferred_time(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return ntohl(pi->nd_opt_pi_preferred_time);
}

/**
 * ndp_msg_opt_prefix_flag_on_link:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get on-link flag.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: on-link flag.
 **/
NDP_EXPORT
bool ndp_msg_opt_prefix_flag_on_link(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return pi->nd_opt_pi_flags_reserved & ND_OPT_PI_FLAG_ONLINK;
}

/**
 * ndp_msg_opt_prefix_flag_auto_addr_conf:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get autonomous address-configuration flag.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: autonomous address-configuration flag.
 **/
NDP_EXPORT
bool ndp_msg_opt_prefix_flag_auto_addr_conf(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return pi->nd_opt_pi_flags_reserved & ND_OPT_PI_FLAG_AUTO;
}

/**
 * ndp_msg_opt_prefix_flag_router_addr:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get router address flag.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: router address flag.
 **/
NDP_EXPORT
bool ndp_msg_opt_prefix_flag_router_addr(struct ndp_msg *msg, int offset)
{
	struct nd_opt_prefix_info *pi =
			ndp_msg_payload_opts_offset(msg, offset);

	return pi->nd_opt_pi_flags_reserved & ND_OPT_PI_FLAG_RADDR;
}

/**
 * ndp_msg_opt_mtu:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get MTU. User should check if mtu option is present before calling this.
 *
 * Returns: MTU.
 **/
NDP_EXPORT
uint32_t ndp_msg_opt_mtu(struct ndp_msg *msg, int offset)
{
	struct nd_opt_mtu *mtu = ndp_msg_payload_opts_offset(msg, offset);

	return ntohl(mtu->nd_opt_mtu_mtu);
}

/**
 * ndp_msg_opt_route_prefix:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get route prefix addr.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: address.
 **/
NDP_EXPORT
struct in6_addr *ndp_msg_opt_route_prefix(struct ndp_msg *msg, int offset)
{
	static struct in6_addr prefix;
	struct __nd_opt_route_info *ri =
			ndp_msg_payload_opts_offset(msg, offset);

	memset(&prefix, 0, sizeof(prefix));
	memcpy(&prefix, &ri->nd_opt_ri_prefix, (ri->nd_opt_ri_len - 1) << 3);
	return &prefix;
}

/**
 * ndp_msg_opt_route_prefix_len:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get route prefix length.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: length of route prefix.
 **/
NDP_EXPORT
uint8_t ndp_msg_opt_route_prefix_len(struct ndp_msg *msg, int offset)
{
	struct __nd_opt_route_info *ri =
			ndp_msg_payload_opts_offset(msg, offset);

	return ri->nd_opt_ri_prefix_len;
}

/**
 * ndp_msg_opt_route_lifetime:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get route lifetime.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: route lifetime in seconds, (uint32_t) -1 means infinity.
 **/
NDP_EXPORT
uint32_t ndp_msg_opt_route_lifetime(struct ndp_msg *msg, int offset)
{
	struct __nd_opt_route_info *ri =
			ndp_msg_payload_opts_offset(msg, offset);

	return ntohl(ri->nd_opt_ri_lifetime);
}

/**
 * ndp_msg_opt_route_preference:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get route preference.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: route preference.
 **/
NDP_EXPORT
enum ndp_route_preference
ndp_msg_opt_route_preference(struct ndp_msg *msg, int offset)
{
	struct __nd_opt_route_info *ri =
			ndp_msg_payload_opts_offset(msg, offset);

	return (ri->nd_opt_ri_prf_reserved >> 3) & 3;
}

/**
 * ndp_msg_opt_rdnss_lifetime:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get Recursive DNS Server lifetime.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: route lifetime in seconds, (uint32_t) -1 means infinity.
 **/
NDP_EXPORT
uint32_t ndp_msg_opt_rdnss_lifetime(struct ndp_msg *msg, int offset)
{
	struct __nd_opt_rdnss *rdnss =
			ndp_msg_payload_opts_offset(msg, offset);

	return ntohl(rdnss->nd_opt_rdnss_lifetime);
}

/**
 * ndp_msg_opt_rdnss_addr:
 * @msg: message structure
 * @offset: in-message offset
 * @addr_index: address index
 *
 * Get Recursive DNS Server address.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: address.
 **/
NDP_EXPORT
struct in6_addr *ndp_msg_opt_rdnss_addr(struct ndp_msg *msg, int offset,
					int addr_index)
{
	static struct in6_addr addr;
	struct __nd_opt_rdnss *rdnss =
			ndp_msg_payload_opts_offset(msg, offset);
	size_t len = rdnss->nd_opt_rdnss_len << 3; /* convert to bytes */

	len -= in_struct_offset(struct __nd_opt_rdnss, nd_opt_rdnss_addresses);
	if ((addr_index + 1) * sizeof(addr) > len)
		return NULL;
	memcpy(&addr, &rdnss->nd_opt_rdnss_addresses[addr_index * sizeof(addr)],
	       sizeof(addr));
	return &addr;
}

/**
 * ndp_msg_opt_dnssl_lifetime:
 * @msg: message structure
 * @offset: in-message offset
 *
 * Get DNS Search List lifetime.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: route lifetime in seconds, (uint32_t) -1 means infinity.
 **/
NDP_EXPORT
uint32_t ndp_msg_opt_dnssl_lifetime(struct ndp_msg *msg, int offset)
{
	struct __nd_opt_dnssl *dnssl =
			ndp_msg_payload_opts_offset(msg, offset);

	return ntohl(dnssl->nd_opt_dnssl_lifetime);
}

/**
 * ndp_msg_opt_dnssl_domain:
 * @msg: message structure
 * @offset: in-message offset
 * @domain_index: domain index
 *
 * Get DNS Search List domain.
 * User should use this function only inside ndp_msg_opt_for_each_offset()
 * macro loop.
 *
 * Returns: address.
 **/
NDP_EXPORT
char *ndp_msg_opt_dnssl_domain(struct ndp_msg *msg, int offset,
			       int domain_index)
{
	int i;
	static char buf[256];
	struct __nd_opt_dnssl *dnssl =
			ndp_msg_payload_opts_offset(msg, offset);
	size_t len = dnssl->nd_opt_dnssl_len << 3; /* convert to bytes */
	char *ptr;

	len -= in_struct_offset(struct __nd_opt_dnssl, nd_opt_dnssl_domains);
	ptr = dnssl->nd_opt_dnssl_domains;

	i = 0;
	while (len > 0) {
		size_t buf_len = 0;
		while (len > 0) {
			uint8_t dom_len = *ptr;

			ptr++;
			len--;
			if (!dom_len)
				break;

			if (dom_len > len)
				return NULL;

			if (buf_len + dom_len + 1 > sizeof(buf))
				return NULL;

			memcpy(buf + buf_len, ptr, dom_len);
			buf[buf_len + dom_len] = '.';
			ptr += dom_len;
			len -= dom_len;
			buf_len += dom_len + 1;
		}
		if (!buf_len)
			break;
		buf[buf_len - 1] = '\0'; /* overwrite final '.' */
		if (i++ == domain_index)
			return buf;
	}
	return NULL;
}

static int ndp_call_handlers(struct ndp *ndp, struct ndp_msg *msg);

static int ndp_sock_recv(struct ndp *ndp)
{
	struct ndp_msg *msg;
	enum ndp_msg_type msg_type;
	size_t len;
	int err;

	msg = ndp_msg_alloc();
	if (!msg)
		return -ENOMEM;

	len = ndp_msg_payload_maxlen(msg);
	err = myrecvfrom6(ndp->sock, msg->buf, &len, 0,
			  &msg->addrto, &msg->ifindex, &msg->hoplimit);
	if (err) {
		err(ndp, "Failed to receive message");
		goto free_msg;
	}
	dbg(ndp, "rcvd from: %s, ifindex: %u, hoplimit: %d",
		 str_in6_addr(&msg->addrto), msg->ifindex, msg->hoplimit);

	if (msg->hoplimit != 255) {
		warn(ndp, "ignoring packet with bad hop limit (%d)", msg->hoplimit);
		err = 0;
		goto free_msg;
	}

	if (len < sizeof(*msg->icmp6_hdr)) {
		warn(ndp, "rcvd icmp6 packet too short (%luB)", len);
		err = 0;
		goto free_msg;
	}
	err = ndp_msg_type_by_raw_type(&msg_type, msg->icmp6_hdr->icmp6_type);
	if (err) {
		err = 0;
		goto free_msg;
	}
	ndp_msg_init(msg, msg_type);
	ndp_msg_payload_len_set(msg, len);

	if (!ndp_msg_check_valid(msg)) {
		warn(ndp, "rcvd invalid ND message");
		err = 0;
		goto free_msg;
	}

	dbg(ndp, "rcvd %s, len: %zuB",
		 ndp_msg_type_info(msg_type)->strabbr, len);

	if (!ndp_msg_check_opts(msg)) {
		err = 0;
		goto free_msg;
	}

	err = ndp_call_handlers(ndp, msg);;

free_msg:
	ndp_msg_destroy(msg);
	return err;
}


/**
 * SECTION: msgrcv handler
 * @short_description: msgrcv handler and related stuff
 */

struct ndp_msgrcv_handler_item {
	struct list_item			list;
	ndp_msgrcv_handler_func_t		func;
	enum ndp_msg_type			msg_type;
	uint32_t				ifindex;
	void *					priv;
};

static struct ndp_msgrcv_handler_item *
ndp_find_msgrcv_handler_item(struct ndp *ndp,
			     ndp_msgrcv_handler_func_t func,
			     enum ndp_msg_type msg_type, uint32_t ifindex,
			     void *priv)
{
	struct ndp_msgrcv_handler_item *handler_item;

	list_for_each_node_entry(handler_item, &ndp->msgrcv_handler_list, list)
		if (handler_item->func == func &&
		    handler_item->msg_type == msg_type &&
		    handler_item->ifindex == ifindex &&
		    handler_item->priv == priv)
			return handler_item;
	return NULL;
}

static int ndp_call_handlers(struct ndp *ndp, struct ndp_msg *msg)
{
	struct ndp_msgrcv_handler_item *handler_item;
	int err;

	list_for_each_node_entry(handler_item,
				 &ndp->msgrcv_handler_list, list) {
		if (handler_item->msg_type != NDP_MSG_ALL &&
		    handler_item->msg_type != ndp_msg_type(msg))
			continue;
		if (handler_item->ifindex &&
		    handler_item->ifindex != msg->ifindex)
			continue;
		err = handler_item->func(ndp, msg, handler_item->priv);
		if (err)
			return err;
	}
	return 0;
}

/**
 * ndp_msgrcv_handler_register:
 * @ndp: libndp library context
 * @func: handler function for received messages
 * @msg_type: message type to match
 * @ifindex: interface index to match
 * @priv: func private data
 *
 * Registers custom @func handler which is going to be called when
 * specified @msg_type is received. If one wants the function to be
 * called for all message types, pass NDP_MSG_ALL,
 * Note that @ifindex can be set to filter only messages received on
 * specified interface. For @func to be called for messages received on
 * all interfaces, just set 0.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_msgrcv_handler_register(struct ndp *ndp, ndp_msgrcv_handler_func_t func,
				enum ndp_msg_type msg_type, uint32_t ifindex,
				void *priv)
{
	struct ndp_msgrcv_handler_item *handler_item;

	if (ndp_find_msgrcv_handler_item(ndp, func, msg_type,
					 ifindex, priv))
		return -EEXIST;
	if (!func)
		return -EINVAL;
	handler_item = malloc(sizeof(*handler_item));
	if (!handler_item)
		return -ENOMEM;
	handler_item->func = func;
	handler_item->msg_type = msg_type;
	handler_item->ifindex = ifindex;
	handler_item->priv = priv;
	list_add_tail(&ndp->msgrcv_handler_list, &handler_item->list);
	return 0;
}

/**
 * ndp_msgrcv_handler_unregister:
 * @ndp: libndp library context
 * @func: handler function for received messages
 * @msg_type: message type to match
 * @ifindex: interface index to match
 * @priv: func private data
 *
 * Unregisters custom @func handler.
 *
 **/
NDP_EXPORT
void ndp_msgrcv_handler_unregister(struct ndp *ndp, ndp_msgrcv_handler_func_t func,
				   enum ndp_msg_type msg_type, uint32_t ifindex,
				   void *priv)
{
	struct ndp_msgrcv_handler_item *handler_item;

	handler_item = ndp_find_msgrcv_handler_item(ndp, func, msg_type,
						    ifindex, priv);
	if (!handler_item)
		return;
	list_del(&handler_item->list);
	free(handler_item);
}


/**
 * SECTION: event fd
 * @short_description: event filedescriptor related stuff
 */

/**
 * ndp_get_eventfd:
 * @ndp: libndp library context
 *
 * Get eventfd filedesctiptor.
 *
 * Returns: fd.
 **/
NDP_EXPORT
int ndp_get_eventfd(struct ndp *ndp)
{
	return ndp->sock;
}

/**
 * ndp_call_eventfd_handler:
 * @ndp: libndp library context
 *
 * Call eventfd handler.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_call_eventfd_handler(struct ndp *ndp)
{
	return ndp_sock_recv(ndp);
}

/**
 * ndp_callall_eventfd_handler:
 * @ndp: libndp library context
 *
 * Call all pending events on eventfd handler.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_callall_eventfd_handler(struct ndp *ndp)
{
	fd_set rfds;
	int fdmax;
	struct timeval tv;
	int fd = ndp_get_eventfd(ndp);
	int ret;
	int err;

	memset(&tv, 0, sizeof(tv));
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	fdmax = fd + 1;
	while (true) {
		ret = select(fdmax, &rfds, NULL, NULL, &tv);
		if (ret == -1)
			return -errno;
		if (!FD_ISSET(fd, &rfds))
			return 0;
		err = ndp_call_eventfd_handler(ndp);
		if (err)
			return err;
	}
}

/**
 * SECTION: Exported context functions
 * @short_description: Core context functions exported to user
 */

/**
 * ndp_open:
 * @p_ndp: pointer where new libndp library context address will be stored
 *
 * Allocates and initializes library context, opens raw socket.
 *
 * Returns: zero on success or negative number in case of an error.
 **/
NDP_EXPORT
int ndp_open(struct ndp **p_ndp)
{
	struct ndp *ndp;
	const char *env;
	int err;

	ndp = myzalloc(sizeof(*ndp));
	if (!ndp)
		return -ENOMEM;
	ndp->log_fn = log_stderr;
	ndp->log_priority = LOG_ERR;
	/* environment overwrites config */
	env = getenv("NDP_LOG");
	if (env != NULL)
		ndp_set_log_priority(ndp, log_priority(env));

	dbg(ndp, "ndp context %p created.", ndp);
	dbg(ndp, "log_priority=%d", ndp->log_priority);

	list_init(&ndp->msgrcv_handler_list);
	err = ndp_sock_open(ndp);
	if (err)
		goto free_ndp;

	*p_ndp = ndp;
	return 0;
free_ndp:
	free(ndp);
	return err;
}

/**
 * ndp_close:
 * @ndp: libndp library context
 *
 * Do library context cleanup.
 **/
NDP_EXPORT
void ndp_close(struct ndp *ndp)
{
	ndp_sock_close(ndp);
	free(ndp);
}


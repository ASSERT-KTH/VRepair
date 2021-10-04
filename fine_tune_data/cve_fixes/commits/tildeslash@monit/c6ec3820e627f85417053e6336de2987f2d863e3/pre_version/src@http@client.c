/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "monit.h"
#include "net.h"
#include "socket.h"
#include "ProcessTree.h"
#include "device.h"
#include "Color.h"
#include "Box.h"

// libmonit
#include "exceptions/AssertException.h"
#include "exceptions/IOException.h"

/**
 *  The monit HTTP GUI client
 *
 *  @file
 */


/* ----------------------------------------------------------------- Private */


static void _argument(StringBuffer_T data, const char *name, const char *value) {
        char *_value = Util_urlEncode((char *)value);
        StringBuffer_append(data, "%s%s=%s", StringBuffer_length(data) ? "&" : "", name, _value);
        FREE(_value);
}


static char *_getBasicAuthHeader() {
        Auth_T c = Run.httpd.credentials;
        // Find the first cleartext credential for authorization
        while (c != NULL) {
                if (c->digesttype == Digest_Cleartext && ! c->is_readonly)
                        break;
                c = c->next;
        }
        if (c)
                return Util_getBasicAuthHeader(c->uname, c->passwd);
        return NULL;
}


static void _parseHttpResponse(Socket_T S) {
        char buf[1024];
        if (! Socket_readLine(S, buf, sizeof(buf)))
                THROW(IOException, "Error receiving data -- %s", STRERROR);
        Str_chomp(buf);
        int status;
        if (! sscanf(buf, "%*s %d", &status))
                THROW(IOException, "Cannot parse status in response: %s", buf);
        if (status >= 300) {
                int content_length = 0;
                // Read HTTP headers
                while (Socket_readLine(S, buf, sizeof(buf))) {
                        if (! strncmp(buf, "\r\n", sizeof(buf)))
                                break;
                        if (Str_startsWith(buf, "Content-Length") && ! sscanf(buf, "%*s%*[: ]%d", &content_length))
                                THROW(IOException, "Invalid Content-Length header: %s", buf);
                }
                // Parse error response
                char *message = NULL;
                if (content_length > 0 && content_length < 1024 && Socket_readLine(S, buf, sizeof(buf))) {
                        char token[] = "</h2>";
                        message = strstr(buf, token);
                        if (strlen(message) > strlen(token)) {
                                message += strlen(token);
                                char *footer = NULL;
                                if ((footer = strstr(message, "<p>")) || (footer = strstr(message, "<hr>")))
                                        *footer = 0;
                        }
                }
                THROW(AssertException, "%s", message ? message : "cannot parse response");
        } else {
                // Skip HTTP headers
                while (Socket_readLine(S, buf, sizeof(buf))) {
                         if (! strncmp(buf, "\r\n", sizeof(buf)))
                                break;
                }
        }
}


static void _send(Socket_T S, const char *request, StringBuffer_T data) {
        _argument(data, "format", "text");
        char *_auth = _getBasicAuthHeader();
        int rv = Socket_print(S,
                "POST %s HTTP/1.0\r\n"
                "Content-Type: application/x-www-form-urlencoded\r\n"
                "Content-Length: %d\r\n"
                 "%s"
                 "\r\n"
                 "%s",
                request,
                StringBuffer_length(data),
                _auth ? _auth : "",
                StringBuffer_toString(data));
        FREE(_auth);
        if (rv < 0)
                THROW(IOException, "Monit: cannot send command to the monit daemon -- %s", STRERROR);
}


static void _receive(Socket_T S) {
        char buf[1024];
        _parseHttpResponse(S);
        boolean_t strip = (Run.flags & Run_Batch || ! Color_support()) ? true : false;
        while (Socket_readLine(S, buf, sizeof(buf))) {
                if (strip)
                        Color_strip(Box_strip(buf));
                printf("%s", buf);
        }
}


static boolean_t _client(const char *request, StringBuffer_T data) {
        boolean_t status = false;
        if (! exist_daemon()) {
                LogError("Monit: the monit daemon is not running\n");
                return status;
        }
        Socket_T S = NULL;
        if (Run.httpd.flags & Httpd_Net) {
                SslOptions_T options = {
                        .flags = (Run.httpd.flags & Httpd_Ssl) ? SSL_Enabled : SSL_Disabled,
                        .clientpemfile = Run.httpd.socket.net.ssl.clientpem,
                        .allowSelfSigned = Run.httpd.flags & Httpd_AllowSelfSignedCertificates
                };
                S = Socket_create(Run.httpd.socket.net.address ? Run.httpd.socket.net.address : "localhost", Run.httpd.socket.net.port, Socket_Tcp, Socket_Ip, options, Run.limits.networkTimeout);
        } else if (Run.httpd.flags & Httpd_Unix) {
                S = Socket_createUnix(Run.httpd.socket.unix.path, Socket_Tcp, Run.limits.networkTimeout);
        } else {
                LogError("Monit: the monit HTTP interface is not enabled, please add the 'set httpd' statement and use the 'allow' option to allow monit to connect\n");
        }
        if (S) {
                TRY
                {
                        _send(S, request, data);
                        _receive(S);
                        status = true;
                }
                ELSE
                {
                        LogError("%s\n", Exception_frame.message);
                }
                END_TRY;
                Socket_free(&S);
        }
        return status;
}


/* ------------------------------------------------------------------ Public */


boolean_t HttpClient_action(const char *action, List_T services) {
        ASSERT(services);
        ASSERT(action);
        if (Util_getAction(action) == Action_Ignored) {
                LogError("Invalid action %s\n", action);
                return false;
        }
        StringBuffer_T data = StringBuffer_create(64);
        _argument(data, "action", action);
        for (list_t s = services->head; s; s = s->next)
                _argument(data, "service", s->e);
        boolean_t rv = _client("/_doaction", data);
        StringBuffer_free(&data);
        return rv;
}


boolean_t HttpClient_report(const char *type) {
        StringBuffer_T data = StringBuffer_create(64);
        if (STR_DEF(type))
                _argument(data, "type", type);
        boolean_t rv = _client("/_report", data);
        StringBuffer_free(&data);
        return rv;
}


boolean_t HttpClient_status(const char *group, const char *service) {
        StringBuffer_T data = StringBuffer_create(64);
        if (STR_DEF(service))
                _argument(data, "service", service);
        if (STR_DEF(group))
                _argument(data, "group", group);
        boolean_t rv = _client("/_status", data);
        StringBuffer_free(&data);
        return rv;
}


boolean_t HttpClient_summary(const char *group, const char *service) {
        StringBuffer_T data = StringBuffer_create(64);
        if (STR_DEF(service))
                _argument(data, "service", service);
        if (STR_DEF(group))
                _argument(data, "group", group);
        boolean_t rv = _client("/_summary", data);
        StringBuffer_free(&data);
        return rv;
}


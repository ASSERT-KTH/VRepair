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

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SETJMP_H
#include <setjmp.h>
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

#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif

#include "monit.h"
#include "processor.h"
#include "base64.h"

// libmonit
#include "util/Str.h"
#include "system/Net.h"


/**
 *  A naive quasi HTTP Processor module that can handle HTTP requests
 *  received from a client, and return responses based on those
 *  requests.
 *
 *  This Processor delegates the actual handling of the request and
 *  reponse to so called cervlets, which must implement two methods;
 *  doGet and doPost.
 *
 *  NOTES
 *    This Processor is command oriented and if a second slash '/' is
 *    found in the URL it's asumed to be the PATHINFO. In other words
 *    this processor perceive an URL as:
 *
 *                      /COMMAND?QUERYSTRING/PATHINFO
 *
 *     The doGet/doPost routines act's on the COMMAND. See the
 *     cervlet.c code in this dir. for an example.
 *
 *  @file
 */


static int _httpPostLimit;


/* -------------------------------------------------------------- Prototypes */


static void do_service(Socket_T);
static void destroy_entry(void *);
static char *get_date(char *, int);
static char *get_server(char *, int);
static void create_headers(HttpRequest);
static void send_response(HttpRequest, HttpResponse);
static boolean_t basic_authenticate(HttpRequest);
static void done(HttpRequest, HttpResponse);
static void destroy_HttpRequest(HttpRequest);
static void reset_response(HttpResponse res);
static HttpParameter parse_parameters(char *);
static boolean_t create_parameters(HttpRequest req);
static void destroy_HttpResponse(HttpResponse);
static HttpRequest create_HttpRequest(Socket_T);
static void internal_error(Socket_T, int, char *);
static HttpResponse create_HttpResponse(Socket_T);
static boolean_t is_authenticated(HttpRequest, HttpResponse);
static int get_next_token(char *s, int *cursor, char **r);


/*
 * An object for implementors of the service functions; doGet and
 * doPost. Implementing modules i.e. CERVLETS, must implement the
 * doGet and doPost functions and the engine will call the add_Impl
 * function to setup the callback to these functions.
 */
struct  ServiceImpl {
        void(*doGet)(HttpRequest, HttpResponse);
        void(*doPost)(HttpRequest, HttpResponse);
} Impl;


/* ------------------------------------------------------------------ Public */


/**
 * Process a HTTP request. This is done by dispatching to the service
 * function.
 * @param s A Socket_T representing the client connection
 */
void *http_processor(Socket_T s) {
        if (! Net_canRead(Socket_getSocket(s), REQUEST_TIMEOUT * 1000))
                internal_error(s, SC_REQUEST_TIMEOUT, "Time out when handling the Request");
        else
                do_service(s);
        Socket_free(&s);
        return NULL;
}


/**
 * Callback for implementors of cervlet functions.
 * @param doGetFunc doGet function
 * @param doPostFunc doPost function
 */
void add_Impl(void(*doGet)(HttpRequest, HttpResponse), void(*doPost)(HttpRequest, HttpResponse)) {
        Impl.doGet = doGet;
        Impl.doPost = doPost;
}


void Processor_setHttpPostLimit() {
        // Base buffer size (space for e.g. "action=<name>")
        _httpPostLimit = STRLEN;
        // Add space for each service
        for (Service_T s = servicelist; s; s = s->next)
                _httpPostLimit += strlen("&service=") + strlen(s->name);
}


void escapeHTML(StringBuffer_T sb, const char *s) {
        for (int i = 0; s[i]; i++) {
                if (s[i] == '<')
                        StringBuffer_append(sb, "&lt;");
                else if (s[i] == '>')
                        StringBuffer_append(sb, "&gt;");
                else if (s[i] == '&')
                        StringBuffer_append(sb, "&amp;");
                else
                        StringBuffer_append(sb, "%c", s[i]);
        }
}


/**
 * Send an error message
 * @param res HttpResponse object
 * @param code Error Code to lookup and send
 * @param msg Optional error message (may be NULL)
 */
void send_error(HttpRequest req, HttpResponse res, int code, const char *msg, ...) {
        ASSERT(msg);

        const char *err = get_status_string(code);
        reset_response(res);
        set_content_type(res, "text/html");
        set_status(res, code);
        StringBuffer_append(res->outputbuffer,
                            "<html>"
                            "<head>"
                            "<title>%d %s</title>"
                            "</head>"
                            "<body bgcolor=#FFFFFF>"
                            "<h2>%s</h2>",
                            code, err, err);
        char *message;
        va_list ap;
        va_start(ap, msg);
        message = Str_vcat(msg, ap);
        va_end(ap);
        escapeHTML(res->outputbuffer, message);
        if (code != SC_UNAUTHORIZED) // We log details in basic_authenticate() already, no need to log generic error sent to client here
                LogError("HttpRequest: error -- client [%s]: %s %d %s\n", NVLSTR(Socket_getRemoteHost(req->S)), SERVER_PROTOCOL, code, message);
        FREE(message);
        char server[STRLEN];
        StringBuffer_append(res->outputbuffer,
                            "<hr>"
                            "<a href='%s'><font size=-1>%s</font></a>"
                            "</body>"
                            "</html>"
                            "\r\n",
                            SERVER_URL, get_server(server, STRLEN));
}


/* -------------------------------------------------------------- Properties */


/**
 * Adds a response header with the given name and value. If the header
 * had already been set the new value overwrites the previous one.
 * @param res HttpResponse object
 * @param name Header key name
 * @param value Header key value
 */
void set_header(HttpResponse res, const char *name, const char *value, ...) {
        HttpHeader h = NULL;

        ASSERT(res);
        ASSERT(name);

        NEW(h);
        h->name = Str_dup(name);
        va_list ap;
        va_start(ap, value);
        h->value = Str_vcat(value, ap);
        va_end(ap);
        if (res->headers) {
                HttpHeader n, p;
                for (n = p = res->headers; p; n = p, p = p->next) {
                        if (IS(p->name, name)) {
                                FREE(p->value);
                                p->value = Str_dup(value);
                                destroy_entry(h);
                                return;
                        }
                }
                n->next = h;
        } else {
                res->headers = h;
        }
}


/**
 * Sets the status code for the response
 * @param res HttpResponse object
 * @param code A HTTP status code <100-510>
 * @param msg The status code string message
 */
void set_status(HttpResponse res, int code) {
        res->status = code;
        res->status_msg = get_status_string(code);
}


/**
 * Set the response content-type
 * @param res HttpResponse object
 * @param mime Mime content type, e.g. text/html
 */
void set_content_type(HttpResponse res, const char *mime) {
        set_header(res, "Content-Type", "%s", mime);
}


/**
 * Returns the value of the specified header
 * @param req HttpRequest object
 * @param name Header name to lookup the value for
 * @return The value of the specified header, NULL if not found
 */
const char *get_header(HttpRequest req, const char *name) {
        for (HttpHeader p = req->headers; p; p = p->next)
                if (IS(p->name, name))
                        return (p->value);
        return NULL;
}


/**
 * Returns the value of the specified parameter
 * @param req HttpRequest object
 * @param name The request parameter key to lookup the value for
 * @return The value of the specified parameter, or NULL if not found
 */
const char *get_parameter(HttpRequest req, const char *name) {
        for (HttpParameter p = req->params; p; p = p->next)
                if (IS(p->name, name))
                        return (p->value);
        return NULL;
}


/**
 * Returns a string containing all (extra) headers found in the
 * response.  The headers are newline separated in the returned
 * string.
 * @param res HttpResponse object
 * @return A String containing all headers set in the Response object
 */
char *get_headers(HttpResponse res) {
        char buf[RES_STRLEN];
        char *b = buf;
        *buf = 0;
        for (HttpHeader p = res->headers; (((b - buf) + STRLEN) < RES_STRLEN) && p; p = p->next)
                b += snprintf(b, STRLEN,"%s: %s\r\n", p->name, p->value);
        return buf[0] ? Str_dup(buf) : NULL;
}


/**
 * Lookup the corresponding HTTP status string for the given status
 * code
 * @param status A HTTP status code
 * @return A default status message for the specified HTTP status
 * code.
 */
const char *get_status_string(int status) {
        switch (status) {
                case SC_OK:
                        return "OK";
                case SC_ACCEPTED:
                        return "Accepted";
                case SC_BAD_GATEWAY:
                        return "Bad Gateway";
                case SC_BAD_REQUEST:
                        return "Bad Request";
                case SC_CONFLICT:
                        return "Conflict";
                case SC_CONTINUE:
                        return "Continue";
                case SC_CREATED:
                        return "Created";
                case SC_EXPECTATION_FAILED:
                        return "Expectation Failed";
                case SC_FORBIDDEN:
                        return "Forbidden";
                case SC_GATEWAY_TIMEOUT:
                        return "Gateway Timeout";
                case SC_GONE:
                        return "Gone";
                case SC_VERSION_NOT_SUPPORTED:
                        return "HTTP Version Not Supported";
                case SC_INTERNAL_SERVER_ERROR:
                        return "Internal Server Error";
                case SC_LENGTH_REQUIRED:
                        return "Length Required";
                case SC_METHOD_NOT_ALLOWED:
                        return "Method Not Allowed";
                case SC_MOVED_PERMANENTLY:
                        return "Moved Permanently";
                case SC_MOVED_TEMPORARILY:
                        return "Moved Temporarily";
                case SC_MULTIPLE_CHOICES:
                        return "Multiple Choices";
                case SC_NO_CONTENT:
                        return "No Content";
                case SC_NON_AUTHORITATIVE:
                        return "Non-Authoritative Information";
                case SC_NOT_ACCEPTABLE:
                        return "Not Acceptable";
                case SC_NOT_FOUND:
                        return "Not Found";
                case SC_NOT_IMPLEMENTED:
                        return "Not Implemented";
                case SC_NOT_MODIFIED:
                        return "Not Modified";
                case SC_PARTIAL_CONTENT:
                        return "Partial Content";
                case SC_PAYMENT_REQUIRED:
                        return "Payment Required";
                case SC_PRECONDITION_FAILED:
                        return "Precondition Failed";
                case SC_PROXY_AUTHENTICATION_REQUIRED:
                        return "Proxy Authentication Required";
                case SC_REQUEST_ENTITY_TOO_LARGE:
                        return "Request Entity Too Large";
                case SC_REQUEST_TIMEOUT:
                        return "Request Timeout";
                case SC_REQUEST_URI_TOO_LARGE:
                        return "Request URI Too Large";
                case SC_RANGE_NOT_SATISFIABLE:
                        return "Requested Range Not Satisfiable";
                case SC_RESET_CONTENT:
                        return "Reset Content";
                case SC_SEE_OTHER:
                        return "See Other";
                case SC_SERVICE_UNAVAILABLE:
                        return "Service Unavailable";
                case SC_SWITCHING_PROTOCOLS:
                        return "Switching Protocols";
                case SC_UNAUTHORIZED:
                        return "Unauthorized";
                case SC_UNSUPPORTED_MEDIA_TYPE:
                        return "Unsupported Media Type";
                case SC_USE_PROXY:
                        return "Use Proxy";
                default: {
                        return "Unknown HTTP status";
                }
        }
}


/* ----------------------------------------------------------------- Private */


/**
 * Receives standard HTTP requests from a client socket and dispatches
 * them to the doXXX methods defined in a cervlet module.
 */
static void do_service(Socket_T s) {
        volatile HttpResponse res = create_HttpResponse(s);
        volatile HttpRequest req = create_HttpRequest(s);
        if (res && req) {
                if (Run.httpd.flags & Httpd_Ssl)
                        set_header(res, "Strict-Transport-Security", "max-age=63072000; includeSubdomains; preload");
                if (is_authenticated(req, res)) {
                        set_header(res, "Set-Cookie", "securitytoken=%s; Max-Age=600; HttpOnly; SameSite=strict%s", res->token, Run.httpd.flags & Httpd_Ssl ? "; Secure" : "");
                        if (IS(req->method, METHOD_GET))
                                Impl.doGet(req, res);
                        else if (IS(req->method, METHOD_POST))
                                Impl.doPost(req, res);
                        else
                                send_error(req, res, SC_NOT_IMPLEMENTED, "Method not implemented");
                }
                send_response(req, res);
        }
        done(req, res);
}


/**
 * Return a (RFC1123) Date string
 */
static char *get_date(char *result, int size) {
        time_t now;
        time(&now);
        if (strftime(result, size, DATEFMT, gmtime(&now)) <= 0)
                *result = 0;
        return result;
}


/**
 * Return this server name + version
 */
static char *get_server(char *result, int size) {
        snprintf(result, size, "%s %s", SERVER_NAME, Run.httpd.flags & Httpd_Signature ? SERVER_VERSION : "");
        return result;
}


/**
 * Send the response to the client. If the response has already been
 * commited, this function does nothing.
 */
static void send_response(HttpRequest req, HttpResponse res) {
        Socket_T S = res->S;

        if (! res->is_committed) {
                char date[STRLEN];
                char server[STRLEN];
#ifdef HAVE_LIBZ
                const char *acceptEncoding = get_header(req, "Accept-Encoding");
                boolean_t canCompress = acceptEncoding && Str_sub(acceptEncoding, "gzip") ? true : false;
#else
                boolean_t canCompress = false;
#endif
                const void *body = NULL;
                size_t bodyLength = 0;
                if (canCompress) {
                        body = StringBuffer_toCompressed(res->outputbuffer, 6, &bodyLength);
                        set_header(res, "Content-Encoding", "gzip");
                } else {
                        body = StringBuffer_toString(res->outputbuffer);
                        bodyLength = StringBuffer_length(res->outputbuffer);
                }
                char *headers = get_headers(res);
                res->is_committed = true;
                get_date(date, STRLEN);
                get_server(server, STRLEN);
                Socket_print(S, "%s %d %s\r\n", res->protocol, res->status, res->status_msg);
                Socket_print(S, "Date: %s\r\n", date);
                Socket_print(S, "Server: %s\r\n", server);
                Socket_print(S, "Content-Length: %zu\r\n", bodyLength);
                Socket_print(S, "Connection: close\r\n");
                if (headers)
                        Socket_print(S, "%s", headers);
                Socket_print(S, "\r\n");
                if (bodyLength)
                        Socket_write(S, (unsigned char *)body, bodyLength);
                FREE(headers);
        }
}


/* --------------------------------------------------------------- Factories */


/**
 * Returns a new HttpRequest object wrapping the client request
 */
static HttpRequest create_HttpRequest(Socket_T S) {
        char line[REQ_STRLEN];
        if (Socket_readLine(S, line, sizeof(line)) == NULL) {
                internal_error(S, SC_BAD_REQUEST, "No request found");
                return NULL;
        }
        Str_chomp(line);
        char method[STRLEN];
        char url[REQ_STRLEN];
        char protocol[STRLEN];
        if (sscanf(line, "%255s %1023s HTTP/%3[1.0]", method, url, protocol) != 3) {
                internal_error(S, SC_BAD_REQUEST, "Cannot parse request");
                return NULL;
        }
        if (strlen(url) >= MAX_URL_LENGTH) {
                internal_error(S, SC_BAD_REQUEST, "[error] URL too long");
                return NULL;
        }
        HttpRequest req = NULL;
        NEW(req);
        req->S = S;
        Util_urlDecode(url);
        req->url = Str_dup(url);
        req->method = Str_dup(method);
        req->protocol = Str_dup(protocol);
        create_headers(req);
        if (! create_parameters(req)) {
                destroy_HttpRequest(req);
                internal_error(S, SC_BAD_REQUEST, "Cannot parse Request parameters");
                return NULL;
        }
        return req;
}


/**
 * Returns a new HttpResponse object wrapping a default response. Use
 * the set_XXX methods to change the object.
 */
static HttpResponse create_HttpResponse(Socket_T S) {
        HttpResponse res = NULL;
        NEW(res);
        res->S = S;
        res->status = SC_OK;
        res->outputbuffer = StringBuffer_create(256);
        res->is_committed = false;
        res->protocol = SERVER_PROTOCOL;
        res->status_msg = get_status_string(SC_OK);
        Util_getToken(res->token);
        return res;
}


/**
 * Create HTTP headers for the given request
 */
static void create_headers(HttpRequest req) {
        char line[REQ_STRLEN] = {0};
        while (Socket_readLine(req->S, line, sizeof(line)) && ! (Str_isEqual(line, "\r\n") || Str_isEqual(line, "\n"))) {
                char *value = strchr(line, ':');
                if (value) {
                        HttpHeader header = NULL;
                        NEW(header);
                        *value++ = 0;
                        Str_trim(line);
                        Str_trim(value);
                        Str_chomp(value);
                        header->name = Str_dup(line);
                        header->value = Str_dup(value);
                        header->next = req->headers;
                        req->headers = header;
                }
        }
}


/**
 * Create parameters for the given request. Returns false if an error
 * occurs.
 */
static boolean_t create_parameters(HttpRequest req) {
        char *query_string = NULL;
        if (IS(req->method, METHOD_POST)) {
                int len;
                const char *content_length = get_header(req, "Content-Length");
                if (! content_length || sscanf(content_length, "%d", &len) != 1 || len < 0 || len > _httpPostLimit)
                        return false;
                if (len != 0) {
                        query_string = CALLOC(1, _httpPostLimit + 1);
                        int n = Socket_read(req->S, query_string, len);
                        if (n != len) {
                                FREE(query_string);
                                return false;
                        }
                }
        } else if (IS(req->method, METHOD_GET)) {
                char *p = strchr(req->url, '?');
                if (p) {
                        *p++ = 0;
                        query_string = Str_dup(p);
                }
        }
        if (query_string) {
                if (*query_string) {
                        char *p = strchr(query_string, '/');
                        if (p) {
                                *p++ = 0;
                                req->pathinfo = Str_dup(p);
                        }
                        req->params = parse_parameters(query_string);
                }
                FREE(query_string);
        }
        return true;
}


/* ----------------------------------------------------------------- Cleanup */


/**
 * Clear the response output buffer and headers
 */
static void reset_response(HttpResponse res) {
        if (res->headers) {
                destroy_entry(res->headers);
                res->headers = NULL; /* Release Pragma */
        }
        StringBuffer_clear(res->outputbuffer);
}


/**
 * Finalize the request and response object.
 */
static void done(HttpRequest req, HttpResponse res) {
        destroy_HttpRequest(req);
        destroy_HttpResponse(res);
}


/**
 * Free a HttpRequest object
 */
static void destroy_HttpRequest(HttpRequest req) {
        if (req) {
                FREE(req->method);
                FREE(req->url);
                FREE(req->pathinfo);
                FREE(req->protocol);
                FREE(req->remote_user);
                if (req->headers)
                        destroy_entry(req->headers);
                if (req->params)
                        destroy_entry(req->params);
                FREE(req);
        }
}


/**
 * Free a HttpResponse object
 */
static void destroy_HttpResponse(HttpResponse res) {
        if (res) {
                StringBuffer_free(&(res->outputbuffer));
                if (res->headers)
                        destroy_entry(res->headers);
                FREE(res);
        }
}


/**
 * Free a (linked list of) http entry object(s). Both HttpHeader and
 * HttpParameter are of this type.
 */
static void destroy_entry(void *p) {
        struct entry *h = p;
        if (h->next)
                destroy_entry(h->next);
        FREE(h->name);
        FREE(h->value);
        FREE(h);
}


/* ----------------------------------------------------- Checkers/Validators */


/**
 * Do Basic Authentication if this auth. style is allowed.
 */
static boolean_t is_authenticated(HttpRequest req, HttpResponse res) {
        if (Run.httpd.credentials) {
                if (! basic_authenticate(req)) {
                        // Send just generic error message to the client to not disclose e.g. username existence in case of credentials harvesting attack
                        send_error(req, res, SC_UNAUTHORIZED, "You are not authorized to access monit. Either you supplied the wrong credentials (e.g. bad password), or your browser doesn't understand how to supply the credentials required");
                        set_header(res, "WWW-Authenticate", "Basic realm=\"monit\"");
                        return false;
                }
        }
        if (IS(req->method, METHOD_POST)) {
                // Check CSRF double-submit cookie (https://www.owasp.org/index.php/Cross-Site_Request_Forgery_(CSRF)_Prevention_Cheat_Sheet#Double_Submit_Cookie)
                const char *cookie = get_header(req, "Cookie");
                const char *token = get_parameter(req, "securitytoken");
                if (! cookie) {
                        LogError("HttpRequest: access denied -- client [%s]: missing CSRF token cookie\n", NVLSTR(Socket_getRemoteHost(req->S)));
                        send_error(req, res, SC_FORBIDDEN, "Invalid CSRF Token");
                        return false;
                }
                if (! token) {
                        LogError("HttpRequest: access denied -- client [%s]: missing CSRF token in HTTP parameter\n", NVLSTR(Socket_getRemoteHost(req->S)));
                        send_error(req, res, SC_FORBIDDEN, "Invalid CSRF Token");
                        return false;
                }
                if (! Str_startsWith(cookie, "securitytoken=")) {
                        LogError("HttpRequest: access denied -- client [%s]: no CSRF token in cookie\n", NVLSTR(Socket_getRemoteHost(req->S)));
                        send_error(req, res, SC_FORBIDDEN, "Invalid CSRF Token");
                        return false;
                }
                if (Str_compareConstantTime(cookie + 14, token)) {
                        LogError("HttpRequest: access denied -- client [%s]: CSRF token mismatch\n", NVLSTR(Socket_getRemoteHost(req->S)));
                        send_error(req, res, SC_FORBIDDEN, "Invalid CSRF Token");
                        return false;
                }
        }
        return true;
}


/**
 * Authenticate the basic-credentials (uname/password) submitted by
 * the user.
 */
static boolean_t basic_authenticate(HttpRequest req) {
        const char *credentials = get_header(req, "Authorization");
        if (! (credentials && Str_startsWith(credentials, "Basic "))) {
                LogError("HttpRequest: access denied -- client [%s]: missing or invalid Authorization header\n", NVLSTR(Socket_getRemoteHost(req->S)));
                return false;
        }
        char buf[STRLEN] = {0};
        strncpy(buf, &credentials[6], sizeof(buf) - 1);
        char uname[STRLEN] = {0};
        if (decode_base64((unsigned char *)uname, buf) <= 0) {
                LogError("HttpRequest: access denied -- client [%s]: invalid Authorization header\n", NVLSTR(Socket_getRemoteHost(req->S)));
                return false;
        }
        if (! *uname) {
                LogError("HttpRequest: access denied -- client [%s]: empty username\n", NVLSTR(Socket_getRemoteHost(req->S)));
                return false;
        }
        char *password = password = strchr(uname, ':');
        if (! password || ! *password) {
                LogError("HttpRequest: access denied -- client [%s]: empty password\n", NVLSTR(Socket_getRemoteHost(req->S)));
                return false;
        }
        *password++ = 0;
        /* Check if user exist */
        if (! Util_getUserCredentials(uname)) {
                LogError("HttpRequest: access denied -- client [%s]: unknown user '%s'\n", NVLSTR(Socket_getRemoteHost(req->S)), uname);
                return false;
        }
        /* Check if user has supplied the right password */
        if (! Util_checkCredentials(uname,  password)) {
                LogError("HttpRequest: access denied -- client [%s]: wrong password for user '%s'\n", NVLSTR(Socket_getRemoteHost(req->S)), uname);
                return false;
        }
        req->remote_user = Str_dup(uname);
        return true;
}


/* --------------------------------------------------------------- Utilities */


/**
 * Send an error message to the client. This is a helper function,
 * used internal if the service function fails to setup the framework
 * properly; i.e. with a valid HttpRequest and a valid HttpResponse.
 */
static void internal_error(Socket_T S, int status, char *msg) {
        char date[STRLEN];
        char server[STRLEN];
        const char *status_msg = get_status_string(status);

        get_date(date, STRLEN);
        get_server(server, STRLEN);
        Socket_print(S,
                     "%s %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: %s\r\n"
                     "Content-Type: text/html\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "<html><head><title>%s</title></head>"
                     "<body bgcolor=#FFFFFF><h2>%s</h2>%s<p>"
                     "<hr><a href='%s'><font size=-1>%s</font></a>"
                     "</body></html>\r\n",
                     SERVER_PROTOCOL, status, status_msg, date, server,
                     status_msg, status_msg, msg, SERVER_URL, server);
        DEBUG("HttpRequest: error -- client [%s]: %s %d %s\n", NVLSTR(Socket_getRemoteHost(S)), SERVER_PROTOCOL, status, msg ? msg : status_msg);
}


/**
 * Parse request parameters from the given query string and return a
 * linked list of HttpParameters
 */
static HttpParameter parse_parameters(char *query_string) {
#define KEY 1
#define VALUE 2
        int token;
        int cursor = 0;
        char *key = NULL;
        char *value = NULL;
        HttpParameter head = NULL;

        while ((token = get_next_token(query_string, &cursor, &value))) {
                if (token == KEY)
                        key = value;
                else if (token == VALUE) {
                        HttpParameter p = NULL;
                        if (! key)
                                goto error;
                        NEW(p);
                        p->name = key;
                        p->value = value;
                        p->next = head;
                        head = p;
                        key = NULL;
                }
        }
        return head;
error:
        FREE(key);
        FREE(value);
        if ( head != NULL )
                destroy_entry(head);
        return NULL;
}


/**
 * A mini-scanner for tokenizing a query string
 */
static int get_next_token(char *s, int *cursor, char **r) {
        int i = *cursor;

        while (s[*cursor]) {
                if (s[*cursor+1] == '=') {
                        *cursor += 1;
                        *r = Str_ndup(&s[i], (*cursor-i));
                        return KEY;
                }
                if (s[*cursor] == '=') {
                        while (s[*cursor] && s[*cursor] != '&') *cursor += 1;
                        if (s[*cursor] == '&') {
                                *r = Str_ndup(&s[i+1], (*cursor-i)-1);
                                *cursor += 1;
                        }  else {
                                *r = Str_ndup(&s[i+1], (*cursor-i));
                        }
                        return VALUE;
                }
                *cursor += 1;
        }
        return 0;
}


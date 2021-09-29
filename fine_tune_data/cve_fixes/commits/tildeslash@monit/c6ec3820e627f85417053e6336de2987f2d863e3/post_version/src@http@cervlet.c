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

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

// libmonit
#include "system/Time.h"
#include "util/List.h"

#include "monit.h"
#include "cervlet.h"
#include "engine.h"
#include "processor.h"
#include "base64.h"
#include "event.h"
#include "alert.h"
#include "ProcessTree.h"
#include "device.h"
#include "protocol.h"
#include "Color.h"
#include "Box.h"


#define ACTION(c) ! strncasecmp(req->url, c, sizeof(c))


/* URL Commands supported */
#define HOME        "/"
#define TEST        "/_monit"
#define ABOUT       "/_about"
#define PING        "/_ping"
#define GETID       "/_getid"
#define STATUS      "/_status"
#define STATUS2     "/_status2"
#define SUMMARY     "/_summary"
#define REPORT      "/_report"
#define RUNTIME     "/_runtime"
#define VIEWLOG     "/_viewlog"
#define DOACTION    "/_doaction"
#define FAVICON     "/favicon.ico"


typedef enum {
        TXT = 0,
        HTML
} __attribute__((__packed__)) Output_Type;


/* Private prototypes */
static boolean_t is_readonly(HttpRequest);
static void printFavicon(HttpResponse);
static void doGet(HttpRequest, HttpResponse);
static void doPost(HttpRequest, HttpResponse);
static void do_head(HttpResponse res, const char *path, const char *name, int refresh);
static void do_foot(HttpResponse res);
static void do_home(HttpResponse);
static void do_home_system(HttpResponse);
static void do_home_filesystem(HttpResponse);
static void do_home_directory(HttpResponse);
static void do_home_file(HttpResponse);
static void do_home_fifo(HttpResponse);
static void do_home_net(HttpResponse);
static void do_home_process(HttpResponse);
static void do_home_program(HttpResponse);
static void do_home_host(HttpResponse);
static void do_about(HttpResponse);
static void do_ping(HttpResponse);
static void do_getid(HttpResponse);
static void do_runtime(HttpRequest, HttpResponse);
static void do_viewlog(HttpRequest, HttpResponse);
static void handle_service(HttpRequest, HttpResponse);
static void handle_service_action(HttpRequest, HttpResponse);
static void handle_doaction(HttpRequest, HttpResponse);
static void handle_runtime(HttpRequest, HttpResponse);
static void handle_runtime_action(HttpRequest, HttpResponse);
static void is_monit_running(HttpResponse);
static void do_service(HttpRequest, HttpResponse, Service_T);
static void print_alerts(HttpResponse, Mail_T);
static void print_buttons(HttpRequest, HttpResponse, Service_T);
static void print_service_rules_timeout(HttpResponse, Service_T);
static void print_service_rules_existence(HttpResponse, Service_T);
static void print_service_rules_port(HttpResponse, Service_T);
static void print_service_rules_socket(HttpResponse, Service_T);
static void print_service_rules_icmp(HttpResponse, Service_T);
static void print_service_rules_perm(HttpResponse, Service_T);
static void print_service_rules_uid(HttpResponse, Service_T);
static void print_service_rules_euid(HttpResponse, Service_T);
static void print_service_rules_gid(HttpResponse, Service_T);
static void print_service_rules_timestamp(HttpResponse, Service_T);
static void print_service_rules_fsflags(HttpResponse, Service_T);
static void print_service_rules_filesystem(HttpResponse, Service_T);
static void print_service_rules_size(HttpResponse, Service_T);
static void print_service_rules_linkstatus(HttpResponse, Service_T);
static void print_service_rules_linkspeed(HttpResponse, Service_T);
static void print_service_rules_linksaturation(HttpResponse, Service_T);
static void print_service_rules_uploadbytes(HttpResponse, Service_T);
static void print_service_rules_uploadpackets(HttpResponse, Service_T);
static void print_service_rules_downloadbytes(HttpResponse, Service_T);
static void print_service_rules_downloadpackets(HttpResponse, Service_T);
static void print_service_rules_uptime(HttpResponse, Service_T);
static void print_service_rules_content(HttpResponse, Service_T);
static void print_service_rules_checksum(HttpResponse, Service_T);
static void print_service_rules_pid(HttpResponse, Service_T);
static void print_service_rules_ppid(HttpResponse, Service_T);
static void print_service_rules_program(HttpResponse, Service_T);
static void print_service_rules_resource(HttpResponse, Service_T);
static void print_status(HttpRequest, HttpResponse, int);
static void print_summary(HttpRequest, HttpResponse);
static void _printReport(HttpRequest req, HttpResponse res);
static void status_service_txt(Service_T, HttpResponse);
static char *get_monitoring_status(Output_Type, Service_T s, char *, int);
static char *get_service_status(Output_Type, Service_T, char *, int);


/**
 *  Implementation of doGet and doPost routines used by the cervlet
 *  processor module. This particilary cervlet will provide
 *  information about the monit deamon and programs monitored by
 *  monit.
 *
 *  @file
 */


/* ------------------------------------------------------------------ Public */


/**
 * Callback hook to the Processor module for registering this modules
 * doGet and doPost methods.
 */
void init_service() {
        add_Impl(doGet, doPost);
}


/* ----------------------------------------------------------------- Private */


static char *_getUptime(time_t delta, char s[256]) {
        static int min = 60;
        static int hour = 3600;
        static int day = 86400;
        long rest_d;
        long rest_h;
        long rest_m;
        char *p = s;

        if (delta < 0) {
                *s = 0;
        } else {
                if ((rest_d = delta / day) > 0) {
                        p += snprintf(p, 256 - (p - s), "%ldd ", rest_d);
                        delta -= rest_d * day;
                }
                if ((rest_h = delta / hour) > 0 || (rest_d > 0)) {
                        p += snprintf(p, 256 - (p - s), "%ldh ", rest_h);
                        delta -= rest_h * hour;
                }
                rest_m = delta / min;
                snprintf(p, 256 - (p - s), "%ldm", rest_m);
        }
        return s;
}


static void _formatStatus(const char *name, Event_Type errorType, Output_Type type, HttpResponse res, Service_T s, boolean_t validValue, const char *value, ...) {
        if (type == HTML) {
                StringBuffer_append(res->outputbuffer, "<tr><td>%c%s</td>", toupper(name[0]), name + 1);
        } else {
                StringBuffer_append(res->outputbuffer, "  %-28s ", name);
        }
        if (! validValue) {
                StringBuffer_append(res->outputbuffer, type == HTML ? "<td class='gray-text'>-</td>" : COLOR_DARKGRAY "-" COLOR_RESET);
        } else {
                va_list ap;
                va_start(ap, value);
                char *_value = Str_vcat(value, ap);
                va_end(ap);
                if (errorType != Event_Null && s->error & errorType)
                        StringBuffer_append(res->outputbuffer, type == HTML ? "<td class='red-text'>" : COLOR_LIGHTRED);
                else
                        StringBuffer_append(res->outputbuffer, type == HTML ? "<td>" : COLOR_DEFAULT);
                if (type == HTML) {
                        // If the output contains multiple line, wrap use <pre>, otherwise keep as is
                        int multiline = strrchr(_value, '\n') > 0;
                        if (multiline)
                                StringBuffer_append(res->outputbuffer, "<pre>");
                        escapeHTML(res->outputbuffer, _value);
                        StringBuffer_append(res->outputbuffer, "%s</td>", multiline ? "</pre>" : "");
                } else {
                        int column = 0;
                        for (int i = 0; _value[i]; i++) {
                                if (_value[i] == '\r') {
                                        // Discard CR
                                        continue;
                                } else if (_value[i] == '\n') {
                                        // Indent 2nd+ line
                                        if (_value[i + 1])
                                        StringBuffer_append(res->outputbuffer, "\n                               ");
                                        column = 0;
                                        continue;
                                } else if (column <= 200) {
                                        StringBuffer_append(res->outputbuffer, "%c", _value[i]);
                                        column++;
                                }
                        }
                        StringBuffer_append(res->outputbuffer, COLOR_RESET);
                }
                FREE(_value);
        }
        StringBuffer_append(res->outputbuffer, type == HTML ? "</tr>" : "\n");
}


static void _printStatus(Output_Type type, HttpResponse res, Service_T s) {
        if (Util_hasServiceStatus(s)) {
                switch (s->type) {
                        case Service_System:
                                _formatStatus("load average", Event_Resource, type, res, s, true, "[%.2f] [%.2f] [%.2f]", systeminfo.loadavg[0], systeminfo.loadavg[1], systeminfo.loadavg[2]);
                                _formatStatus("cpu", Event_Resource, type, res, s, true, "%.1f%%us %.1f%%sy"
#ifdef HAVE_CPU_WAIT
                                        " %.1f%%wa"
#endif
                                        , systeminfo.total_cpu_user_percent > 0. ? systeminfo.total_cpu_user_percent : 0., systeminfo.total_cpu_syst_percent > 0. ? systeminfo.total_cpu_syst_percent : 0.
#ifdef HAVE_CPU_WAIT
                                        , systeminfo.total_cpu_wait_percent > 0. ? systeminfo.total_cpu_wait_percent : 0.
#endif
                                );
                                _formatStatus("memory usage", Event_Resource, type, res, s, true, "%s [%.1f%%]", Str_bytesToSize(systeminfo.total_mem, (char[10]){}), systeminfo.total_mem_percent);
                                _formatStatus("swap usage", Event_Resource, type, res, s, true, "%s [%.1f%%]", Str_bytesToSize(systeminfo.total_swap, (char[10]){}), systeminfo.total_swap_percent);
                                _formatStatus("uptime", Event_Uptime, type, res, s, systeminfo.booted > 0, "%s", _getUptime(Time_now() - systeminfo.booted, (char[256]){}));
                                _formatStatus("boot time", Event_Null, type, res, s, true, "%s", Time_string(systeminfo.booted, (char[32]){}));
                                break;

                        case Service_File:
                                _formatStatus("permission", Event_Permission, type, res, s, s->inf->priv.file.mode >= 0, "%o", s->inf->priv.file.mode & 07777);
                                _formatStatus("uid", Event_Uid, type, res, s, s->inf->priv.file.uid >= 0, "%d", s->inf->priv.file.uid);
                                _formatStatus("gid", Event_Gid, type, res, s, s->inf->priv.file.gid >= 0, "%d", s->inf->priv.file.gid);
                                _formatStatus("size", Event_Size, type, res, s, s->inf->priv.file.size >= 0, "%s", Str_bytesToSize(s->inf->priv.file.size, (char[10]){}));
                                _formatStatus("timestamp", Event_Timestamp, type, res, s, s->inf->priv.file.timestamp > 0, "%s", Time_string(s->inf->priv.file.timestamp, (char[32]){}));
                                if (s->matchlist)
                                        _formatStatus("content match", Event_Content, type, res, s, true, "%s", (s->error & Event_Content) ? "yes" : "no");
                                if (s->checksum)
                                        _formatStatus("checksum", Event_Checksum, type, res, s, *s->inf->priv.file.cs_sum, "%s (%s)", s->inf->priv.file.cs_sum, checksumnames[s->checksum->type]);
                                break;

                        case Service_Directory:
                                _formatStatus("permission", Event_Permission, type, res, s, s->inf->priv.directory.mode >= 0, "%o", s->inf->priv.directory.mode & 07777);
                                _formatStatus("uid", Event_Uid, type, res, s, s->inf->priv.directory.uid >= 0, "%d", s->inf->priv.directory.uid);
                                _formatStatus("gid", Event_Gid, type, res, s, s->inf->priv.directory.gid >= 0, "%d", s->inf->priv.directory.gid);
                                _formatStatus("timestamp", Event_Timestamp, type, res, s, s->inf->priv.directory.timestamp > 0, "%s", Time_string(s->inf->priv.directory.timestamp, (char[32]){}));
                                break;

                        case Service_Fifo:
                                _formatStatus("permission", Event_Permission, type, res, s, s->inf->priv.fifo.mode >= 0, "%o", s->inf->priv.fifo.mode & 07777);
                                _formatStatus("uid", Event_Uid, type, res, s, s->inf->priv.fifo.uid >= 0, "%d", s->inf->priv.fifo.uid);
                                _formatStatus("gid", Event_Gid, type, res, s, s->inf->priv.fifo.gid >= 0, "%d", s->inf->priv.fifo.gid);
                                _formatStatus("timestamp", Event_Timestamp, type, res, s, s->inf->priv.fifo.timestamp > 0, "%s", Time_string(s->inf->priv.fifo.timestamp, (char[32]){}));
                                break;

                        case Service_Net:
                                {
                                        long long speed = Link_getSpeed(s->inf->priv.net.stats);
                                        long long ibytes = Link_getBytesInPerSecond(s->inf->priv.net.stats);
                                        long long obytes = Link_getBytesOutPerSecond(s->inf->priv.net.stats);
                                        _formatStatus("link", Event_Link, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%d errors", Link_getErrorsInPerSecond(s->inf->priv.net.stats) + Link_getErrorsOutPerSecond(s->inf->priv.net.stats));
                                        if (speed > 0) {
                                                _formatStatus("capacity", Event_Speed, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%.0lf Mb/s %s-duplex", (double)speed / 1000000., Link_getDuplex(s->inf->priv.net.stats) == 1 ? "full" : "half");
                                                _formatStatus("download bytes", Event_ByteIn, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%s/s (%.1f%% link saturation)", Str_bytesToSize(ibytes, (char[10]){}), 100. * ibytes * 8 / (double)speed);
                                                _formatStatus("upload bytes", Event_ByteOut, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%s/s (%.1f%% link saturation)", Str_bytesToSize(obytes, (char[10]){}), 100. * obytes * 8 / (double)speed);
                                        } else {
                                                _formatStatus("download bytes", Event_ByteIn, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%s/s", Str_bytesToSize(ibytes, (char[10]){}));
                                                _formatStatus("upload bytes", Event_ByteOut, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%s/s", Str_bytesToSize(obytes, (char[10]){}));
                                        }
                                        _formatStatus("download packets", Event_PacketIn, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%lld per second", Link_getPacketsInPerSecond(s->inf->priv.net.stats));
                                        _formatStatus("upload packets", Event_PacketOut, type, res, s, Link_getState(s->inf->priv.net.stats) == 1, "%lld per second", Link_getPacketsOutPerSecond(s->inf->priv.net.stats));
                                }
                                break;

                        case Service_Filesystem:
                                _formatStatus("permission", Event_Permission, type, res, s, s->inf->priv.filesystem.mode >= 0, "%o", s->inf->priv.filesystem.mode & 07777);
                                _formatStatus("uid", Event_Uid, type, res, s, s->inf->priv.filesystem.uid >= 0, "%d", s->inf->priv.filesystem.uid);
                                _formatStatus("gid", Event_Gid, type, res, s, s->inf->priv.filesystem.gid >= 0, "%d", s->inf->priv.filesystem.gid);
                                _formatStatus("filesystem flags", Event_Fsflag, type, res, s, true, "0x%x", s->inf->priv.filesystem.flags);
                                _formatStatus("block size", Event_Null, type, res, s, true, "%s", Str_bytesToSize(s->inf->priv.filesystem.f_bsize, (char[10]){}));
                                _formatStatus("space total", Event_Null, type, res, s, true, "%s (of which %.1f%% is reserved for root user)",
                                        s->inf->priv.filesystem.f_bsize > 0 ? Str_bytesToSize(s->inf->priv.filesystem.f_blocks * s->inf->priv.filesystem.f_bsize, (char[10]){}) : "0 MB",
                                        s->inf->priv.filesystem.f_blocks > 0 ? ((float)100 * (float)(s->inf->priv.filesystem.f_blocksfreetotal - s->inf->priv.filesystem.f_blocksfree) / (float)s->inf->priv.filesystem.f_blocks) : 0);
                                _formatStatus("space free for non superuser", Event_Null, type, res, s, true, "%s [%.1f%%]",
                                        s->inf->priv.filesystem.f_bsize > 0 ? Str_bytesToSize(s->inf->priv.filesystem.f_blocksfree * s->inf->priv.filesystem.f_bsize, (char[10]){}) : "0 MB",
                                        s->inf->priv.filesystem.f_blocks > 0 ? ((float)100 * (float)s->inf->priv.filesystem.f_blocksfree / (float)s->inf->priv.filesystem.f_blocks) : 0);
                                _formatStatus("space free total", Event_Resource, type, res, s, true, "%s [%.1f%%]",
                                        s->inf->priv.filesystem.f_bsize > 0 ? Str_bytesToSize(s->inf->priv.filesystem.f_blocksfreetotal * s->inf->priv.filesystem.f_bsize, (char[10]){}) : "0 MB",
                                        s->inf->priv.filesystem.f_blocks > 0 ? ((float)100 * (float)s->inf->priv.filesystem.f_blocksfreetotal / (float)s->inf->priv.filesystem.f_blocks) : 0);
                                if (s->inf->priv.filesystem.f_files > 0) {
                                        _formatStatus("inodes total", Event_Null, type, res, s, true, "%lld", s->inf->priv.filesystem.f_files);
                                        _formatStatus("inodes free", Event_Resource, type, res, s, true, "%lld [%.1f%%]", s->inf->priv.filesystem.f_filesfree, (float)100 * (float)s->inf->priv.filesystem.f_filesfree / (float)s->inf->priv.filesystem.f_files);
                                }
                                break;

                        case Service_Process:
                                _formatStatus("pid", Event_Pid, type, res, s, s->inf->priv.process.pid >= 0, "%d", s->inf->priv.process.pid);
                                _formatStatus("parent pid", Event_PPid, type, res, s, s->inf->priv.process.ppid >= 0, "%d", s->inf->priv.process.ppid);
                                _formatStatus("uid", Event_Uid, type, res, s, s->inf->priv.process.uid >= 0, "%d", s->inf->priv.process.uid);
                                _formatStatus("effective uid", Event_Uid, type, res, s, s->inf->priv.process.euid >= 0, "%d", s->inf->priv.process.euid);
                                _formatStatus("gid", Event_Gid, type, res, s, s->inf->priv.process.gid >= 0, "%d", s->inf->priv.process.gid);
                                _formatStatus("uptime", Event_Uptime, type, res, s, s->inf->priv.process.uptime >= 0, "%s", _getUptime(s->inf->priv.process.uptime, (char[256]){}));
                                if (Run.flags & Run_ProcessEngineEnabled) {
                                        _formatStatus("threads", Event_Resource, type, res, s, s->inf->priv.process.threads >= 0, "%d", s->inf->priv.process.threads);
                                        _formatStatus("children", Event_Resource, type, res, s, s->inf->priv.process.children >= 0, "%d", s->inf->priv.process.children);
                                        _formatStatus("cpu", Event_Resource, type, res, s, s->inf->priv.process.cpu_percent >= 0, "%.1f%%", s->inf->priv.process.cpu_percent);
                                        _formatStatus("cpu total", Event_Resource, type, res, s, s->inf->priv.process.total_cpu_percent >= 0, "%.1f%%", s->inf->priv.process.total_cpu_percent);
                                        _formatStatus("memory", Event_Resource, type, res, s, s->inf->priv.process.mem_percent >= 0, "%.1f%% [%s]", s->inf->priv.process.mem_percent, Str_bytesToSize(s->inf->priv.process.mem, (char[10]){}));
                                        _formatStatus("memory total", Event_Resource, type, res, s, s->inf->priv.process.total_mem_percent >= 0, "%.1f%% [%s]", s->inf->priv.process.total_mem_percent, Str_bytesToSize(s->inf->priv.process.total_mem, (char[10]){}));
                                }
                                break;

                        case Service_Program:
                                if (s->program->started) {
                                        _formatStatus("last exit value", Event_Status, type, res, s, true, "%d", s->program->exitStatus);
                                        _formatStatus("last output", Event_Status, type, res, s, StringBuffer_length(s->program->output), "%s", StringBuffer_toString(s->program->output));
                                }
                                break;

                        default:
                                break;
                }
                for (Icmp_T i = s->icmplist; i; i = i->next) {
                        if (i->is_available == Connection_Failed)
                                _formatStatus("ping response time", Event_Icmp, type, res, s, true, "connection failed");
                        else
                                _formatStatus("ping response time", Event_Null, type, res, s, i->is_available != Connection_Init && i->response >= 0., "%s", Str_milliToTime(i->response, (char[23]){}));
                }
                for (Port_T p = s->portlist; p; p = p->next) {
                        if (p->is_available == Connection_Failed) {
                                _formatStatus("port response time", Event_Connection, type, res, s, true, "FAILED to [%s]:%d%s type %s/%s %sprotocol %s", p->hostname, p->target.net.port, Util_portRequestDescription(p), Util_portTypeDescription(p), Util_portIpDescription(p), p->target.net.ssl.flags ? "using SSL/TLS " : "", p->protocol->name);
                        } else {
                                _formatStatus("port response time", Event_Null, type, res, s, p->is_available != Connection_Init, "%s to %s:%d%s type %s/%s %s protocol %s", Str_milliToTime(p->response, (char[23]){}), p->hostname, p->target.net.port, Util_portRequestDescription(p), Util_portTypeDescription(p), Util_portIpDescription(p), p->target.net.ssl.flags ? "using SSL/TLS " : "", p->protocol->name);
                        }
                }
                for (Port_T p = s->socketlist; p; p = p->next) {
                        if (p->is_available == Connection_Failed) {
                                _formatStatus("unix socket response time", Event_Connection, type, res, s, true, "FAILED to %s type %s protocol %s", p->target.unix.pathname, Util_portTypeDescription(p), p->protocol->name);
                        } else {
                                _formatStatus("unix socket response time", Event_Null, type, res, s, p->is_available != Connection_Init, "%s to %s type %s protocol %s", Str_milliToTime(p->response, (char[23]){}), p->target.unix.pathname, Util_portTypeDescription(p), p->protocol->name);
                        }
                }
        }
        _formatStatus("data collected", Event_Null, type, res, s, true, "%s", Time_string(s->collected.tv_sec, (char[32]){}));
}


/**
 * Called by the Processor (via the service method)
 * to handle a POST request.
 */
static void doPost(HttpRequest req, HttpResponse res) {
        set_content_type(res, "text/html");
        if (ACTION(RUNTIME))
                handle_runtime_action(req, res);
        else if (ACTION(VIEWLOG))
                do_viewlog(req, res);
        else if (ACTION(STATUS))
                print_status(req, res, 1);
        else if (ACTION(STATUS2))
                print_status(req, res, 2);
        else if (ACTION(SUMMARY))
                print_summary(req, res);
        else if (ACTION(REPORT))
                _printReport(req, res);
        else if (ACTION(DOACTION))
                handle_doaction(req, res);
        else
                handle_service_action(req, res);
}


/**
 * Called by the Processor (via the service method)
 * to handle a GET request.
 */
static void doGet(HttpRequest req, HttpResponse res) {
        set_content_type(res, "text/html");
        if (ACTION(HOME)) {
                LOCK(Run.mutex)
                do_home(res);
                END_LOCK;
        } else if (ACTION(RUNTIME)) {
                handle_runtime(req, res);
        } else if (ACTION(TEST)) {
                is_monit_running(res);
        } else if (ACTION(ABOUT)) {
                do_about(res);
        } else if (ACTION(FAVICON)) {
                printFavicon(res);
        } else if (ACTION(PING)) {
                do_ping(res);
        } else if (ACTION(GETID)) {
                do_getid(res);
        } else {
                handle_service(req, res);
        }
}


/* ----------------------------------------------------------------- Helpers */


static void is_monit_running(HttpResponse res) {
        set_status(res, exist_daemon() ? SC_OK : SC_GONE);
}


static void printFavicon(HttpResponse res) {
        static size_t l;
        Socket_T S = res->S;
        static unsigned char *favicon = NULL;

        if (! favicon) {
                favicon = CALLOC(sizeof(unsigned char), strlen(FAVICON_ICO));
                l = decode_base64(favicon, FAVICON_ICO);
        }
        if (l) {
                res->is_committed = true;
                Socket_print(S, "HTTP/1.0 200 OK\r\n");
                Socket_print(S, "Content-length: %lu\r\n", (unsigned long)l);
                Socket_print(S, "Content-Type: image/x-icon\r\n");
                Socket_print(S, "Connection: close\r\n\r\n");
                Socket_write(S, favicon, l);
        }
}


static void do_head(HttpResponse res, const char *path, const char *name, int refresh) {
        StringBuffer_append(res->outputbuffer,
                            "<!DOCTYPE html>"\
                            "<html>"\
                            "<head>"\
                            "<title>Monit: %s</title> "\
                            "<style type=\"text/css\"> "\
                            " html, body {height: 100%%;margin: 0;} "\
                            " body {background-color: white;font: normal normal normal 16px/20px 'HelveticaNeue', Helvetica, Arial, sans-serif; color:#222;} "\
                            " h1 {padding:30px 0 10px 0; text-align:center;color:#222;font-size:28px;} "\
                            " h2 {padding:20px 0 10px 0; text-align:center;color:#555;font-size:22px;} "\
                            " a:hover {text-decoration: none;} "\
                            " a {text-decoration: underline;color:#222} "\
                            " table {border-collapse:collapse; border:0;} "\
                            " .stripe {background:#EDF5FF} "\
                            " .rule {background:#ddd} "\
                            " .red-text {color:#ff0000;} "\
                            " .green-text {color:#00ff00;} "\
                            " .gray-text {color:#999999;} "\
                            " .blue-text {color:#0000ff;} "\
                            " .yellow-text {color:#ffff00;} "\
                            " .orange-text {color:#ff8800;} "\
                            " .short {overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 350px;}"\
                            " #wrap {min-height: 100%%;} "\
                            " #main {overflow:auto; padding-bottom:50px;} "\
                            " /*Opera Fix*/body:before {content:\"\";height:100%%;float:left;width:0;margin-top:-32767px;/} "\
                            " #footer {position: relative;margin-top: -50px; height: 50px; clear:both; font-size:11px;color:#777;text-align:center;} "\
                            " #footer a {color:#333;} #footer a:hover {text-decoration: none;} "\
                            " #nav {background:#ddd;font:normal normal normal 14px/0px 'HelveticaNeue', Helvetica;} "\
                            " #nav td {padding:5px 10px;} "\
                            " #header {margin-bottom:30px;background:#EFF7FF} "\
                            " #nav, #header {border-bottom:1px solid #ccc;} "\
                            " #header-row {width:95%%;} "\
                            " #header-row th {padding:30px 10px 10px 10px;font-size:120%%;} "\
                            " #header-row td {padding:3px 10px;} "\
                            " #header-row .first {min-width:200px;width:200px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;} "\
                            " #status-table {width:95%%;} "\
                            " #status-table th {text-align:left;background:#edf5ff;font-weight:normal;} "\
                            " #status-table th, #status-table td, #status-table tr {border:1px solid #ccc;padding:5px;} "\
                            " #buttons {font-size:20px; margin:40px 0 20px 0;} "\
                            " #buttons td {padding-right:50px;} "\
                            " #buttons input {font-size:18px;padding:5px;} "\
                            "</style>"\
                            "<meta HTTP-EQUIV='REFRESH' CONTENT=%d> "\
                            "<meta HTTP-EQUIV='Expires' Content=0> "\
                            "<meta HTTP-EQUIV='Pragma' CONTENT='no-cache'> "\
                            "<meta charset='UTF-8'>" \
                            "<link rel='shortcut icon' href='favicon.ico'>"\
                            "</head>"\
                            "<body><div id='wrap'><div id='main'>" \
                            "<table id='nav' width='100%%'>"\
                            "  <tr>"\
                            "    <td width='20%%'><a href='.'>Home</a>&nbsp;&gt;&nbsp;<a href='%s'>%s</a></td>"\
                            "    <td width='60%%' style='text-align:center;'>Use <a href='http://mmonit.com/'>M/Monit</a> to manage all your Monit instances</td>"\
                            "    <td width='20%%'><p align='right'><a href='_about'>Monit %s</a></td>"\
                            "  </tr>"\
                            "</table>"\
                            "<center>",
                            Run.system->name, refresh, path, name, VERSION);
}


static void do_foot(HttpResponse res) {
        StringBuffer_append(res->outputbuffer,
                            "</center></div></div>"
                            "<div id='footer'>"
                            "Copyright &copy; 2001-2016 <a href=\"http://tildeslash.com/\">Tildeslash</a>. All rights reserved. "
                            "<span style='margin-left:5px;'></span>"
                            "<a href=\"http://mmonit.com/monit/\">Monit web site</a> | "
                            "<a href=\"http://mmonit.com/wiki/\">Monit Wiki</a> | "
                            "<a href=\"http://mmonit.com/\">M/Monit</a>"
                            "</div></body></html>");
}


static void do_home(HttpResponse res) {
        do_head(res, "", "", Run.polltime);
        StringBuffer_append(res->outputbuffer,
                            "<table id='header' width='100%%'>"
                            " <tr>"
                            "  <td colspan=2 valign='top' align='left' width='100%%'>"
                            "  <h1>Monit Service Manager</h1>"
                            "  <p align='center'>Monit is <a href='_runtime'>running</a> on %s and monitoring:</p><br>"
                            "  </td>"
                            " </tr>"
                            "</table>", Run.system->name);

        do_home_system(res);
        do_home_process(res);
        do_home_program(res);
        do_home_filesystem(res);
        do_home_file(res);
        do_home_fifo(res);
        do_home_directory(res);
        do_home_net(res);
        do_home_host(res);

        do_foot(res);
}


static void do_about(HttpResponse res) {
        StringBuffer_append(res->outputbuffer,
                            "<html><head><title>about monit</title></head><body bgcolor=white>"
                            "<br><h1><center><a href='http://mmonit.com/monit/'>"
                            "monit " VERSION "</a></center></h1>");
        StringBuffer_append(res->outputbuffer,
                            "<ul>"
                            "<li style='padding-bottom:10px;'>Copyright &copy; 2001-2016 <a "
                            "href='http://tildeslash.com/'>Tildeslash Ltd"
                            "</a>. All Rights Reserved.</li></ul>");
        StringBuffer_append(res->outputbuffer, "<hr size='1'>");
        StringBuffer_append(res->outputbuffer,
                            "<p>This program is free software; you can redistribute it and/or "
                            "modify it under the terms of the GNU Affero General Public License version 3</p>"
                            "<p>This program is distributed in the hope that it will be useful, but "
                            "WITHOUT ANY WARRANTY; without even the implied warranty of "
                            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the "
                            "<a href='http://www.gnu.org/licenses/agpl.html'>"
                            "GNU AFFERO GENERAL PUBLIC LICENSE</a> for more details.</p>");
        StringBuffer_append(res->outputbuffer,
                            "<center><p style='padding-top:20px;'>[<a href='.'>Back to Monit</a>]</p></body></html>");
}


static void do_ping(HttpResponse res) {
        StringBuffer_append(res->outputbuffer, "pong");
}


static void do_getid(HttpResponse res) {
        StringBuffer_append(res->outputbuffer, "%s", Run.id);
}

static void do_runtime(HttpRequest req, HttpResponse res) {
        int pid = exist_daemon();
        char buf[STRLEN];

        do_head(res, "_runtime", "Runtime", 1000);
        StringBuffer_append(res->outputbuffer,
                            "<h2>Monit runtime status</h2>");
        StringBuffer_append(res->outputbuffer, "<table id='status-table'><tr>"
                            "<th width='40%%'>Parameter</th>"
                            "<th width='60%%'>Value</th></tr>");
        StringBuffer_append(res->outputbuffer, "<tr><td>Monit ID</td><td>%s</td></tr>", Run.id);
        StringBuffer_append(res->outputbuffer, "<tr><td>Host</td><td>%s</td></tr>",  Run.system->name);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Process id</td><td>%d</td></tr>", pid);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Effective user running Monit</td>"
                            "<td>%s</td></tr>", Run.Env.user);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Controlfile</td><td>%s</td></tr>", Run.files.control);
        if (Run.files.log)
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>Logfile</td><td>%s</td></tr>", Run.files.log);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Pidfile</td><td>%s</td></tr>", Run.files.pid);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>State file</td><td>%s</td></tr>", Run.files.state);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Debug</td><td>%s</td></tr>",
                            Run.debug ? "True" : "False");
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Log</td><td>%s</td></tr>", (Run.flags & Run_Log) ? "True" : "False");
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Use syslog</td><td>%s</td></tr>",
                            (Run.flags & Run_UseSyslog) ? "True" : "False");
        if (Run.eventlist_dir) {
                if (Run.eventlist_slots < 0)
                        snprintf(buf, STRLEN, "unlimited");
                else
                        snprintf(buf, STRLEN, "%d", Run.eventlist_slots);
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>Event queue</td>"
                                    "<td>base directory %s with %d slots</td></tr>",
                                    Run.eventlist_dir, Run.eventlist_slots);
        }
#ifdef HAVE_OPENSSL
        {
                const char *options = Ssl_printOptions(&(Run.ssl), (char[STRLEN]){}, STRLEN);
                if (options && *options)
                        StringBuffer_append(res->outputbuffer,
                                    "<tr><td>SSL options</td><td>%s</td></tr>", options);
        }
#endif
        if (Run.mmonits) {
                StringBuffer_append(res->outputbuffer, "<tr><td>M/Monit server(s)</td><td>");
                for (Mmonit_T c = Run.mmonits; c; c = c->next)
                {
                        StringBuffer_append(res->outputbuffer, "%s with timeout %s", c->url->url, Str_milliToTime(c->timeout, (char[23]){}));
#ifdef HAVE_OPENSSL
                        if (c->ssl.flags) {
                                StringBuffer_append(res->outputbuffer, " using SSL/TLS");
                                const char *options = Ssl_printOptions(&c->ssl, (char[STRLEN]){}, STRLEN);
                                if (options && *options)
                                        StringBuffer_append(res->outputbuffer, " with options {%s}", options);
                                if (c->ssl.checksum)
                                        StringBuffer_append(res->outputbuffer, " and certificate checksum %s equal to '%s'", checksumnames[c->ssl.checksumType], c->ssl.checksum);
                        }
#endif
                        if (c->url->user)
                                StringBuffer_append(res->outputbuffer, " using credentials");
                        if (c->next)
                                StringBuffer_append(res->outputbuffer, "</td></tr><tr><td>&nbsp;</td><td>");
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (Run.mailservers) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Mail server(s)</td><td>");
                for (MailServer_T mta = Run.mailservers; mta; mta = mta->next) {
                        StringBuffer_append(res->outputbuffer, "%s:%d", mta->host, mta->port);
#ifdef HAVE_OPENSSL
                        if (mta->ssl.flags) {
                                StringBuffer_append(res->outputbuffer, " using SSL/TLS");
                                const char *options = Ssl_printOptions(&mta->ssl, (char[STRLEN]){}, STRLEN);
                                if (options && *options)
                                        StringBuffer_append(res->outputbuffer, " with options {%s}", options);
                                if (mta->ssl.checksum)
                                        StringBuffer_append(res->outputbuffer, " and certificate checksum %s equal to '%s'", checksumnames[mta->ssl.checksumType], mta->ssl.checksum);
                        }
#endif
                        if (mta->next)
                                StringBuffer_append(res->outputbuffer, "</td></tr><tr><td>&nbsp;</td><td>");
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (Run.MailFormat.from) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Default mail from</td><td>");
                if (Run.MailFormat.from->name)
                        StringBuffer_append(res->outputbuffer, "%s &lt;%s&gt;", Run.MailFormat.from->name, Run.MailFormat.from->address);
                else
                        StringBuffer_append(res->outputbuffer, "%s", Run.MailFormat.from->address);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (Run.MailFormat.replyto) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Default mail reply to</td><td>");
                if (Run.MailFormat.replyto->name)
                        StringBuffer_append(res->outputbuffer, "%s &lt;%s&gt;", Run.MailFormat.replyto->name, Run.MailFormat.replyto->address);
                else
                        StringBuffer_append(res->outputbuffer, "%s", Run.MailFormat.replyto->address);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (Run.MailFormat.subject)
                StringBuffer_append(res->outputbuffer, "<tr><td>Default mail subject</td><td>%s</td></tr>", Run.MailFormat.subject);
        if (Run.MailFormat.message)
                StringBuffer_append(res->outputbuffer, "<tr><td>Default mail message</td><td>%s</td></tr>", Run.MailFormat.message);
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for Send/Expect buffer</td><td>%s</td></tr>", Str_bytesToSize(Run.limits.sendExpectBuffer, buf));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for file content buffer</td><td>%s</td></tr>", Str_bytesToSize(Run.limits.fileContentBuffer, buf));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for HTTP content buffer</td><td>%s</td></tr>", Str_bytesToSize(Run.limits.httpContentBuffer, buf));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for program output</td><td>%s</td></tr>", Str_bytesToSize(Run.limits.programOutput, buf));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for network timeout</td><td>%s</td></tr>", Str_milliToTime(Run.limits.networkTimeout, (char[23]){}));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for check program timeout</td><td>%s</td></tr>", Str_milliToTime(Run.limits.programTimeout, (char[23]){}));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for service stop timeout</td><td>%s</td></tr>", Str_milliToTime(Run.limits.stopTimeout, (char[23]){}));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for service start timeout</td><td>%s</td></tr>", Str_milliToTime(Run.limits.startTimeout, (char[23]){}));
        StringBuffer_append(res->outputbuffer, "<tr><td>Limit for service restart timeout</td><td>%s</td></tr>", Str_milliToTime(Run.limits.restartTimeout, (char[23]){}));
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>On reboot</td><td>%s</td></tr>", onrebootnames[Run.onreboot]);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Poll time</td><td>%d seconds with start delay %d seconds</td></tr>",
                            Run.polltime, Run.startdelay);
        if (Run.httpd.flags & Httpd_Net) {
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>httpd bind address</td><td>%s</td></tr>",
                                    Run.httpd.socket.net.address ? Run.httpd.socket.net.address : "Any/All");
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>httpd portnumber</td><td>%d</td></tr>", Run.httpd.socket.net.port);
        } else if (Run.httpd.flags & Httpd_Unix) {
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>httpd unix socket</td><td>%s</td></tr>",
                                    Run.httpd.socket.unix.path);
        }
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>httpd signature</td><td>%s</td></tr>",
                            Run.httpd.flags & Httpd_Signature ? "True" : "False");
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Use ssl encryption</td><td>%s</td></tr>",
                            Run.httpd.flags & Httpd_Ssl ? "True" : "False");
        if (Run.httpd.flags & Httpd_Ssl) {
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>PEM key/certificate file</td><td>%s</td></tr>",
                                    Run.httpd.socket.net.ssl.pem);
                if (Run.httpd.socket.net.ssl.clientpem != NULL) {
                        StringBuffer_append(res->outputbuffer,
                                            "<tr><td>Client PEM key/certification"
                                            "</td><td>%s</td></tr>", "Enabled");
                        StringBuffer_append(res->outputbuffer,
                                            "<tr><td>Client PEM key/certificate file"
                                            "</td><td>%s</td></tr>", Run.httpd.socket.net.ssl.clientpem);
                } else {
                        StringBuffer_append(res->outputbuffer,
                                            "<tr><td>Client PEM key/certification"
                                            "</td><td>%s</td></tr>", "Disabled");
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr><td>Allow self certified certificates "
                                    "</td><td>%s</td></tr>", Run.httpd.flags & Httpd_AllowSelfSignedCertificates ? "True" : "False");
        }
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>httpd auth. style</td><td>%s</td></tr>",
                            Run.httpd.credentials && Engine_hasAllow() ? "Basic Authentication and Host/Net allow list" : Run.httpd.credentials ? "Basic Authentication" : Engine_hasAllow() ? "Host/Net allow list" : "No authentication");
        print_alerts(res, Run.maillist);
        StringBuffer_append(res->outputbuffer, "</table>");
        if (! is_readonly(req)) {
                StringBuffer_append(res->outputbuffer,
                                    "<table id='buttons'><tr>");
                StringBuffer_append(res->outputbuffer,
                                    "<td style='color:red;'>"
                                    "<form method=POST action='_runtime'>Stop Monit http server? "
                                    "<input type=hidden name='securitytoken' value='%s'>"
                                    "<input type=hidden name='action' value='stop'>"
                                    "<input type=submit value='Go'>"
                                    "</form>"
                                    "</td>",
                                    res->token);
                StringBuffer_append(res->outputbuffer,
                                    "<td>"
                                    "<form method=POST action='_runtime'>Force validate now? "
                                    "<input type=hidden name='securitytoken' value='%s'>"
                                    "<input type=hidden name='action' value='validate'>"
                                    "<input type=submit value='Go'>"
                                    "</form>"
                                    "</td>",
                                    res->token);

                if ((Run.flags & Run_Log) && ! (Run.flags & Run_UseSyslog)) {
                        StringBuffer_append(res->outputbuffer,
                                            "<td>"
                                            "<form method=POST action='_viewlog'>View Monit logfile? "
                                            "<input type=hidden name='securitytoken' value='%s'>"
                                            "<input type=submit value='Go'>"
                                            "</form>"
                                            "</td>",
                                            res->token);
                }
                StringBuffer_append(res->outputbuffer,
                                    "</tr></table>");
        }
        do_foot(res);
}


static void do_viewlog(HttpRequest req, HttpResponse res) {
        if (is_readonly(req)) {
                send_error(req, res, SC_FORBIDDEN, "You do not have sufficient privileges to access this page");
                return;
        }
        do_head(res, "_viewlog", "View log", 100);
        if ((Run.flags & Run_Log) && ! (Run.flags & Run_UseSyslog)) {
                struct stat sb;
                if (! stat(Run.files.log, &sb)) {
                        FILE *f = fopen(Run.files.log, "r");
                        if (f) {
#define BUFSIZE 512
                                size_t n;
                                char buf[BUFSIZE+1];
                                StringBuffer_append(res->outputbuffer, "<br><p><form><textarea cols=120 rows=30 readonly>");
                                while ((n = fread(buf, sizeof(char), BUFSIZE, f)) > 0) {
                                        buf[n] = 0;
                                        StringBuffer_append(res->outputbuffer, "%s", buf);
                                }
                                fclose(f);
                                StringBuffer_append(res->outputbuffer, "</textarea></form>");
                        } else {
                                StringBuffer_append(res->outputbuffer, "Error opening logfile: %s", STRERROR);
                        }
                } else {
                        StringBuffer_append(res->outputbuffer, "Error stating logfile: %s", STRERROR);
                }
        } else {
                StringBuffer_append(res->outputbuffer,
                                    "<b>Cannot view logfile:</b><br>");
                if (! (Run.flags & Run_Log))
                        StringBuffer_append(res->outputbuffer, "Monit was started without logging");
                else
                        StringBuffer_append(res->outputbuffer, "Monit uses syslog");
        }
        do_foot(res);
}


static void handle_service(HttpRequest req, HttpResponse res) {
        char *name = req->url;
        Service_T s = Util_getService(++name);
        if (! s) {
                send_error(req, res, SC_NOT_FOUND, "There is no service named \"%s\"", name ? name : "");
                return;
        }
        do_service(req, res, s);
}


static void handle_service_action(HttpRequest req, HttpResponse res) {
        char *name = req->url;
        Service_T s = Util_getService(++name);
        if (! s) {
                send_error(req, res, SC_NOT_FOUND, "There is no service named \"%s\"", name ? name : "");
                return;
        }
        const char *action = get_parameter(req, "action");
        if (action) {
                if (is_readonly(req)) {
                        send_error(req, res, SC_FORBIDDEN, "You do not have sufficient privileges to access this page");
                        return;
                }
                Action_Type doaction = Util_getAction(action);
                if (doaction == Action_Ignored) {
                        send_error(req, res, SC_BAD_REQUEST, "Invalid action \"%s\"", action);
                        return;
                }
                s->doaction = doaction;
                const char *token = get_parameter(req, "token");
                if (token) {
                        FREE(s->token);
                        s->token = Str_dup(token);
                }
                LogInfo("'%s' %s on user request\n", s->name, action);
                Run.flags |= Run_ActionPending; /* set the global flag */
                do_wakeupcall();
        }
        do_service(req, res, s);
}


static void handle_doaction(HttpRequest req, HttpResponse res) {
        Service_T s;
        Action_Type doaction = Action_Ignored;
        const char *action = get_parameter(req, "action");
        const char *token = get_parameter(req, "token");
        if (action) {
                if (is_readonly(req)) {
                        send_error(req, res, SC_FORBIDDEN, "You do not have sufficient privileges to access this page");
                        return;
                }
                if ((doaction = Util_getAction(action)) == Action_Ignored) {
                        send_error(req, res, SC_BAD_REQUEST, "Invalid action \"%s\"", action);
                        return;
                }
                for (HttpParameter p = req->params; p; p = p->next) {
                        if (IS(p->name, "service")) {
                                s  = Util_getService(p->value);
                                if (! s) {
                                        send_error(req, res, SC_BAD_REQUEST, "There is no service named \"%s\"", p->value ? p->value : "");
                                        return;
                                }
                                s->doaction = doaction;
                                LogInfo("'%s' %s on user request\n", s->name, action);
                        }
                }
                /* Set token for last service only so we'll get it back after all services were handled */
                if (token) {
                        Service_T q = NULL;
                        for (s = servicelist; s; s = s->next)
                                if (s->doaction == doaction)
                                        q = s;
                        if (q) {
                                FREE(q->token);
                                q->token = Str_dup(token);
                        }
                }
                Run.flags |= Run_ActionPending;
                do_wakeupcall();
        }
}


static void handle_runtime(HttpRequest req, HttpResponse res) {
        LOCK(Run.mutex)
        do_runtime(req, res);
        END_LOCK;
}


static void handle_runtime_action(HttpRequest req, HttpResponse res) {
        const char *action = get_parameter(req, "action");
        if (action) {
                if (is_readonly(req)) {
                        send_error(req, res, SC_FORBIDDEN, "You do not have sufficient privileges to access this page");
                        return;
                }
                if (IS(action, "validate")) {
                        LogInfo("The Monit http server woke up on user request\n");
                        do_wakeupcall();
                } else if (IS(action, "stop")) {
                        LogInfo("The Monit http server stopped on user request\n");
                        send_error(req, res, SC_SERVICE_UNAVAILABLE, "The Monit http server is stopped");
                        Engine_stop();
                        return;
                }
        }
        handle_runtime(req, res);
}


static void do_service(HttpRequest req, HttpResponse res, Service_T s) {
        char buf[STRLEN];

        ASSERT(s);

        do_head(res, s->name, s->name, Run.polltime);
        StringBuffer_append(res->outputbuffer,
                            "<h2>%s status</h2>"
                            "<table id='status-table'>"
                            "<tr>"
                            "<th width='30%%'>Parameter</th>"
                            "<th width='70%%'>Value</th>"
                            "</tr>"
                            "<tr>"
                            "<td>Name</td>"
                            "<td>%s</td>"
                            "</tr>",
                            servicetypes[s->type],
                            s->name);
        if (s->type == Service_Process)
                StringBuffer_append(res->outputbuffer, "<tr><td>%s</td><td>%s</td></tr>", s->matchlist ? "Match" : "Pid file", s->path);
        else if (s->type == Service_Host)
                StringBuffer_append(res->outputbuffer, "<tr><td>Address</td><td>%s</td></tr>", s->path);
        else if (s->type == Service_Net)
                StringBuffer_append(res->outputbuffer, "<tr><td>Interface</td><td>%s</td></tr>", s->path);
        else if (s->type != Service_System)
                StringBuffer_append(res->outputbuffer, "<tr><td>Path</td><td>%s</td></tr>", s->path);
        StringBuffer_append(res->outputbuffer, "<tr><td>Status</td><td>%s</td></tr>", get_service_status(HTML, s, buf, sizeof(buf)));
        for (ServiceGroup_T sg = servicegrouplist; sg; sg = sg->next)
                for (list_t m = sg->members->head; m; m = m->next)
                        if (m->e == s)
                                StringBuffer_append(res->outputbuffer, "<tr><td>Group</td><td class='blue-text'>%s</td></tr>", sg->name);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Monitoring status</td><td>%s</td></tr>", get_monitoring_status(HTML, s, buf, sizeof(buf)));
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>Monitoring mode</td><td>%s</td></tr>", modenames[s->mode]);
        StringBuffer_append(res->outputbuffer,
                            "<tr><td>On reboot</td><td>%s</td></tr>", onrebootnames[s->onreboot]);
        for (Dependant_T d = s->dependantlist; d; d = d->next) {
                if (d->dependant != NULL) {
                        StringBuffer_append(res->outputbuffer,
                                            "<tr><td>Depends on service </td><td> <a href=%s> %s </a></td></tr>",
                                            d->dependant, d->dependant);
                }
        }
        if (s->start) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Start program</td><td>'%s'", Util_commandDescription(s->start, (char[STRLEN]){}));
                if (s->start->has_uid)
                        StringBuffer_append(res->outputbuffer, " as uid %d", s->start->uid);
                if (s->start->has_gid)
                        StringBuffer_append(res->outputbuffer, " as gid %d", s->start->gid);
                StringBuffer_append(res->outputbuffer, " timeout %s", Str_milliToTime(s->start->timeout, (char[23]){}));
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (s->stop) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Stop program</td><td>'%s'", Util_commandDescription(s->stop, (char[STRLEN]){}));
                if (s->stop->has_uid)
                        StringBuffer_append(res->outputbuffer, " as uid %d", s->stop->uid);
                if (s->stop->has_gid)
                        StringBuffer_append(res->outputbuffer, " as gid %d", s->stop->gid);
                StringBuffer_append(res->outputbuffer, " timeout %s", Str_milliToTime(s->stop->timeout, (char[23]){}));
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (s->restart) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Restart program</td><td>'%s'", Util_commandDescription(s->restart, (char[STRLEN]){}));
                if (s->restart->has_uid)
                        StringBuffer_append(res->outputbuffer, " as uid %d", s->restart->uid);
                if (s->restart->has_gid)
                        StringBuffer_append(res->outputbuffer, " as gid %d", s->restart->gid);
                StringBuffer_append(res->outputbuffer, " timeout %s", Str_milliToTime(s->restart->timeout, (char[23]){}));
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        if (s->every.type != Every_Cycle) {
                StringBuffer_append(res->outputbuffer, "<tr><td>Check service</td><td>");
                if (s->every.type == Every_SkipCycles)
                        StringBuffer_append(res->outputbuffer, "every %d cycle", s->every.spec.cycle.number);
                else if (s->every.type == Every_Cron)
                        StringBuffer_append(res->outputbuffer, "every <code>\"%s\"</code>", s->every.spec.cron);
                else if (s->every.type == Every_NotInCron)
                        StringBuffer_append(res->outputbuffer, "not every <code>\"%s\"</code>", s->every.spec.cron);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
        _printStatus(HTML, res, s);
        // Rules
        print_service_rules_timeout(res, s);
        print_service_rules_existence(res, s);
        print_service_rules_icmp(res, s);
        print_service_rules_port(res, s);
        print_service_rules_socket(res, s);
        print_service_rules_perm(res, s);
        print_service_rules_uid(res, s);
        print_service_rules_euid(res, s);
        print_service_rules_gid(res, s);
        print_service_rules_timestamp(res, s);
        print_service_rules_fsflags(res, s);
        print_service_rules_filesystem(res, s);
        print_service_rules_size(res, s);
        print_service_rules_linkstatus(res, s);
        print_service_rules_linkspeed(res, s);
        print_service_rules_linksaturation(res, s);
        print_service_rules_uploadbytes(res, s);
        print_service_rules_uploadpackets(res, s);
        print_service_rules_downloadbytes(res, s);
        print_service_rules_downloadpackets(res, s);
        print_service_rules_uptime(res, s);
        print_service_rules_content(res, s);
        print_service_rules_checksum(res, s);
        print_service_rules_pid(res, s);
        print_service_rules_ppid(res, s);
        print_service_rules_program(res, s);
        print_service_rules_resource(res, s);
        print_alerts(res, s->maillist);
        StringBuffer_append(res->outputbuffer, "</table>");
        print_buttons(req, res, s);
        do_foot(res);
}


static void do_home_system(HttpResponse res) {
        Service_T s = Run.system;
        char buf[STRLEN];

        StringBuffer_append(res->outputbuffer,
                            "<table id='header-row'>"
                            "<tr>"
                            "<th align='left' class='first'>System</th>"
                            "<th align='left'>Status</th>");

        if (Run.flags & Run_ProcessEngineEnabled) {
                StringBuffer_append(res->outputbuffer,
                                    "<th align='right'>Load</th>"
                                    "<th align='right'>CPU</th>"
                                    "<th align='right'>Memory</th>"
                                    "<th align='right'>Swap</th>");
        }
        StringBuffer_append(res->outputbuffer,
                            "</tr>"
                            "<tr class='stripe'>"
                            "<td align='left'><a href='%s'>%s</a></td>"
                            "<td align='left'>%s</td>",
                            s->name, s->name,
                            get_service_status(HTML, s, buf, sizeof(buf)));
        if (Run.flags & Run_ProcessEngineEnabled) {
                StringBuffer_append(res->outputbuffer,
                                    "<td align='right'>[%.2f]&nbsp;[%.2f]&nbsp;[%.2f]</td>"
                                    "<td align='right'>"
                                    "%.1f%%us,&nbsp;%.1f%%sy"
#ifdef HAVE_CPU_WAIT
                                    ",&nbsp;%.1f%%wa"
#endif
                                    "</td>",
                                    systeminfo.loadavg[0], systeminfo.loadavg[1], systeminfo.loadavg[2],
                                    systeminfo.total_cpu_user_percent > 0. ? systeminfo.total_cpu_user_percent : 0.,
                                    systeminfo.total_cpu_syst_percent > 0. ? systeminfo.total_cpu_syst_percent : 0.
#ifdef HAVE_CPU_WAIT
                                    , systeminfo.total_cpu_wait_percent > 0. ? systeminfo.total_cpu_wait_percent : 0.
#endif
                                    );
                StringBuffer_append(res->outputbuffer,
                                    "<td align='right'>%.1f%% [%s]</td>",
                                    systeminfo.total_mem_percent, Str_bytesToSize(systeminfo.total_mem, buf));
                StringBuffer_append(res->outputbuffer,
                                    "<td align='right'>%.1f%% [%s]</td>",
                                    systeminfo.total_swap_percent, Str_bytesToSize(systeminfo.total_swap, buf));
        }
        StringBuffer_append(res->outputbuffer,
                            "</tr>"
                            "</table>");
}


static void do_home_process(HttpResponse res) {
        char      buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Process)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Process</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Uptime</th>");
                        if (Run.flags & Run_ProcessEngineEnabled) {
                                StringBuffer_append(res->outputbuffer,
                                                    "<th align='right'>CPU Total</b></th>"
                                                    "<th align='right'>Memory Total</th>");
                        }
                        StringBuffer_append(res->outputbuffer, "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s) || s->inf->priv.process.uptime < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%s</td>", _getUptime(s->inf->priv.process.uptime, (char[256]){}));
                if (Run.flags & Run_ProcessEngineEnabled) {
                        if (! Util_hasServiceStatus(s) || s->inf->priv.process.total_cpu_percent < 0)
                                StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                        else
                                StringBuffer_append(res->outputbuffer, "<td align='right' class='%s'>%.1f%%</td>", (s->error & Event_Resource) ? "red-text" : "", s->inf->priv.process.total_cpu_percent);
                        if (! Util_hasServiceStatus(s) || s->inf->priv.process.total_mem_percent < 0)
                                StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                        else
                                StringBuffer_append(res->outputbuffer, "<td align='right' class='%s'>%.1f%% [%s]</td>", (s->error & Event_Resource) ? "red-text" : "", s->inf->priv.process.total_mem_percent, Str_bytesToSize(s->inf->priv.process.total_mem, buf));
                }
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


static void do_home_program(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Program)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Program</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='left'>Output</th>"
                                            "<th align='right'>Last started</th>"
                                            "<th align='right'>Exit value</th>"
                                            "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s)) {
                        StringBuffer_append(res->outputbuffer, "<td align='left'>-</td>");
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                } else {
                        if (s->program->started) {
                                StringBuffer_append(res->outputbuffer, "<td align='left' class='short'>");
                                if (StringBuffer_length(s->program->output)) {
                                        // Print first line only (escape HTML characters if any)
                                        const char *output = StringBuffer_toString(s->program->output);
                                        for (int i = 0; output[i]; i++) {
                                                if (output[i] == '<')
                                                        StringBuffer_append(res->outputbuffer, "&lt;");
                                                else if (output[i] == '>')
                                                        StringBuffer_append(res->outputbuffer, "&gt;");
                                                else if (output[i] == '&')
                                                        StringBuffer_append(res->outputbuffer, "&amp;");
                                                else if (output[i] == '\r' || output[i] == '\n')
                                                        break;
                                                else
                                                        StringBuffer_append(res->outputbuffer, "%c", output[i]);
                                        }
                                } else {
                                        StringBuffer_append(res->outputbuffer, "no output");
                                }
                                StringBuffer_append(res->outputbuffer, "</td>");
                                StringBuffer_append(res->outputbuffer, "<td align='right'>%s</td>", Time_fmt((char[32]){}, 32, "%d %b %Y %H:%M:%S", s->program->started));
                                StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->program->exitStatus);
                        } else {
                                StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                                StringBuffer_append(res->outputbuffer, "<td align='right'>Not yet started</td>");
                                StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                        }
                }
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");

}


static void do_home_net(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Net)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Net</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Upload</th>"
                                            "<th align='right'>Download</th>"
                                            "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s) || Link_getState(s->inf->priv.net.stats) != 1) {
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                } else {
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%s&#47;s</td>", Str_bytesToSize(Link_getBytesOutPerSecond(s->inf->priv.net.stats), buf));
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%s&#47;s</td>", Str_bytesToSize(Link_getBytesInPerSecond(s->inf->priv.net.stats), buf));
                }
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


static void do_home_filesystem(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Filesystem)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Filesystem</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Space usage</th>"
                                            "<th align='right'>Inodes usage</th>"
                                            "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s)) {
                        StringBuffer_append(res->outputbuffer,
                                            "<td align='right'>- [-]</td>"
                                            "<td align='right'>- [-]</td>");
                } else {
                        StringBuffer_append(res->outputbuffer,
                                            "<td align='right'>%.1f%% [%s]</td>",
                                            s->inf->priv.filesystem.space_percent,
                                            s->inf->priv.filesystem.f_bsize > 0 ? Str_bytesToSize(s->inf->priv.filesystem.space_total * s->inf->priv.filesystem.f_bsize, buf) : "0 MB");
                        if (s->inf->priv.filesystem.f_files > 0) {
                                StringBuffer_append(res->outputbuffer,
                                                    "<td align='right'>%.1f%% [%lld objects]</td>",
                                                    s->inf->priv.filesystem.inode_percent,
                                                    s->inf->priv.filesystem.inode_total);
                        } else {
                                StringBuffer_append(res->outputbuffer,
                                                    "<td align='right'>not supported by filesystem</td>");
                        }
                }
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


static void do_home_file(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_File)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>File</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Size</th>"
                                            "<th align='right'>Permission</th>"
                                            "<th align='right'>UID</th>"
                                            "<th align='right'>GID</th>"
                                            "</tr>");

                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s) || s->inf->priv.file.size < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%s</td>", Str_bytesToSize(s->inf->priv.file.size, (char[10]){}));
                if (! Util_hasServiceStatus(s) || s->inf->priv.file.mode < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%04o</td>", s->inf->priv.file.mode & 07777);
                if (! Util_hasServiceStatus(s) || s->inf->priv.file.uid < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->inf->priv.file.uid);
                if (! Util_hasServiceStatus(s) || s->inf->priv.file.gid < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->inf->priv.file.gid);
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


static void do_home_fifo(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Fifo)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Fifo</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Permission</th>"
                                            "<th align='right'>UID</th>"
                                            "<th align='right'>GID</th>"
                                            "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s) || s->inf->priv.fifo.mode < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%04o</td>", s->inf->priv.fifo.mode & 07777);
                if (! Util_hasServiceStatus(s) || s->inf->priv.fifo.uid < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->inf->priv.fifo.uid);
                if (! Util_hasServiceStatus(s) || s->inf->priv.fifo.gid < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->inf->priv.fifo.gid);
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


static void do_home_directory(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Directory)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Directory</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Permission</th>"
                                            "<th align='right'>UID</th>"
                                            "<th align='right'>GID</th>"
                                            "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s) || s->inf->priv.directory.mode < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%04o</td>", s->inf->priv.directory.mode & 07777);
                if (! Util_hasServiceStatus(s) || s->inf->priv.directory.uid < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->inf->priv.directory.uid);
                if (! Util_hasServiceStatus(s) || s->inf->priv.directory.gid < 0)
                        StringBuffer_append(res->outputbuffer, "<td align='right'>-</td>");
                else
                        StringBuffer_append(res->outputbuffer, "<td align='right'>%d</td>", s->inf->priv.directory.gid);
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


static void do_home_host(HttpResponse res) {
        char buf[STRLEN];
        boolean_t on = true;
        boolean_t header = true;

        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type != Service_Host)
                        continue;
                if (header) {
                        StringBuffer_append(res->outputbuffer,
                                            "<table id='header-row'>"
                                            "<tr>"
                                            "<th align='left' class='first'>Host</th>"
                                            "<th align='left'>Status</th>"
                                            "<th align='right'>Protocol(s)</th>"
                                            "</tr>");
                        header = false;
                }
                StringBuffer_append(res->outputbuffer,
                                    "<tr %s>"
                                    "<td align='left'><a href='%s'>%s</a></td>"
                                    "<td align='left'>%s</td>",
                                    on ? "class='stripe'" : "",
                                    s->name, s->name,
                                    get_service_status(HTML, s, buf, sizeof(buf)));
                if (! Util_hasServiceStatus(s)) {
                        StringBuffer_append(res->outputbuffer,
                                            "<td align='right'>-</td>");
                } else {
                        StringBuffer_append(res->outputbuffer,
                                            "<td align='right'>");
                        for (Icmp_T icmp = s->icmplist; icmp; icmp = icmp->next) {
                                if (icmp != s->icmplist)
                                        StringBuffer_append(res->outputbuffer, "&nbsp;&nbsp;<b>|</b>&nbsp;&nbsp;");
                                switch (icmp->is_available) {
                                        case Connection_Init:
                                                StringBuffer_append(res->outputbuffer, "<span class='gray-text'>[Ping]</span>");
                                                break;
                                        case Connection_Failed:
                                                StringBuffer_append(res->outputbuffer, "<span class='red-text'>[Ping]</span>");
                                                break;
                                        default:
                                                StringBuffer_append(res->outputbuffer, "<span>[Ping]</span>");
                                                break;
                                }
                        }
                        if (s->icmplist && s->portlist)
                                StringBuffer_append(res->outputbuffer, "&nbsp;&nbsp;<b>|</b>&nbsp;&nbsp;");
                        for (Port_T port = s->portlist; port; port = port->next) {
                                if (port != s->portlist)
                                        StringBuffer_append(res->outputbuffer, "&nbsp;&nbsp;<b>|</b>&nbsp;&nbsp;");
                                switch (port->is_available) {
                                        case Connection_Init:
                                                StringBuffer_append(res->outputbuffer, "<span class='gray-text'>[%s] at port %d</span>", port->protocol->name, port->target.net.port);
                                                break;
                                        case Connection_Failed:
                                                StringBuffer_append(res->outputbuffer, "<span class='red-text'>[%s] at port %d</span>", port->protocol->name, port->target.net.port);
                                                break;
                                        default:
                                                StringBuffer_append(res->outputbuffer, "<span>[%s] at port %d</span>", port->protocol->name, port->target.net.port);
                                                break;
                                }
                        }
                        StringBuffer_append(res->outputbuffer, "</td>");
                }
                StringBuffer_append(res->outputbuffer, "</tr>");
                on = ! on;
        }
        if (! header)
                StringBuffer_append(res->outputbuffer, "</table>");
}


/* ------------------------------------------------------------------------- */


static void print_alerts(HttpResponse res, Mail_T s) {
        for (Mail_T r = s; r; r = r->next) {
                StringBuffer_append(res->outputbuffer,
                                    "<tr class='stripe'><td>Alert mail to</td>"
                                    "<td>%s</td></tr>", r->to ? r->to : "");
                StringBuffer_append(res->outputbuffer, "<tr><td>Alert on</td><td>");
                if (r->events == Event_Null) {
                        StringBuffer_append(res->outputbuffer, "No events");
                } else if (r->events == Event_All) {
                        StringBuffer_append(res->outputbuffer, "All events");
                } else {
                        if (IS_EVENT_SET(r->events, Event_Action))
                                StringBuffer_append(res->outputbuffer, "Action ");
                        if (IS_EVENT_SET(r->events, Event_ByteIn))
                                StringBuffer_append(res->outputbuffer, "ByteIn ");
                        if (IS_EVENT_SET(r->events, Event_ByteOut))
                                StringBuffer_append(res->outputbuffer, "ByteOut ");
                        if (IS_EVENT_SET(r->events, Event_Checksum))
                                StringBuffer_append(res->outputbuffer, "Checksum ");
                        if (IS_EVENT_SET(r->events, Event_Connection))
                                StringBuffer_append(res->outputbuffer, "Connection ");
                        if (IS_EVENT_SET(r->events, Event_Content))
                                StringBuffer_append(res->outputbuffer, "Content ");
                        if (IS_EVENT_SET(r->events, Event_Data))
                                StringBuffer_append(res->outputbuffer, "Data ");
                        if (IS_EVENT_SET(r->events, Event_Exec))
                                StringBuffer_append(res->outputbuffer, "Exec ");
                        if (IS_EVENT_SET(r->events, Event_Fsflag))
                                StringBuffer_append(res->outputbuffer, "Fsflags ");
                        if (IS_EVENT_SET(r->events, Event_Gid))
                                StringBuffer_append(res->outputbuffer, "Gid ");
                        if (IS_EVENT_SET(r->events, Event_Instance))
                                StringBuffer_append(res->outputbuffer, "Instance ");
                        if (IS_EVENT_SET(r->events, Event_Invalid))
                                StringBuffer_append(res->outputbuffer, "Invalid ");
                        if (IS_EVENT_SET(r->events, Event_Link))
                                StringBuffer_append(res->outputbuffer, "Link ");
                        if (IS_EVENT_SET(r->events, Event_Nonexist))
                                StringBuffer_append(res->outputbuffer, "Nonexist ");
                        if (IS_EVENT_SET(r->events, Event_Permission))
                                StringBuffer_append(res->outputbuffer, "Permission ");
                        if (IS_EVENT_SET(r->events, Event_PacketIn))
                                StringBuffer_append(res->outputbuffer, "PacketIn ");
                        if (IS_EVENT_SET(r->events, Event_PacketOut))
                                StringBuffer_append(res->outputbuffer, "PacketOut ");
                        if (IS_EVENT_SET(r->events, Event_Pid))
                                StringBuffer_append(res->outputbuffer, "PID ");
                        if (IS_EVENT_SET(r->events, Event_Icmp))
                                StringBuffer_append(res->outputbuffer, "Ping ");
                        if (IS_EVENT_SET(r->events, Event_PPid))
                                StringBuffer_append(res->outputbuffer, "PPID ");
                        if (IS_EVENT_SET(r->events, Event_Resource))
                                StringBuffer_append(res->outputbuffer, "Resource ");
                        if (IS_EVENT_SET(r->events, Event_Saturation))
                                StringBuffer_append(res->outputbuffer, "Saturation ");
                        if (IS_EVENT_SET(r->events, Event_Size))
                                StringBuffer_append(res->outputbuffer, "Size ");
                        if (IS_EVENT_SET(r->events, Event_Speed))
                                StringBuffer_append(res->outputbuffer, "Speed ");
                        if (IS_EVENT_SET(r->events, Event_Status))
                                StringBuffer_append(res->outputbuffer, "Status ");
                        if (IS_EVENT_SET(r->events, Event_Timeout))
                                StringBuffer_append(res->outputbuffer, "Timeout ");
                        if (IS_EVENT_SET(r->events, Event_Timestamp))
                                StringBuffer_append(res->outputbuffer, "Timestamp ");
                        if (IS_EVENT_SET(r->events, Event_Uid))
                                StringBuffer_append(res->outputbuffer, "Uid ");
                        if (IS_EVENT_SET(r->events, Event_Uptime))
                                StringBuffer_append(res->outputbuffer, "Uptime ");
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
                if (r->reminder) {
                        StringBuffer_append(res->outputbuffer,
                                            "<tr><td>Alert reminder</td><td>%u cycles</td></tr>",
                                            r->reminder);
                }
        }
}


static void print_buttons(HttpRequest req, HttpResponse res, Service_T s) {
        if (is_readonly(req)) {
                 // A read-only REMOTE_USER does not get access to these buttons
                return;
        }
        StringBuffer_append(res->outputbuffer, "<table id='buttons'><tr>");
        /* Start program */
        if (s->start)
                StringBuffer_append(res->outputbuffer,
                                    "<td>"
                                    "<form method=POST action=%s>"
                                    "<input type=hidden name='securitytoken' value='%s'>"
                                    "<input type=hidden value='start' name=action>"
                                    "<input type=submit value='Start service'>"
                                    "</form>"
                                    "</td>", s->name, res->token);
        /* Stop program */
        if (s->stop)
                StringBuffer_append(res->outputbuffer,
                                    "<td>"
                                    "<form method=POST action=%s>"
                                    "<input type=hidden name='securitytoken' value='%s'>"
                                    "<input type=hidden value='stop' name=action>"
                                    "<input type=submit value='Stop service'>"
                                    "</form>"
                                    "</td>", s->name, res->token);
        /* Restart program */
        if ((s->start && s->stop) || s->restart)
                StringBuffer_append(res->outputbuffer,
                                    "<td>"
                                    "<form method=POST action=%s>"
                                    "<input type=hidden name='securitytoken' value='%s'>"
                                    "<input type=hidden value='restart' name=action>"
                                    "<input type=submit value='Restart service'>"
                                    "</form>"
                                    "</td>", s->name, res->token);
        /* (un)monitor */
        StringBuffer_append(res->outputbuffer,
                                    "<td>"
                                    "<form method=POST action=%s>"
                                    "<input type=hidden name='securitytoken' value='%s'>"
                                    "<input type=hidden value='%s' name=action>"
                                    "<input type=submit value='%s'>"
                                    "</form>"
                                    "</td>",
                                    s->name,
                                    res->token,
                                    s->monitor ? "unmonitor" : "monitor",
                                    s->monitor ? "Disable monitoring" : "Enable monitoring");
        StringBuffer_append(res->outputbuffer, "</tr></table>");
}


static void print_service_rules_timeout(HttpResponse res, Service_T s) {
        for (ActionRate_T ar = s->actionratelist; ar; ar = ar->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Timeout</td><td>If restarted %d times within %d cycle(s) then ", ar->count, ar->cycle);
                Util_printAction(ar->action->failed, res->outputbuffer);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_existence(HttpResponse res, Service_T s) {
        for (Nonexist_T l = s->nonexistlist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Existence</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If doesn't exist");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_port(HttpResponse res, Service_T s) {
        for (Port_T p = s->portlist; p; p = p->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Port</td><td>");
                StringBuffer_T buf = StringBuffer_create(64);
                StringBuffer_append(buf, "If failed [%s]:%d%s",
                        p->hostname, p->target.net.port, Util_portRequestDescription(p));
                if (p->outgoing.ip)
                        StringBuffer_append(buf, " via address %s", p->outgoing.ip);
                StringBuffer_append(buf, " type %s/%s protocol %s with timeout %s",
                        Util_portTypeDescription(p), Util_portIpDescription(p), p->protocol->name, Str_milliToTime(p->timeout, (char[23]){}));
                if (p->retry > 1)
                        StringBuffer_append(buf, " and retry %d times", p->retry);
#ifdef HAVE_OPENSSL
                if (p->target.net.ssl.flags) {
                        StringBuffer_append(buf, " using SSL/TLS");
                        const char *options = Ssl_printOptions(&p->target.net.ssl, (char[STRLEN]){}, STRLEN);
                        if (options && *options)
                                StringBuffer_append(buf, " with options {%s}", options);
                        if (p->target.net.ssl.minimumValidDays > 0)
                                StringBuffer_append(buf, " and certificate expires in more than %d days", p->target.net.ssl.minimumValidDays);
                        if (p->target.net.ssl.checksum)
                                StringBuffer_append(buf, " and certificate checksum %s equal to '%s'", checksumnames[p->target.net.ssl.checksumType], p->target.net.ssl.checksum);
                }
#endif
                Util_printRule(res->outputbuffer, p->action, "%s", StringBuffer_toString(buf));
                StringBuffer_free(&buf);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_socket(HttpResponse res, Service_T s) {
        for (Port_T p = s->socketlist; p; p = p->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Unix Socket</td><td>");
                if (p->retry > 1)
                        Util_printRule(res->outputbuffer, p->action, "If failed %s type %s protocol %s with timeout %s and retry %d time(s)", p->target.unix.pathname, Util_portTypeDescription(p), p->protocol->name, Str_milliToTime(p->timeout, (char[23]){}), p->retry);
                else
                        Util_printRule(res->outputbuffer, p->action, "If failed %s type %s protocol %s with timeout %s", p->target.unix.pathname, Util_portTypeDescription(p), p->protocol->name, Str_milliToTime(p->timeout, (char[23]){}));
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_icmp(HttpResponse res, Service_T s) {
        for (Icmp_T i = s->icmplist; i; i = i->next) {
                switch (i->family) {
                        case Socket_Ip4:
                                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Ping4</td><td>");
                                break;
                        case Socket_Ip6:
                                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Ping6</td><td>");
                                break;
                        default:
                                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Ping</td><td>");
                                break;
                }
                Util_printRule(res->outputbuffer, i->action, "If failed [count %d size %d with timeout %s%s%s]", i->count, i->size, Str_milliToTime(i->timeout, (char[23]){}), i->outgoing.ip ? " via address " : "", i->outgoing.ip ? i->outgoing.ip : "");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_perm(HttpResponse res, Service_T s) {
        if (s->perm) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Permissions</td><td>");
                if (s->perm->test_changes)
                        Util_printRule(res->outputbuffer, s->perm->action, "If changed");
                else
                        Util_printRule(res->outputbuffer, s->perm->action, "If failed %o", s->perm->perm);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_uid(HttpResponse res, Service_T s) {
        if (s->uid) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>UID</td><td>");
                Util_printRule(res->outputbuffer, s->uid->action, "If failed %d", s->uid->uid);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_euid(HttpResponse res, Service_T s) {
        if (s->euid) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>EUID</td><td>");
                Util_printRule(res->outputbuffer, s->euid->action, "If failed %d", s->euid->uid);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_gid(HttpResponse res, Service_T s) {
        if (s->gid) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>GID</td><td>");
                Util_printRule(res->outputbuffer, s->gid->action, "If failed %d", s->gid->gid);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_timestamp(HttpResponse res, Service_T s) {
        for (Timestamp_T t = s->timestamplist; t; t = t->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Timestamp</td><td>");
                if (t->test_changes)
                        Util_printRule(res->outputbuffer, t->action, "If changed");
                else
                        Util_printRule(res->outputbuffer, t->action, "If %s %d second(s)", operatornames[t->operator], t->time);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_fsflags(HttpResponse res, Service_T s) {
        for (Fsflag_T l = s->fsflaglist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Filesystem flags</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If changed");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_filesystem(HttpResponse res, Service_T s) {
        for (Filesystem_T dl = s->filesystemlist; dl; dl = dl->next) {
                if (dl->resource == Resource_Inode) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Inodes usage limit</td><td>");
                        if (dl->limit_absolute > -1)
                                Util_printRule(res->outputbuffer, dl->action, "If %s %lld", operatornames[dl->operator], dl->limit_absolute);
                        else
                                Util_printRule(res->outputbuffer, dl->action, "If %s %.1f%%", operatornames[dl->operator], dl->limit_percent);
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                } else if (dl->resource == Resource_InodeFree) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Inodes free limit</td><td>");
                        if (dl->limit_absolute > -1)
                                Util_printRule(res->outputbuffer, dl->action, "If %s %lld", operatornames[dl->operator], dl->limit_absolute);
                        else
                                Util_printRule(res->outputbuffer, dl->action, "If %s %.1f%%", operatornames[dl->operator], dl->limit_percent);
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                } else if (dl->resource == Resource_Space) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Space usage limit</td><td>");
                        if (dl->limit_absolute > -1) {
                                if (s->inf->priv.filesystem.f_bsize > 0)
                                        Util_printRule(res->outputbuffer, dl->action, "If %s %s", operatornames[dl->operator], Str_bytesToSize(dl->limit_absolute * s->inf->priv.filesystem.f_bsize, (char[10]){}));
                                else
                                        Util_printRule(res->outputbuffer, dl->action, "If %s %lld blocks", operatornames[dl->operator], dl->limit_absolute);
                        } else {
                                Util_printRule(res->outputbuffer, dl->action, "If %s %.1f%%", operatornames[dl->operator], dl->limit_percent);
                        }
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                } else if (dl->resource == Resource_SpaceFree) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Space free limit</td><td>");
                        if (dl->limit_absolute > -1) {
                                if (s->inf->priv.filesystem.f_bsize > 0)
                                        Util_printRule(res->outputbuffer, dl->action, "If %s %s", operatornames[dl->operator], Str_bytesToSize(dl->limit_absolute * s->inf->priv.filesystem.f_bsize, (char[10]){}));
                                else
                                        Util_printRule(res->outputbuffer, dl->action, "If %s %lld blocks", operatornames[dl->operator], dl->limit_absolute);
                        } else {
                                Util_printRule(res->outputbuffer, dl->action, "If %s %.1f%%", operatornames[dl->operator], dl->limit_percent);
                        }
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                }
        }
}


static void print_service_rules_size(HttpResponse res, Service_T s) {
        for (Size_T sl = s->sizelist; sl; sl = sl->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Size</td><td>");
                if (sl->test_changes)
                        Util_printRule(res->outputbuffer, sl->action, "If changed");
                else
                        Util_printRule(res->outputbuffer, sl->action, "If %s %llu byte(s)", operatornames[sl->operator], sl->size);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_linkstatus(HttpResponse res, Service_T s) {
        for (LinkStatus_T l = s->linkstatuslist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Link status</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If failed");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_linkspeed(HttpResponse res, Service_T s) {
        for (LinkSpeed_T l = s->linkspeedlist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Link capacity</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If changed");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_linksaturation(HttpResponse res, Service_T s) {
        for (LinkSaturation_T l = s->linksaturationlist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Link saturation</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If %s %.1f%%", operatornames[l->operator], l->limit);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_uploadbytes(HttpResponse res, Service_T s) {
        for (Bandwidth_T bl = s->uploadbyteslist; bl; bl = bl->next) {
                if (bl->range == Time_Second) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Upload bytes</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %s/s", operatornames[bl->operator], Str_bytesToSize(bl->limit, (char[10]){}));
                } else {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Total upload bytes</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %s in last %d %s(s)", operatornames[bl->operator], Str_bytesToSize(bl->limit, (char[10]){}), bl->rangecount, Util_timestr(bl->range));
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_uploadpackets(HttpResponse res, Service_T s) {
        for (Bandwidth_T bl = s->uploadpacketslist; bl; bl = bl->next) {
                if (bl->range == Time_Second) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Upload packets</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %lld packets/s", operatornames[bl->operator], bl->limit);
                } else {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Total upload packets</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %lld packets in last %d %s(s)", operatornames[bl->operator], bl->limit, bl->rangecount, Util_timestr(bl->range));
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_downloadbytes(HttpResponse res, Service_T s) {
        for (Bandwidth_T bl = s->downloadbyteslist; bl; bl = bl->next) {
                if (bl->range == Time_Second) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Download bytes</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %s/s", operatornames[bl->operator], Str_bytesToSize(bl->limit, (char[10]){}));
                } else {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Total download bytes</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %s in last %d %s(s)", operatornames[bl->operator], Str_bytesToSize(bl->limit, (char[10]){}), bl->rangecount, Util_timestr(bl->range));
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_downloadpackets(HttpResponse res, Service_T s) {
        for (Bandwidth_T bl = s->downloadpacketslist; bl; bl = bl->next) {
                if (bl->range == Time_Second) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Download packets</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %lld packets/s", operatornames[bl->operator], bl->limit);
                } else {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Total download packets</td><td>");
                        Util_printRule(res->outputbuffer, bl->action, "If %s %lld packets in last %d %s(s)", operatornames[bl->operator], bl->limit, bl->rangecount, Util_timestr(bl->range));
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_uptime(HttpResponse res, Service_T s) {
        for (Uptime_T ul = s->uptimelist; ul; ul = ul->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Uptime</td><td>");
                Util_printRule(res->outputbuffer, ul->action, "If %s %s", operatornames[ul->operator], _getUptime(ul->uptime, (char[256]){}));
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}

static void print_service_rules_content(HttpResponse res, Service_T s) {
        if (s->type != Service_Process) {
                for (Match_T ml = s->matchignorelist; ml; ml = ml->next) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Ignore content</td><td>");
                        Util_printRule(res->outputbuffer, ml->action, "If content %s \"%s\"", ml->not ? "!=" : "=", ml->match_string);
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                }
                for (Match_T ml = s->matchlist; ml; ml = ml->next) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Content match</td><td>");
                        Util_printRule(res->outputbuffer, ml->action, "If content %s \"%s\"", ml->not ? "!=" : "=", ml->match_string);
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                }
        }
}


static void print_service_rules_checksum(HttpResponse res, Service_T s) {
        if (s->checksum) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Checksum</td><td>");
                if (s->checksum->test_changes)
                        Util_printRule(res->outputbuffer, s->checksum->action, "If changed %s", checksumnames[s->checksum->type]);
                else
                        Util_printRule(res->outputbuffer, s->checksum->action, "If failed %s(%s)", s->checksum->hash, checksumnames[s->checksum->type]);
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_pid(HttpResponse res, Service_T s) {
        for (Pid_T l = s->pidlist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>PID</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If changed");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_ppid(HttpResponse res, Service_T s) {
        for (Pid_T l = s->ppidlist; l; l = l->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>PPID</td><td>");
                Util_printRule(res->outputbuffer, l->action, "If changed");
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static void print_service_rules_program(HttpResponse res, Service_T s) {
        if (s->type == Service_Program) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Program timeout</td><td>Terminate the program if not finished within %s</td></tr>", Str_milliToTime(s->program->timeout, (char[23]){}));
                for (Status_T status = s->statuslist; status; status = status->next) {
                        StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>Test Exit value</td><td>");
                        if (status->operator == Operator_Changed)
                                Util_printRule(res->outputbuffer, status->action, "If exit value changed");
                        else
                                Util_printRule(res->outputbuffer, status->action, "If exit value %s %d", operatorshortnames[status->operator], status->return_value);
                        StringBuffer_append(res->outputbuffer, "</td></tr>");
                }
        }
}


static void print_service_rules_resource(HttpResponse res, Service_T s) {
        char buf[STRLEN];
        for (Resource_T q = s->resourcelist; q; q = q->next) {
                StringBuffer_append(res->outputbuffer, "<tr class='rule'><td>");
                switch (q->resource_id) {
                        case Resource_CpuPercent:
                                StringBuffer_append(res->outputbuffer, "CPU usage limit");
                                break;

                        case Resource_CpuPercentTotal:
                                StringBuffer_append(res->outputbuffer, "CPU usage limit (incl. children)");
                                break;

                        case Resource_CpuUser:
                                StringBuffer_append(res->outputbuffer, "CPU user limit");
                                break;

                        case Resource_CpuSystem:
                                StringBuffer_append(res->outputbuffer, "CPU system limit");
                                break;

                        case Resource_CpuWait:
                                StringBuffer_append(res->outputbuffer, "CPU wait limit");
                                break;

                        case Resource_MemoryPercent:
                                StringBuffer_append(res->outputbuffer, "Memory usage limit");
                                break;

                        case Resource_MemoryKbyte:
                                StringBuffer_append(res->outputbuffer, "Memory amount limit");
                                break;

                        case Resource_SwapPercent:
                                StringBuffer_append(res->outputbuffer, "Swap usage limit");
                                break;

                        case Resource_SwapKbyte:
                                StringBuffer_append(res->outputbuffer, "Swap amount limit");
                                break;

                        case Resource_LoadAverage1m:
                                StringBuffer_append(res->outputbuffer, "Load average (1min)");
                                break;

                        case Resource_LoadAverage5m:
                                StringBuffer_append(res->outputbuffer, "Load average (5min)");
                                break;

                        case Resource_LoadAverage15m:
                                StringBuffer_append(res->outputbuffer, "Load average (15min)");
                                break;

                        case Resource_Threads:
                                StringBuffer_append(res->outputbuffer, "Threads");
                                break;

                        case Resource_Children:
                                StringBuffer_append(res->outputbuffer, "Children");
                                break;

                        case Resource_MemoryKbyteTotal:
                                StringBuffer_append(res->outputbuffer, "Memory amount limit (incl. children)");
                                break;

                        case Resource_MemoryPercentTotal:
                                StringBuffer_append(res->outputbuffer, "Memory usage limit (incl. children)");
                                break;
                        default:
                                break;
                }
                StringBuffer_append(res->outputbuffer, "</td><td>");
                switch (q->resource_id) {
                        case Resource_CpuPercent:
                        case Resource_CpuPercentTotal:
                        case Resource_MemoryPercentTotal:
                        case Resource_CpuUser:
                        case Resource_CpuSystem:
                        case Resource_CpuWait:
                        case Resource_MemoryPercent:
                        case Resource_SwapPercent:
                                Util_printRule(res->outputbuffer, q->action, "If %s %.1f%%", operatornames[q->operator], q->limit);
                                break;

                        case Resource_MemoryKbyte:
                        case Resource_SwapKbyte:
                        case Resource_MemoryKbyteTotal:
                                Util_printRule(res->outputbuffer, q->action, "If %s %s", operatornames[q->operator], Str_bytesToSize(q->limit, buf));
                                break;

                        case Resource_LoadAverage1m:
                        case Resource_LoadAverage5m:
                        case Resource_LoadAverage15m:
                                Util_printRule(res->outputbuffer, q->action, "If %s %.1f", operatornames[q->operator], q->limit);
                                break;

                        case Resource_Threads:
                        case Resource_Children:
                                Util_printRule(res->outputbuffer, q->action, "If %s %.0f", operatornames[q->operator], q->limit);
                                break;
                        default:
                                break;
                }
                StringBuffer_append(res->outputbuffer, "</td></tr>");
        }
}


static boolean_t is_readonly(HttpRequest req) {
        if (req->remote_user) {
                Auth_T user_creds = Util_getUserCredentials(req->remote_user);
                return (user_creds ? user_creds->is_readonly : true);
        }
        return false;
}


/* ----------------------------------------------------------- Status output */


/* Print status in the given format. Text status is default. */
static void print_status(HttpRequest req, HttpResponse res, int version) {
        const char *stringFormat = get_parameter(req, "format");
        if (stringFormat && Str_startsWith(stringFormat, "xml")) {
                char buf[STRLEN];
                StringBuffer_T sb = StringBuffer_create(256);
                status_xml(sb, NULL, version, Socket_getLocalHost(req->S, buf, sizeof(buf)));
                StringBuffer_append(res->outputbuffer, "%s", StringBuffer_toString(sb));
                StringBuffer_free(&sb);
                set_content_type(res, "text/xml");
        } else {
                set_content_type(res, "text/plain");

                StringBuffer_append(res->outputbuffer, "Monit %s uptime: %s\n\n", VERSION, _getUptime(ProcessTree_getProcessUptime(getpid()), (char[256]){}));

                int found = 0;
                const char *stringGroup = Util_urlDecode((char *)get_parameter(req, "group"));
                const char *stringService = Util_urlDecode((char *)get_parameter(req, "service"));
                if (stringGroup) {
                        for (ServiceGroup_T sg = servicegrouplist; sg; sg = sg->next) {
                                if (IS(stringGroup, sg->name)) {
                                        for (list_t m = sg->members->head; m; m = m->next) {
                                                status_service_txt(m->e, res);
                                                found++;
                                        }
                                        break;
                                }
                        }
                } else {
                        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                                if (! stringService || IS(stringService, s->name)) {
                                        status_service_txt(s, res);
                                        found++;
                                }
                        }
                }
                if (found == 0) {
                        if (stringGroup)
                                send_error(req, res, SC_BAD_REQUEST, "Service group '%s' not found", stringGroup);
                        else if (stringService)
                                send_error(req, res, SC_BAD_REQUEST, "Service '%s' not found", stringService);
                        else
                                send_error(req, res, SC_BAD_REQUEST, "No service found");
                }
        }
}


static void _printServiceSummary(Box_T t, Service_T s) {
        Box_setColumn(t, 1, "%s", s->name);
        Box_setColumn(t, 2, "%s", get_service_status(TXT, s, (char[STRLEN]){}, STRLEN));
        Box_setColumn(t, 3, "%s", servicetypes[s->type]);
        Box_printRow(t);
}


static int _printServiceSummaryByType(Box_T t, Service_Type type) {
        int found = 0;
        for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                if (s->type == type) {
                        _printServiceSummary(t, s);
                        found++;
                }
        }
        return found;
}


static void print_summary(HttpRequest req, HttpResponse res) {
        set_content_type(res, "text/plain");

        StringBuffer_append(res->outputbuffer, "Monit %s uptime: %s\n", VERSION, _getUptime(ProcessTree_getProcessUptime(getpid()), (char[256]){}));

        int found = 0;
        const char *stringGroup = Util_urlDecode((char *)get_parameter(req, "group"));
        const char *stringService = Util_urlDecode((char *)get_parameter(req, "service"));
        Box_T t = Box_new(res->outputbuffer, 3, (BoxColumn_T []){
                        {.name = "Service Name", .width = 31, .wrap = false, .align = BoxAlign_Left},
                        {.name = "Status",       .width = 26, .wrap = false, .align = BoxAlign_Left},
                        {.name = "Type",         .width = 13, .wrap = false, .align = BoxAlign_Left}
                  }, true);
        if (stringGroup) {
                for (ServiceGroup_T sg = servicegrouplist; sg; sg = sg->next) {
                        if (IS(stringGroup, sg->name)) {
                                for (list_t m = sg->members->head; m; m = m->next) {
                                        _printServiceSummary(t, m->e);
                                        found++;
                                }
                                break;
                        }
                }
        } else if (stringService) {
                for (Service_T s = servicelist_conf; s; s = s->next_conf) {
                        if (IS(stringService, s->name)) {
                                _printServiceSummary(t, s);
                                found++;
                        }
                }
        } else {
                found += _printServiceSummaryByType(t, Service_System);
                found += _printServiceSummaryByType(t, Service_Process);
                found += _printServiceSummaryByType(t, Service_File);
                found += _printServiceSummaryByType(t, Service_Fifo);
                found += _printServiceSummaryByType(t, Service_Directory);
                found += _printServiceSummaryByType(t, Service_Filesystem);
                found += _printServiceSummaryByType(t, Service_Host);
                found += _printServiceSummaryByType(t, Service_Net);
                found += _printServiceSummaryByType(t, Service_Program);
        }
        Box_free(&t);
        if (found == 0) {
                if (stringGroup)
                        send_error(req, res, SC_BAD_REQUEST, "Service group '%s' not found", stringGroup);
                else if (stringService)
                        send_error(req, res, SC_BAD_REQUEST, "Service '%s' not found", stringService);
                else
                        send_error(req, res, SC_BAD_REQUEST, "No service found");
        }
}


static void _printReport(HttpRequest req, HttpResponse res) {
        set_content_type(res, "text/plain");
        const char *type = get_parameter(req, "type");
        int count = 0;
        if (! type) {
                float up = 0, down = 0, init = 0, unmonitored = 0, total = 0;
                for (Service_T s = servicelist; s; s = s->next) {
                        if (s->monitor == Monitor_Not)
                                unmonitored++;
                        else if (s->monitor & Monitor_Init)
                                init++;
                        else if (s->error)
                                down++;
                        else
                                up++;
                        total++;
                }
                StringBuffer_append(res->outputbuffer,
                        "up:           %*.0f (%.1f%%)\n"
                        "down:         %*.0f (%.1f%%)\n"
                        "initialising: %*.0f (%.1f%%)\n"
                        "unmonitored:  %*.0f (%.1f%%)\n"
                        "total:        %*.0f services\n",
                        3, up, 100. * up / total,
                        3, down, 100. * down / total,
                        3, init, 100. * init / total,
                        3, unmonitored, 100. * unmonitored / total,
                        3, total);
        } else if (Str_isEqual(type, "up")) {
                for (Service_T s = servicelist; s; s = s->next)
                        if (s->monitor != Monitor_Not && ! (s->monitor & Monitor_Init) && ! s->error)
                                count++;
                StringBuffer_append(res->outputbuffer, "%d\n", count);
        } else if (Str_isEqual(type, "down")) {
                for (Service_T s = servicelist; s; s = s->next)
                        if (s->monitor != Monitor_Not && ! (s->monitor & Monitor_Init) && s->error)
                                count++;
                StringBuffer_append(res->outputbuffer, "%d\n", count);
        } else if (Str_startsWith(type, "initiali")) { // allow 'initiali(s|z)ing'
                for (Service_T s = servicelist; s; s = s->next)
                        if (s->monitor & Monitor_Init)
                                count++;
                StringBuffer_append(res->outputbuffer, "%d\n", count);
        } else if (Str_isEqual(type, "unmonitored")) {
                for (Service_T s = servicelist; s; s = s->next)
                        if (s->monitor == Monitor_Not)
                                count++;
                StringBuffer_append(res->outputbuffer, "%d\n", count);
        } else if (Str_isEqual(type, "total")) {
                for (Service_T s = servicelist; s; s = s->next)
                        count++;
                StringBuffer_append(res->outputbuffer, "%d\n", count);
        } else {
                send_error(req, res, SC_BAD_REQUEST, "Invalid report type: '%s'", type);
        }
}


static void status_service_txt(Service_T s, HttpResponse res) {
        char buf[STRLEN];
        StringBuffer_append(res->outputbuffer,
                COLOR_BOLDCYAN "%s '%s'" COLOR_RESET "\n"
                "  %-28s %s\n",
                servicetypes[s->type], s->name,
                "status", get_service_status(TXT, s, buf, sizeof(buf)));
        StringBuffer_append(res->outputbuffer,
                "  %-28s %s\n",
                "monitoring status", get_monitoring_status(TXT, s, buf, sizeof(buf)));
        StringBuffer_append(res->outputbuffer,
                "  %-28s %s\n",
                "monitoring mode", modenames[s->mode]);
        StringBuffer_append(res->outputbuffer,
                "  %-28s %s\n",
                "on reboot", onrebootnames[s->onreboot]);
        _printStatus(TXT, res, s);
        StringBuffer_append(res->outputbuffer, "\n");
}


static char *get_monitoring_status(Output_Type type, Service_T s, char *buf, int buflen) {
        ASSERT(s);
        ASSERT(buf);
        if (s->monitor == Monitor_Not) {
                if (type == HTML)
                        snprintf(buf, buflen, "<span class='gray-text'>Not monitored</span>");
                else
                        snprintf(buf, buflen, Color_lightYellow("Not monitored"));
        } else if (s->monitor & Monitor_Waiting) {
                if (type == HTML)
                        snprintf(buf, buflen, "<span>Waiting</span>");
                else
                        snprintf(buf, buflen, Color_white("Waiting"));
        } else if (s->monitor & Monitor_Init) {
                if (type == HTML)
                        snprintf(buf, buflen, "<span class='blue-text'>Initializing</span>");
                else
                        snprintf(buf, buflen, Color_lightBlue("Initializing"));
        } else if (s->monitor & Monitor_Yes) {
                if (type == HTML)
                        snprintf(buf, buflen, "<span>Monitored</span>");
                else
                        snprintf(buf, buflen, "Monitored");
        }
        return buf;
}


static char *get_service_status(Output_Type type, Service_T s, char *buf, int buflen) {
        ASSERT(s);
        ASSERT(buf);
        if (s->monitor == Monitor_Not || s->monitor & Monitor_Init) {
                get_monitoring_status(type, s, buf, buflen);
        } else if (s->error == 0) {
                if (type == HTML)
                        snprintf(buf, buflen, "<span class='green-text'>%s</span>", statusnames[s->type]);
                else
                        snprintf(buf, buflen, Color_lightGreen("%s", statusnames[s->type]));
        } else {
                // In the case that the service has actualy some failure, the error bitmap will be non zero
                char *p = buf;
                EventTable_T *et = Event_Table;
                while ((*et).id) {
                        if (s->error & (*et).id) {
                                if (p > buf)
                                        p += snprintf(p, buflen - (p - buf), " | ");
                                if (s->error_hint & (*et).id) {
                                        if (type == HTML)
                                                p += snprintf(p, buflen - (p - buf), "<span class='orange-text'>%s</span>", (*et).description_changed);
                                        else
                                                p += snprintf(p, buflen - (p - buf), Color_lightYellow("%s", (*et).description_changed));
                                } else {
                                        if (type == HTML)
                                                p += snprintf(p, buflen - (p - buf), "<span class='red-text'>%s</span>", (*et).description_failed);
                                        else
                                                p += snprintf(p, buflen - (p - buf), Color_lightRed("%s", (*et).description_failed));
                                }
                        }
                        et++;
                }
        }
        if (s->doaction)
                snprintf(buf + strlen(buf), buflen - strlen(buf) - 1, " - %s pending", actionnames[s->doaction]);
        return buf;
}


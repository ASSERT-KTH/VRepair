/*
 * Embedthis Http Library Source
 */

#include "http.h"



/********* Start of file src/service.c ************/


/*
    service.c -- Http service. Includes timer for expired requests.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Locals ************************************/
/*
    Public singleton
 */
#undef HTTP
PUBLIC Http *HTTP;

/**
    Standard HTTP error code table
 */
typedef struct HttpStatusCode {
    int     code;                           /**< Http error code */
    char    *codeString;                    /**< Code as a string (for hashing) */
    char    *msg;                           /**< Error message */
} HttpStatusCode;


PUBLIC HttpStatusCode HttpStatusCodes[] = {
    { 100, "100", "Continue" },
    { 101, "101", "Switching Protocols" },
    { 200, "200", "OK" },
    { 201, "201", "Created" },
    { 202, "202", "Accepted" },
    { 204, "204", "No Content" },
    { 205, "205", "Reset Content" },
    { 206, "206", "Partial Content" },
    { 301, "301", "Moved Permanently" },
    { 302, "302", "Moved Temporarily" },
    { 304, "304", "Not Modified" },
    { 305, "305", "Use Proxy" },
    { 307, "307", "Temporary Redirect" },
    { 400, "400", "Bad Request" },
    { 401, "401", "Unauthorized" },
    { 402, "402", "Payment Required" },
    { 403, "403", "Forbidden" },
    { 404, "404", "Not Found" },
    { 405, "405", "Method Not Allowed" },
    { 406, "406", "Not Acceptable" },
    { 408, "408", "Request Timeout" },
    { 409, "409", "Conflict" },
    { 410, "410", "Gone" },
    { 411, "411", "Length Required" },
    { 412, "412", "Precondition Failed" },
    { 413, "413", "Request Entity Too Large" },
    { 414, "414", "Request-URI Too Large" },
    { 415, "415", "Unsupported Media Type" },
    { 416, "416", "Requested Range Not Satisfiable" },
    { 417, "417", "Expectation Failed" },
    { 500, "500", "Internal Server Error" },
    { 501, "501", "Not Implemented" },
    { 502, "502", "Bad Gateway" },
    { 503, "503", "Service Unavailable" },
    { 504, "504", "Gateway Timeout" },
    { 505, "505", "Http Version Not Supported" },
    { 507, "507", "Insufficient Storage" },

    /*
        Proprietary codes (used internally) when connection to client is severed
     */
    { 550, "550", "Comms Error" },
    { 551, "551", "General Client Error" },
    { 0,   0 }
};

/****************************** Forward Declarations **************************/

static void httpTimer(Http *http, MprEvent *event);
static bool isIdle(bool traceRequests);
static void manageHttp(Http *http, int flags);
static void terminateHttp(int state, int how, int status);
static void updateCurrentDate();

/*********************************** Code *************************************/

PUBLIC Http *httpCreate(int flags)
{
    Http            *http;
    HttpStatusCode  *code;

    mprGlobalLock();
    if (MPR->httpService) {
        mprGlobalUnlock();
        return MPR->httpService;
    }
    if ((http = mprAllocObj(Http, manageHttp)) == 0) {
        mprGlobalUnlock();
        return 0;
    }
    MPR->httpService = HTTP = http;
    http->software = sclone(ME_HTTP_SOFTWARE);
    http->protocol = sclone("HTTP/1.1");
    http->mutex = mprCreateLock();
    http->stages = mprCreateHash(-1, MPR_HASH_STABLE);
    http->hosts = mprCreateList(-1, MPR_LIST_STABLE);
    http->connections = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
    http->authTypes = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_UNIQUE | MPR_HASH_STABLE);
    http->authStores = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_UNIQUE | MPR_HASH_STABLE);
    http->routeSets = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    http->booted = mprGetTime();
    http->flags = flags;
    http->monitorMaxPeriod = 0;
    http->monitorMinPeriod = MAXINT;
    http->secret = mprGetRandomString(HTTP_MAX_SECRET);
    http->trace = httpCreateTrace(0);
    http->startLevel = 2;
    http->localPlatform = slower(sfmt("%s-%s-%s", ME_OS, ME_CPU, ME_PROFILE));
    httpSetPlatform(http->localPlatform);

    updateCurrentDate();
    http->statusCodes = mprCreateHash(41, MPR_HASH_STATIC_VALUES | MPR_HASH_STATIC_KEYS | MPR_HASH_STABLE);
    for (code = HttpStatusCodes; code->code; code++) {
        mprAddKey(http->statusCodes, code->codeString, code);
    }
    httpGetUserGroup();
    httpInitParser();
    httpInitAuth();
    httpOpenNetConnector();
    httpOpenSendConnector();
    httpOpenRangeFilter();
    httpOpenChunkFilter();
#if ME_HTTP_WEB_SOCKETS
    httpOpenWebSockFilter();
#endif
    mprSetIdleCallback(isIdle);
    mprAddTerminator(terminateHttp);

    if (flags & HTTP_SERVER_SIDE) {
        http->endpoints = mprCreateList(-1, MPR_LIST_STABLE);
        http->counters = mprCreateList(-1, MPR_LIST_STABLE);
        http->monitors = mprCreateList(-1, MPR_LIST_STABLE);
        http->routeTargets = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        http->routeConditions = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        http->routeUpdates = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        http->sessionCache = mprCreateCache(MPR_CACHE_SHARED | MPR_HASH_STABLE);
        http->addresses = mprCreateHash(-1, MPR_HASH_STABLE);
        http->defenses = mprCreateHash(-1, MPR_HASH_STABLE);
        http->remedies = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        httpOpenUploadFilter();
        httpOpenCacheHandler();
        httpOpenPassHandler();
        httpOpenActionHandler();
        httpOpenDirHandler();
        httpOpenFileHandler();
        http->serverLimits = httpCreateLimits(1);
        httpDefineRouteBuiltins();
        httpAddCounters();
        httpAddRemedies();
        httpCreateDefaultHost();
    }
    if (flags & HTTP_CLIENT_SIDE) {
        http->defaultClientHost = sclone("127.0.0.1");
        http->defaultClientPort = 80;
        http->clientLimits = httpCreateLimits(0);
        http->clientRoute = httpCreateConfiguredRoute(0, 0);
        http->clientHandler = httpCreateHandler("client", 0);
    }
    mprGlobalUnlock();
    return http;
}


static void manageHttp(Http *http, int flags)
{
    HttpConn    *conn;
    int         next;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(http->addresses);
        mprMark(http->authStores);
        mprMark(http->authTypes);
        mprMark(http->clientHandler);
        mprMark(http->clientLimits);
        mprMark(http->clientRoute);
        mprMark(http->connections);
        mprMark(http->context);
        mprMark(http->counters);
        mprMark(http->currentDate);
        mprMark(http->dateCache);
        mprMark(http->defaultClientHost);
        mprMark(http->defenses);
        mprMark(http->endpoints);
        mprMark(http->forkData);
        mprMark(http->group);
        mprMark(http->hosts);
        mprMark(http->localPlatform);
        mprMark(http->monitors);
        mprMark(http->mutex);
        mprMark(http->parsers);
        mprMark(http->platform);
        mprMark(http->platformDir);
        mprMark(http->protocol);
        mprMark(http->proxyHost);
        mprMark(http->remedies);
        mprMark(http->routeConditions);
        mprMark(http->routeSets);
        mprMark(http->routeTargets);
        mprMark(http->routeUpdates);
        mprMark(http->secret);
        mprMark(http->serverLimits);
        mprMark(http->sessionCache);
        mprMark(http->software);
        mprMark(http->stages);
        mprMark(http->statusCodes);
        mprMark(http->timer);
        mprMark(http->timestamp);
        mprMark(http->trace);
        mprMark(http->user);

        /*
            Endpoints keep connections alive until a timeout. Keep marking even if no other references.
         */
        lock(http->connections);
        for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
            if (httpServerConn(conn)) {
                mprMark(conn);
            }
        }
        unlock(http->connections);
    }
}


PUBLIC Http *httpGetHttp()
{
    return HTTP;
}


PUBLIC int httpStartEndpoints()
{
    HttpEndpoint    *endpoint;
    int             next;

    if (!HTTP) {
        return MPR_ERR_BAD_STATE;
    }
    for (ITERATE_ITEMS(HTTP->endpoints, endpoint, next)) {
        if (httpStartEndpoint(endpoint) < 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    if (httpApplyUserGroup() < 0) {
        httpStopEndpoints();
        return MPR_ERR_CANT_OPEN;
    }
    return 0;
}


PUBLIC void httpStopEndpoints()
{
    HttpEndpoint    *endpoint;
    Http            *http;
    int             next;

    if ((http = HTTP) == 0) {
        return;
    }
    lock(http->connections);
    for (next = 0; (endpoint = mprGetNextItem(http->endpoints, &next)) != 0; ) {
        httpStopEndpoint(endpoint);
    }
    unlock(http->connections);
}


/*
    Called to close all connections owned by a service (e.g. ejs)
 */
PUBLIC void httpStopConnections(void *data)
{
    Http        *http;
    HttpConn    *conn;
    int         next;

    if ((http = HTTP) == 0) {
        return;
    }
    lock(http->connections);
    for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
        if (data == 0 || conn->data == data) {
            httpDestroyConn(conn);
        }
    }
    unlock(http->connections);
}


/*
    Destroy the http service. This should be called only after ensuring all running requests have completed.
    Normally invoked by the http terminator from mprDestroy
 */
PUBLIC void httpDestroy() 
{
    Http            *http;

    if ((http = HTTP) == 0) {
        return;
    }
    httpStopConnections(0);
    httpStopEndpoints();

    if (http->timer) {
        mprRemoveEvent(http->timer);
        http->timer = 0;
    }
    if (http->timestamp) {
        mprRemoveEvent(http->timestamp);
        http->timestamp = 0;
    }
    MPR->httpService = NULL;
}


/*
    Http terminator called from mprDestroy
 */
static void terminateHttp(int state, int how, int status)
{
    if (state >= MPR_STOPPED) {
        httpDestroy();
    }
}


/*
    Test if the http service (including MPR) is idle with no running requests
 */
static bool isIdle(bool traceRequests)
{
    HttpConn        *conn;
    Http            *http;
    MprTicks        now;
    int             next;
    static MprTicks lastTrace = 0;

    if ((http = MPR->httpService) != 0) {
        now = http->now;
        lock(http->connections);
        for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
            if (conn->state != HTTP_STATE_BEGIN && conn->state != HTTP_STATE_COMPLETE) {
                if (traceRequests && lastTrace < now) {
                    if (conn->rx) {
                        mprLog("info http", 2, "Request for \"%s\" is still active", 
                            conn->rx->uri ? conn->rx->uri : conn->rx->pathInfo);
                    }
                    lastTrace = now;
                }
                unlock(http->connections);
                return 0;
            }
        }
        unlock(http->connections);
    } else {
        now = mprGetTicks();
    }
    return mprServicesAreIdle(traceRequests);
}


PUBLIC void httpAddEndpoint(HttpEndpoint *endpoint)
{
    mprAddItem(HTTP->endpoints, endpoint);
}


PUBLIC void httpRemoveEndpoint(HttpEndpoint *endpoint)
{
    mprRemoveItem(HTTP->endpoints, endpoint);
}


/*
    Lookup a host address. If ipAddr is null or port is -1, then those elements are wild.
 */
PUBLIC HttpEndpoint *httpLookupEndpoint(cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    int             next;

    if (ip == 0) {
        ip = "";
    }
    for (next = 0; (endpoint = mprGetNextItem(HTTP->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assert(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                return endpoint;
            }
        }
    }
    return 0;
}


PUBLIC HttpEndpoint *httpGetFirstEndpoint()
{
    return mprGetFirstItem(HTTP->endpoints);
}


/*
    WARNING: this should not be called by users as httpCreateHost will automatically call this.
 */
PUBLIC void httpAddHost(HttpHost *host)
{
    mprAddItem(HTTP->hosts, host);
}


PUBLIC void httpRemoveHost(HttpHost *host)
{
    mprRemoveItem(HTTP->hosts, host);
}


PUBLIC HttpHost *httpLookupHost(cchar *name)
{
    HttpHost    *host;
    int         next;

    for (next = 0; (host = mprGetNextItem(HTTP->hosts, &next)) != 0; ) {
        if (smatch(name, host->name)) {
            return host;
        }
    }
    return 0;
}


PUBLIC void httpInitLimits(HttpLimits *limits, bool serverSide)
{
    memset(limits, 0, sizeof(HttpLimits));
    limits->bufferSize = ME_MAX_QBUFFER;
    limits->cacheItemSize = ME_MAX_CACHE_ITEM;
    limits->chunkSize = ME_MAX_CHUNK;
    limits->clientMax = ME_MAX_CLIENTS;
    limits->connectionsMax = ME_MAX_CONNECTIONS;
    limits->headerMax = ME_MAX_NUM_HEADERS;
    limits->headerSize = ME_MAX_HEADERS;
    limits->keepAliveMax = ME_MAX_KEEP_ALIVE;
    limits->processMax = ME_MAX_PROCESSES;
    limits->requestsPerClientMax = ME_MAX_REQUESTS_PER_CLIENT;
    limits->sessionMax = ME_MAX_SESSIONS;
    limits->uriSize = ME_MAX_URI;

    limits->inactivityTimeout = ME_MAX_INACTIVITY_DURATION;
    limits->requestTimeout = ME_MAX_REQUEST_DURATION;
    limits->requestParseTimeout = ME_MAX_PARSE_DURATION;
    limits->sessionTimeout = ME_MAX_SESSION_DURATION;

    limits->webSocketsMax = ME_MAX_WSS_SOCKETS;
    limits->webSocketsMessageSize = ME_MAX_WSS_MESSAGE;
    limits->webSocketsFrameSize = ME_MAX_WSS_FRAME;
    limits->webSocketsPacketSize = ME_MAX_WSS_PACKET;
    limits->webSocketsPing = ME_MAX_PING_DURATION;

    if (serverSide) {
        limits->receiveFormSize = ME_MAX_RECEIVE_FORM;
        limits->receiveBodySize = ME_MAX_RECEIVE_BODY;
        limits->transmissionBodySize = ME_MAX_TX_BODY;
        limits->uploadSize = ME_MAX_UPLOAD;
    } else {
        limits->receiveFormSize = MAXOFF;
        limits->receiveBodySize = MAXOFF;
        limits->transmissionBodySize = MAXOFF;
        limits->uploadSize = MAXOFF;
    }

#if KEEP
    mprSetMaxSocketClients(endpoint, atoi(value));

    if (scaselesscmp(key, "LimitClients") == 0) {
        mprSetMaxSocketClients(endpoint, atoi(value));
        return 1;
    }
    if (scaselesscmp(key, "LimitMemoryMax") == 0) {
        mprSetAllocLimits(endpoint, -1, atoi(value));
        return 1;
    }
    if (scaselesscmp(key, "LimitMemoryRedline") == 0) {
        mprSetAllocLimits(endpoint, atoi(value), -1);
        return 1;
    }
#endif
}


PUBLIC HttpLimits *httpCreateLimits(int serverSide)
{
    HttpLimits  *limits;

    if ((limits = mprAllocStruct(HttpLimits)) != 0) {
        httpInitLimits(limits, serverSide);
    }
    return limits;
}


PUBLIC void httpEaseLimits(HttpLimits *limits)
{
    limits->receiveFormSize = MAXOFF;
    limits->receiveBodySize = MAXOFF;
    limits->transmissionBodySize = MAXOFF;
    limits->uploadSize = MAXOFF;
}


PUBLIC void httpAddStage(HttpStage *stage)
{
    mprAddKey(HTTP->stages, stage->name, stage);
}


PUBLIC HttpStage *httpLookupStage(cchar *name)
{
    HttpStage   *stage;

    if (!HTTP) {
        return 0;
    }
    if ((stage = mprLookupKey(HTTP->stages, name)) == 0 || stage->flags & HTTP_STAGE_INTERNAL) {
        return 0;
    }
    return stage;
}


PUBLIC void *httpLookupStageData(cchar *name)
{
    HttpStage   *stage;

    if (!HTTP) {
        return 0;
    }
    if ((stage = mprLookupKey(HTTP->stages, name)) != 0) {
        return stage->stageData;
    }
    return 0;
}


PUBLIC cchar *httpLookupStatus(int status)
{
    HttpStatusCode  *ep;
    char            *key;

    if (!HTTP) {
        return 0;
    }
    key = itos(status);
    ep = (HttpStatusCode*) mprLookupKey(HTTP->statusCodes, key);
    if (ep == 0) {
        return "Custom error";
    }
    return ep->msg;
}


PUBLIC void httpSetForkCallback(MprForkCallback callback, void *data)
{
    HTTP->forkCallback = callback;
    HTTP->forkData = data;
}


PUBLIC void httpSetListenCallback(HttpListenCallback fn)
{
    HTTP->listenCallback = fn;
}


/*
    The http timer does maintenance activities and will fire per second while there are active requests.
    This routine will also be called by httpTerminate with event == 0 to signify a shutdown.
    NOTE: Because we lock the http here, connections cannot be deleted while we are modifying the list.
 */
static void httpTimer(Http *http, MprEvent *event)
{
    HttpConn    *conn;
    HttpStage   *stage;
    HttpLimits  *limits;
    MprModule   *module;
    int         next, active, abort;

    updateCurrentDate();

    /* 
       Check for any inactive connections or expired requests (inactivityTimeout and requestTimeout)
       OPT - could check for expired connections every 10 seconds.
     */
    lock(http->connections);
    for (active = 0, next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; active++) {
        limits = conn->limits;
        if (!conn->timeoutEvent) {
            abort = mprIsStopping();
            if (httpServerConn(conn) && (HTTP_STATE_CONNECTED < conn->state && conn->state < HTTP_STATE_PARSED) && 
                    (http->now - conn->started) > limits->requestParseTimeout) {
                conn->timeout = HTTP_PARSE_TIMEOUT;
                abort = 1;
            } else if ((http->now - conn->lastActivity) > limits->inactivityTimeout) {
                conn->timeout = HTTP_INACTIVITY_TIMEOUT;
                abort = 1;
            } else if ((http->now - conn->started) > limits->requestTimeout) {
                conn->timeout = HTTP_REQUEST_TIMEOUT;
                abort = 1;
            } else if (!event) {
                /* Called directly from httpStop to stop connections */
                if (MPR->exitTimeout > 0) {
                    if (conn->state == HTTP_STATE_COMPLETE || 
                        (HTTP_STATE_CONNECTED < conn->state && conn->state < HTTP_STATE_PARSED)) {
                        abort = 1;
                    }
                } else {
                    abort = 1;
                }
            }
            if (abort && !mprGetDebugMode()) {
                httpScheduleConnTimeout(conn);
            }
        }
    }

    /*
        Check for unloadable modules
        OPT - could check for modules every minute
     */
    if (mprGetListLength(http->connections) == 0) {
        for (next = 0; (module = mprGetNextItem(MPR->moduleService->modules, &next)) != 0; ) {
            if (module->timeout) {
                if (module->lastActivity + module->timeout < http->now) {
                    mprLog("info http", 2, "Unloading inactive module %s", module->name);
                    if ((stage = httpLookupStage(module->name)) != 0) {
                        if (mprUnloadModule(module) < 0)  {
                            active++;
                        } else {
                            stage->flags |= HTTP_STAGE_UNLOADED;
                        }
                    } else {
                        mprUnloadModule(module);
                    }
                } else {
                    active++;
                }
            }
        }
    }
    httpPruneMonitors();

    if (active == 0 || mprIsStopping()) {
        if (event) {
            mprRemoveEvent(event);
        }
        http->timer = 0;
        /*
            Going to sleep now, so schedule a GC to free as much as possible.
         */
        mprGC(MPR_GC_FORCE | MPR_GC_NO_BLOCK);
    } else {
        mprGC(MPR_GC_NO_BLOCK);
    }
    unlock(http->connections);
}


static void timestamp()
{
    mprLog("info http", 0, "Time: %s", mprGetDate(NULL));
}


PUBLIC void httpSetTimestamp(MprTicks period)
{
    Http    *http;

    http = HTTP;
    if (period < (10 * MPR_TICKS_PER_SEC)) {
        period = (10 * MPR_TICKS_PER_SEC);
    }
    if (http->timestamp) {
        mprRemoveEvent(http->timestamp);
    }
    if (period > 0) {
        http->timestamp = mprCreateTimerEvent(NULL, "httpTimestamp", period, timestamp, NULL, 
            MPR_EVENT_CONTINUOUS | MPR_EVENT_QUICK);
    }
}


PUBLIC void httpAddConn(HttpConn *conn)
{
    Http    *http;

    http = HTTP;
    http->now = mprGetTicks();
    assert(http->now >= 0);
    conn->started = http->now;
    mprAddItem(http->connections, conn);
    updateCurrentDate();

    lock(http);
    conn->seqno = (int) ++http->totalConnections;
    if (!http->timer) {
#if ME_DEBUG
        if (!mprGetDebugMode())
#endif
        {
            http->timer = mprCreateTimerEvent(NULL, "httpTimer", HTTP_TIMER_PERIOD, httpTimer, http, 
                MPR_EVENT_CONTINUOUS | MPR_EVENT_QUICK);
        }
    }
    unlock(http);
}


PUBLIC void httpRemoveConn(HttpConn *conn)
{
    mprRemoveItem(HTTP->connections, conn);
}


PUBLIC char *httpGetDateString(MprPath *sbuf)
{
    MprTicks    when;

    if (sbuf == 0) {
        when = mprGetTime();
    } else {
        when = (MprTicks) sbuf->mtime * MPR_TICKS_PER_SEC;
    }
    return mprFormatUniversalTime(HTTP_DATE_FORMAT, when);
}


PUBLIC void *httpGetContext()
{
    return HTTP->context;
}


PUBLIC void httpSetContext(void *context)
{
    HTTP->context = context;
}


PUBLIC int httpGetDefaultClientPort()
{
    return HTTP->defaultClientPort;
}


PUBLIC cchar *httpGetDefaultClientHost()
{
    return HTTP->defaultClientHost;
}


PUBLIC void httpSetDefaultClientPort(int port)
{
    HTTP->defaultClientPort = port;
}


PUBLIC void httpSetDefaultClientHost(cchar *host)
{
    HTTP->defaultClientHost = sclone(host);
}


PUBLIC void httpSetSoftware(cchar *software)
{
    HTTP->software = sclone(software);
}


PUBLIC void httpSetProxy(cchar *host, int port)
{
    HTTP->proxyHost = sclone(host);
    HTTP->proxyPort = port;
}


static void updateCurrentDate()
{
    Http        *http;
    MprTicks    diff;

    http = HTTP;
    http->now = mprGetTicks();
    diff = http->now - http->currentTime;
    if (diff <= MPR_TICKS_PER_SEC || diff >= MPR_TICKS_PER_SEC) {
        /*
            Optimize and only update the string date representation once per second
         */
        http->currentTime = http->now;
        http->currentDate = httpGetDateString(NULL);
    }
}


PUBLIC void httpGetStats(HttpStats *sp)
{
    Http                *http;
    HttpAddress         *address;
    MprKey              *kp;
    MprMemStats         *ap;
    MprWorkerStats      wstats;
    ssize               memSessions;

    memset(sp, 0, sizeof(*sp));
    http = HTTP;
    ap = mprGetMemStats();

    sp->cpuUsage = ap->cpuUsage;
    sp->cpuCores = ap->cpuCores;
    sp->ram = ap->ram;
    sp->mem = ap->rss;
    sp->memRedline = ap->warnHeap;
    sp->memMax = ap->maxHeap;

    sp->heap = ap->bytesAllocated;
    sp->heapUsed = ap->bytesAllocated - ap->bytesFree;
    sp->heapPeak = ap->bytesAllocatedPeak;
    sp->heapFree = ap->bytesFree;
    sp->heapRegions = ap->heapRegions;

    mprGetWorkerStats(&wstats);
    sp->workersBusy = wstats.busy;
    sp->workersIdle = wstats.idle;
    sp->workersYielded = wstats.yielded;
    sp->workersMax = wstats.max;

    sp->activeConnections = mprGetListLength(http->connections);
    sp->activeProcesses = http->activeProcesses;

    mprGetCacheStats(http->sessionCache, &sp->activeSessions, &memSessions);
    sp->memSessions = memSessions;

    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        sp->activeRequests += (int) address->counters[HTTP_COUNTER_ACTIVE_REQUESTS].value;
        sp->activeClients++;
    }
    unlock(http->addresses);

    sp->totalRequests = http->totalRequests;
    sp->totalConnections = http->totalConnections;
    sp->totalSweeps = MPR->heap->stats.sweeps;
}


PUBLIC char *httpStatsReport(int flags)
{
    MprTime             now;
    MprBuf              *buf;
    HttpStats           s;
    double              elapsed;
    static MprTime      lastTime;
    static HttpStats    last;
    double              mb;

    mb = 1024.0 * 1024;
    now = mprGetTime();
    elapsed = (now - lastTime) / 1000.0;
    httpGetStats(&s);
    buf = mprCreateBuf(0, 0);

    mprPutToBuf(buf, "\nHttp Report: at %s\n\n", mprGetDate("%D %T"));
    if (flags & HTTP_STATS_MEMORY) {
        mprPutToBuf(buf, "Memory       %8.1f MB, %5.1f%% max\n", s.mem / mb, s.mem / (double) s.memMax * 100.0);
        mprPutToBuf(buf, "Heap         %8.1f MB, %5.1f%% mem\n", s.heap / mb, s.heap / (double) s.mem * 100.0);
        mprPutToBuf(buf, "Heap-peak    %8.1f MB\n", s.heapPeak / mb);
        mprPutToBuf(buf, "Heap-used    %8.1f MB, %5.1f%% used\n", s.heapUsed / mb, s.heapUsed / (double) s.heap * 100.0);
        mprPutToBuf(buf, "Heap-free    %8.1f MB, %5.1f%% free\n", s.heapFree / mb, s.heapFree / (double) s.heap * 100.0);

        if (s.memMax == (size_t) -1) {
            mprPutToBuf(buf, "Heap limit          -\n");
            mprPutToBuf(buf, "Heap readline       -\n");
        } else {
            mprPutToBuf(buf, "Heap limit   %8.1f MB\n", s.memMax / mb);
            mprPutToBuf(buf, "Heap redline %8.1f MB\n", s.memRedline / mb);
        }
    }

    mprPutToBuf(buf, "Connections  %8.1f per/sec\n", (s.totalConnections - last.totalConnections) / elapsed);
    mprPutToBuf(buf, "Requests     %8.1f per/sec\n", (s.totalRequests - last.totalRequests) / elapsed);
    mprPutToBuf(buf, "Sweeps       %8.1f per/sec\n", (s.totalSweeps - last.totalSweeps) / elapsed);
    mprPutCharToBuf(buf, '\n');

    mprPutToBuf(buf, "Clients      %8d active\n", s.activeClients);
    mprPutToBuf(buf, "Connections  %8d active\n", s.activeConnections);
    mprPutToBuf(buf, "Processes    %8d active\n", s.activeProcesses);
    mprPutToBuf(buf, "Requests     %8d active\n", s.activeRequests);
    mprPutToBuf(buf, "Sessions     %8d active\n", s.activeSessions);
    mprPutToBuf(buf, "Workers      %8d busy - %d yielded, %d idle, %d max\n", 
        s.workersBusy, s.workersYielded, s.workersIdle, s.workersMax);
    mprPutToBuf(buf, "Sessions     %8.1f MB\n", s.memSessions / mb);
    mprPutCharToBuf(buf, '\n');

    last = s;
    lastTime = now;
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


PUBLIC bool httpConfigure(HttpConfigureProc proc, void *data, MprTicks timeout)
{
    Http        *http;
    MprTicks    mark;

    http = HTTP;
    mark = mprGetTicks();
    if (timeout < 0) {
        timeout = http->serverLimits->requestTimeout;
    } else if (timeout == 0) {
        timeout = MAXINT;
    }
    do {
        lock(http->connections);
        /* Own request will count as 1 */
        if (mprGetListLength(http->connections) == 0) {
            (proc)(data);
            unlock(http->connections);
            return 1;
        }
        unlock(http->connections);
        mprSleep(10);
        /* Defaults to 10 secs */
    } while (mprGetRemainingTicks(mark, timeout) > 0);
    return 0;
}


PUBLIC int httpApplyUserGroup() 
{
#if ME_UNIX_LIKE
    Http    *http;

    http = HTTP;
    if (http->userChanged || http->groupChanged) {
        if (!smatch(MPR->logPath, "stdout") && !smatch(MPR->logPath, "stderr")) {
            if (chown(MPR->logPath, http->uid, http->gid) < 0) {
                mprLog("critical http", 0, "Cannot change ownership on %s", MPR->logPath);
            }
        }
    }
    if (httpApplyChangedGroup() < 0 || httpApplyChangedUser() < 0) {
        return MPR_ERR_CANT_COMPLETE;
    }
    if (http->userChanged || http->groupChanged) {
        struct group    *gp;
        gid_t           glist[64], gid;
        MprBuf          *gbuf = mprCreateBuf(0, 0);
        cchar           *groups;
        int             i, ngroup;

        gid = getgid();
        ngroup = getgroups(sizeof(glist) / sizeof(gid_t), glist);
        if (ngroup > 1) {
            mprPutStringToBuf(gbuf, ", groups: ");
            for (i = 0; i < ngroup; i++) {
                if (glist[i] == gid) continue;
                if ((gp = getgrgid(glist[i])) != 0) {
                    mprPutToBuf(gbuf, "%s (%d) ", gp->gr_name, glist[i]);
                } else {
                    mprPutToBuf(gbuf, "(%d) ", glist[i]);
                }
            }
        }
        groups = mprGetBufStart(gbuf);
        mprLog("info http", 2, "Running as user \"%s\" (%d), group \"%s\" (%d)%s", http->user, http->uid, 
            http->group, http->gid, groups);
    }
#endif
    return 0;
}


PUBLIC void httpGetUserGroup()
{
#if ME_UNIX_LIKE
    Http            *http;
    struct passwd   *pp;
    struct group    *gp;

    http = HTTP;
    http->uid = getuid();
    if ((pp = getpwuid(http->uid)) == 0) {
        mprLog("critical http", 0, "Cannot read user credentials: %d. Check your /etc/passwd file.", http->uid);
    } else {
        http->user = sclone(pp->pw_name);
    }
    http->gid = getgid();
    if ((gp = getgrgid(http->gid)) == 0) {
        mprLog("critical http", 0, "Cannot read group credentials: %d. Check your /etc/group file", http->gid);
    } else {
        http->group = sclone(gp->gr_name);
    }
#else
    Http *http = HTTP;
    http->uid = http->gid = -1;
#endif
}


PUBLIC int httpSetUserAccount(cchar *newUser)
{
    Http        *http;

    http = HTTP;
    if (smatch(newUser, "HTTP") || smatch(newUser, "APPWEB")) {
#if ME_UNIX_LIKE
        /* Only change user if root */
        if (getuid() != 0) {
            mprLog("info http", 2, "Running as user \"%s\"", http->user);
            return 0;
        }
#endif
#if MACOSX || FREEBSD
        newUser = "_www";
#elif LINUX || ME_UNIX_LIKE
        newUser = "nobody";
#elif WINDOWS
        newUser = "Administrator";
#endif
    }
#if ME_UNIX_LIKE
{
    struct passwd   *pp;
    if (snumber(newUser)) {
        http->uid = atoi(newUser);
        if ((pp = getpwuid(http->uid)) == 0) {
            mprLog("critical http", 0, "Bad user id: %d", http->uid);
            return MPR_ERR_CANT_ACCESS;
        }
        newUser = pp->pw_name;

    } else {
        if ((pp = getpwnam(newUser)) == 0) {
            mprLog("critical http", 0, "Bad user name: %s", newUser);
            return MPR_ERR_CANT_ACCESS;
        }
        http->uid = pp->pw_uid;
    }
    http->userChanged = 1;
}
#endif
    http->user = sclone(newUser);
    return 0;
}


//  TODO - this should be pushed down into http
PUBLIC int httpSetGroupAccount(cchar *newGroup)
{
    Http    *http;

    http = HTTP;
    if (smatch(newGroup, "HTTP") || smatch(newGroup, "APPWEB")) {
#if ME_UNIX_LIKE
        /* Only change group if root */
        if (getuid() != 0) {
            return 0;
        }
#endif
#if MACOSX || FREEBSD
        newGroup = "_www";
#elif LINUX || ME_UNIX_LIKE
{
        char    *buf;
        newGroup = "nobody";
        /*
            Debian has nogroup, Fedora has nobody. Ugh!
         */
        if ((buf = mprReadPathContents("/etc/group", NULL)) != 0) {
            if (scontains(buf, "nogroup:")) {
                newGroup = "nogroup";
            }
        }
}
#elif WINDOWS
        newGroup = "Administrator";
#endif
    }
#if ME_UNIX_LIKE
    struct group    *gp;

    if (snumber(newGroup)) {
        http->gid = atoi(newGroup);
        if ((gp = getgrgid(http->gid)) == 0) {
            mprLog("critical http", 0, "Bad group id: %d", http->gid);
            return MPR_ERR_CANT_ACCESS;
        }
        newGroup = gp->gr_name;

    } else {
        if ((gp = getgrnam(newGroup)) == 0) {
            mprLog("critical http", 0, "Bad group name: %s", newGroup);
            return MPR_ERR_CANT_ACCESS;
        }
        http->gid = gp->gr_gid;
    }
    http->groupChanged = 1;
#endif
    http->group = sclone(newGroup);
    return 0;
}


PUBLIC int httpApplyChangedUser()
{
#if ME_UNIX_LIKE
    Http    *http;

    http = HTTP;
    if (http->userChanged && http->uid >= 0) {
        if (http->gid >= 0 && http->groupChanged) {
            if (setgroups(0, NULL) == -1) {
                mprLog("critical http", 0, "Cannot clear supplemental groups");
            }
            if (setgid(http->gid) == -1) {
                mprLog("critical http", 0, "Cannot change group to %s: %d"
                    "WARNING: This is a major security exposure", http->group, http->gid);
            }
        } else {
            struct passwd   *pp;
            if ((pp = getpwuid(http->uid)) == 0) {
                mprLog("critical http", 0, "Cannot get user entry for id: %d", http->uid);
                return MPR_ERR_CANT_ACCESS;
            }
            mprLog("http", 4, "Initgroups for %s GID %d", http->user, pp->pw_gid);
            if (initgroups(http->user, pp->pw_gid) == -1) {
                mprLog("critical http", 0, "Cannot initgroups for %s, errno: %d", http->user, errno);
            }
        }
        if ((setuid(http->uid)) != 0) {
            mprLog("critical http", 0, "Cannot change user to: %s: %d"
                "WARNING: This is a major security exposure", http->user, http->uid);
            return MPR_ERR_BAD_STATE;
#if LINUX && PR_SET_DUMPABLE
        } else {
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }
    }
#endif
    return 0;
}


PUBLIC int httpApplyChangedGroup()
{
#if ME_UNIX_LIKE
    Http    *http;

    http = HTTP;
    if (http->groupChanged && http->gid >= 0) {
        if (setgid(http->gid) != 0) {
            mprLog("critical http", 0, "Cannot change group to %s: %d\n"
                "WARNING: This is a major security exposure", http->group, http->gid);
            if (getuid() != 0) {
                mprLog("critical http", 0, "Log in as administrator/root and retry");
            }
            return MPR_ERR_BAD_STATE;
#if LINUX && PR_SET_DUMPABLE
        } else {
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }
    }
#endif
    return 0;
}


PUBLIC int httpParsePlatform(cchar *platform, cchar **osp, cchar **archp, cchar **profilep)
{
    char   *arch, *os, *profile, *rest;

    if (osp) {
        *osp = 0;
    }
    if (archp) {
       *archp = 0;
    }
    if (profilep) {
       *profilep = 0;
    }
    if (platform == 0 || *platform == '\0') {
        return MPR_ERR_BAD_ARGS;
    }
    os = stok(sclone(platform), "-", &rest);
    arch = sclone(stok(NULL, "-", &rest));
    profile = sclone(rest);
    if (os == 0 || arch == 0 || profile == 0 || *os == '\0' || *arch == '\0' || *profile == '\0') {
        return MPR_ERR_BAD_ARGS;
    }
    if (osp) {
        *osp = os;
    }
    if (archp) {
       *archp = arch;
    }
    if (profilep) {
       *profilep = profile;
    }
    return 0;
}


PUBLIC int httpSetPlatform(cchar *platform)
{
    Http    *http;
    cchar   *junk;

    http = HTTP;
    if (platform && httpParsePlatform(platform, &junk, &junk, &junk) < 0) {
        return MPR_ERR_BAD_ARGS;
    }
    http->platform = platform ? sclone(platform) : http->localPlatform;
    mprLog("info http", 2, "Using platform %s", http->platform);
    return 0;
}


/*
    Set the platform objects location
 */
PUBLIC int httpSetPlatformDir(cchar *path)
{
    Http    *http;

    http = HTTP;
    if (path) {
        http->platformDir = mprGetAbsPath(path);
    } else {
        http->platformDir = mprGetPathDir(mprGetPathDir(mprGetAppPath()));
    }
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/actionHandler.c ************/


/*
    actionHandler.c -- Action handler

    This handler maps URIs to actions that are C functions that have been registered via httpDefineAction.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Code *************************************/

static void startAction(HttpQueue *q)
{
    HttpConn    *conn;
    HttpAction     action;
    cchar       *name;

    conn = q->conn;
    assert(!conn->error);
    assert(!conn->tx->finalized);

    name = conn->rx->pathInfo;
    if ((action = mprLookupKey(conn->tx->handler->stageData, name)) == 0) {
        httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot find action: %s", name);
    } else {
        (*action)(conn);
    }
}


PUBLIC void httpDefineAction(cchar *name, HttpAction action)
{
    HttpStage   *stage;

    if ((stage = httpLookupStage("actionHandler")) == 0) {
        mprLog("error http action", 0, "Cannot find actionHandler");
        return;
    }
    mprAddKey(stage->stageData, name, action);
}


PUBLIC int httpOpenActionHandler()
{
    HttpStage     *stage;

    if ((stage = httpCreateHandler("actionHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->actionHandler = stage;
    if ((stage->stageData = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
        return MPR_ERR_MEMORY;
    }
    stage->start = startAction;
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/auth.c ************/


/*

    auth.c - Authorization and access management

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************* Forwards ***********************************/

#undef  GRADUATE_HASH
#define GRADUATE_HASH(auth, field) \
    if (!auth->field) { \
        if (auth->parent && auth->field && auth->field == auth->parent->field) { \
            auth->field = mprCloneHash(auth->parent->field); \
        } else { \
            auth->field = mprCreateHash(0, MPR_HASH_STABLE); \
        } \
    }

static void manageAuth(HttpAuth *auth, int flags);
static void formLogin(HttpConn *conn);
PUBLIC int formParse(HttpConn *conn, cchar **username, cchar **password);
static bool configVerifyUser(HttpConn *conn, cchar *username, cchar *password);

/*********************************** Code *************************************/

PUBLIC void httpInitAuth()
{
    /*
        Auth protocol types: basic, digest, form
        These are typically not used for web frameworks like ESP or PHP 
     */
    httpCreateAuthType("basic", httpBasicLogin, httpBasicParse, httpBasicSetHeaders);
    httpCreateAuthType("digest", httpDigestLogin, httpDigestParse, httpDigestSetHeaders);
    httpCreateAuthType("form", formLogin, formParse, NULL);

    /*
        Stores: app, config, system
     */
    httpCreateAuthStore("app", NULL);
    httpCreateAuthStore("config", configVerifyUser);
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
    httpCreateAuthStore("system", httpPamVerifyUser);
#endif

#if DEPRECATED || 1
    httpCreateAuthStore("file", configVerifyUser);
    httpCreateAuthStore("internal", configVerifyUser);
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
    httpCreateAuthStore("pam", httpPamVerifyUser);
#endif
#endif
}

PUBLIC HttpAuth *httpCreateAuth()
{
    HttpAuth    *auth;

    if ((auth = mprAllocObj(HttpAuth, manageAuth)) == 0) {
        return 0;
    }
    auth->realm = MPR->emptyString;
    return auth;
}


PUBLIC HttpAuth *httpCreateInheritedAuth(HttpAuth *parent)
{
    HttpAuth      *auth;

    if ((auth = mprAllocObj(HttpAuth, manageAuth)) == 0) {
        return 0;
    }
    if (parent) {
        //  OPT. Structure assignment
        auth->flags = parent->flags;
        auth->allow = parent->allow;
        auth->cipher = parent->cipher;
        auth->deny = parent->deny;
        auth->type = parent->type;
        auth->store = parent->store;
        auth->flags = parent->flags;
        auth->qop = parent->qop;
        auth->realm = parent->realm;
        auth->permittedUsers = parent->permittedUsers;
        auth->abilities = parent->abilities;
        auth->userCache = parent->userCache;
        auth->roles = parent->roles;
        auth->loggedOutPage = parent->loggedOutPage;
        auth->loggedInPage = parent->loggedInPage;
        auth->loginPage = parent->loginPage;
        auth->username = parent->username;
        auth->verifyUser = parent->verifyUser;
        auth->parent = parent;
    }
    return auth;
}


static void manageAuth(HttpAuth *auth, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(auth->cipher);
        mprMark(auth->realm);
        mprMark(auth->allow);
        mprMark(auth->deny);
        mprMark(auth->userCache);
        mprMark(auth->roles);
        mprMark(auth->abilities);
        mprMark(auth->permittedUsers);
        mprMark(auth->loginPage);
        mprMark(auth->loggedInPage);
        mprMark(auth->loggedOutPage);
        mprMark(auth->username);
        mprMark(auth->qop);
        mprMark(auth->type);
        mprMark(auth->store);
    }
}


static void manageAuthType(HttpAuthType *type, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(type->name);
    }
}



static void manageAuthStore(HttpAuthStore *store, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(store->name);
    }
}

/*
    Authenticate a user using the session stored username. This will set HttpRx.authenticated if authentication succeeds.
    Note: this does not call httpLogin except for auto-login cases where a password is not used.
 */
PUBLIC bool httpAuthenticate(HttpConn *conn)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    cchar       *ip, *username;

    rx = conn->rx;
    auth = rx->route->auth;

    if (!rx->authenticateProbed) {
        rx->authenticateProbed = 1;
        ip = httpGetSessionVar(conn, HTTP_SESSION_IP, 0);
        username = httpGetSessionVar(conn, HTTP_SESSION_USERNAME, 0);
        if (!smatch(ip, conn->ip) || !username) {
            if (auth->username && *auth->username) {
                /* Auto-login */
                httpLogin(conn, auth->username, NULL);
                username = httpGetSessionVar(conn, HTTP_SESSION_USERNAME, 0);
            }
            if (!username) {
                return 0;
            }
        }
        httpTrace(conn, "auth.login.authenticated", "context", 
            "msg: 'Using cached authentication data', username:'%s'", username);
        conn->username = username;
        rx->authenticated = 1;
    }
    return rx->authenticated;
}


/*
    Test if the user has the requisite abilities to perform an action. Abilities may be explicitly defined or if NULL,
    the abilities specified by the route are used.
 */
PUBLIC bool httpCanUser(HttpConn *conn, cchar *abilities)
{
    HttpAuth    *auth;
    char        *ability, *tok;
    MprKey      *kp;

    auth = conn->rx->route->auth;
    if (auth->permittedUsers && !mprLookupKey(auth->permittedUsers, conn->username)) {
        return 0;
    }
    if (!auth->abilities && !abilities) {
        /* No abilities are required */
        return 1;
    }
    if (!conn->username) {
        /* User not authenticated */
        return 0;
    }
    if (!conn->user && (conn->user = mprLookupKey(auth->userCache, conn->username)) == 0) {
        return 0;
    }
    if (abilities) {
        for (ability = stok(sclone(abilities), " \t,", &tok); abilities; abilities = stok(NULL, " \t,", &tok)) {
            if (!mprLookupKey(conn->user->abilities, ability)) {
                return 0;
            }
        }
    } else {
        for (ITERATE_KEYS(auth->abilities, kp)) {
            if (!mprLookupKey(conn->user->abilities, kp->key)) {
                return 0;
            }
        }
    }
    return 1;
}


PUBLIC HttpAuthStore *httpCreateAuthStore(cchar *name, HttpVerifyUser verifyUser)
{
    HttpAuthStore   *store;

    if ((store = mprAllocObj(HttpAuthStore, manageAuthStore)) == 0) {
        return 0;
    }
    store->name = sclone(name);
    store->verifyUser = verifyUser;
    if (mprAddKey(HTTP->authStores, name, store) == 0) {
        return 0;
    }
    return store;
}



PUBLIC int httpCreateAuthType(cchar *name, HttpAskLogin askLogin, HttpParseAuth parseAuth, HttpSetAuth setAuth)
{
    HttpAuthType    *type;

    if ((type = mprAllocObj(HttpAuthType, manageAuthType)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    type->name = sclone(name);
    type->askLogin = askLogin;
    type->parseAuth = parseAuth;
    type->setAuth = setAuth;

    if (mprAddKey(HTTP->authTypes, name, type) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}


/*
    Get the username and password credentials. If using an in-protocol auth scheme like basic|digest, the
    rx->authDetails will contain the credentials and the parseAuth callback will be invoked to parse.
    Otherwise, it is expected that "username" and "password" fields are present in the request parameters.

    This is called by authCondition which thereafter calls httpLogin
 */
PUBLIC bool httpGetCredentials(HttpConn *conn, cchar **username, cchar **password)
{
    HttpAuth    *auth;

    assert(username);
    assert(password);
    *username = *password = NULL;

    auth = conn->rx->route->auth;
    if (auth->type) {
        if (conn->authType && !smatch(conn->authType, auth->type->name)) {
            return 0;
        }
        if (auth->type->parseAuth && (auth->type->parseAuth)(conn, username, password) < 0) {
            return 0;
        }
    } else {
        *username = httpGetParam(conn, "username", 0);
        *password = httpGetParam(conn, "password", 0);
    }
    return 1;
}


PUBLIC bool httpIsAuthenticated(HttpConn *conn)
{
    return httpAuthenticate(conn);
}


/*
    Login the user and create an authenticated session state store
 */
PUBLIC bool httpLogin(HttpConn *conn, cchar *username, cchar *password)
{
    HttpRx          *rx;
    HttpAuth        *auth;
    HttpSession     *session;
    HttpVerifyUser  verifyUser;

    rx = conn->rx;
    auth = rx->route->auth;
    if (!username || !*username) {
        httpTrace(conn, "auth.login.error", "error", "msg:'missing username'");
        return 0;
    }
    if (!auth->store) {
        mprLog("error http auth", 0, "No AuthStore defined");
        return 0;
    }
    if ((verifyUser = auth->verifyUser) == 0) {
        if (!auth->parent || (verifyUser = auth->parent->verifyUser) == 0) {
            verifyUser = auth->store->verifyUser;
        }
    }
    if (!verifyUser) {
        mprLog("error http auth", 0, "No user verification routine defined on route %s", rx->route->name);
        return 0;
    }
    if (auth->username && *auth->username) {
        /* If using auto-login, replace the username */
        username = auth->username;
        password = 0;
    }
    if (!(verifyUser)(conn, username, password)) {
        return 0;
    }
    if (!(auth->flags & HTTP_AUTH_NO_SESSION) && !auth->store->noSession) {
        if ((session = httpCreateSession(conn)) == 0) {
            /* Too many sessions */
            return 0;
        }
        httpSetSessionVar(conn, HTTP_SESSION_USERNAME, username);
        httpSetSessionVar(conn, HTTP_SESSION_IP, conn->ip);
    }
    rx->authenticated = 1;
    rx->authenticateProbed = 1;
    conn->username = sclone(username);
    conn->encoded = 0;
    return 1;
}


PUBLIC bool httpIsLoggedIn(HttpConn *conn)
{
    return httpAuthenticate(conn);
}


/*
    Log the user out and remove the authentication username from the session state
 */
PUBLIC void httpLogout(HttpConn *conn)
{
    conn->rx->authenticated = 0;
    httpDestroySession(conn);
}


PUBLIC void httpSetAuthVerify(HttpAuth *auth, HttpVerifyUser verifyUser)
{
    auth->verifyUser = verifyUser;
}


PUBLIC void httpSetAuthAllow(HttpAuth *auth, cchar *allow)
{
    GRADUATE_HASH(auth, allow);
    mprAddKey(auth->allow, sclone(allow), auth);
}


PUBLIC void httpSetAuthAnyValidUser(HttpAuth *auth)
{
    auth->permittedUsers = 0;
}


PUBLIC void httpSetAuthLogin(HttpAuth *auth, cchar *value)
{
    auth->loginPage = sclone(value);
}


/*
    Web form login service routine. Called in response to a form-based login request when defined via httpSetAuthLogin.
    It is expected that "authCondition" has already authenticated the request.
 */
static void loginServiceProc(HttpConn *conn)
{
    HttpAuth    *auth;
    
    auth = conn->rx->route->auth;
    if (httpIsAuthenticated(conn)) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loggedInPage ? auth->loggedInPage : "~");
    } else {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    }
}


/*
    Logout service for use with httpSetAuthFormDetails.
 */
static void logoutServiceProc(HttpConn *conn)
{
    HttpRoute       *route;
    HttpAuth        *auth;
    cchar           *loggedOut;

    route = conn->rx->route;
    auth = route->auth;

    httpLogout(conn);

    loggedOut = (auth->loggedOutPage) ? auth->loggedOutPage : auth->loginPage;
    if (!loggedOut) {
        loggedOut = "/";
    }
    httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, loggedOut);
}


static HttpRoute *createLoginRoute(HttpRoute *route, cchar *pattern, HttpAction action)
{
    bool    secure;

    secure = 0;
    if (sstarts(pattern, "https:///")) {
        pattern = &pattern[8];
        secure = 1;
    }
    if ((route = httpCreateInheritedRoute(route)) != 0) {
        httpSetRoutePattern(route, sjoin(pattern, "$", NULL), 0);
        if (secure) {
            httpAddRouteCondition(route, "secure", "https://", HTTP_ROUTE_REDIRECT);
        }
        if (action) {
            route->handler = route->http->actionHandler;
            httpDefineAction(pattern, action);
        }
        httpSetRouteName(route, strim(pattern, "^$", 0));
        httpFinalizeRoute(route);
    }
    return route;
}


/*
    Define login URLs by creating routes. Used by Appweb AuthType directive.
    Web frameworks like ESP should NOT use this.
 */
PUBLIC void httpSetAuthFormDetails(HttpRoute *route, cchar *loginPage, cchar *loginService, cchar *logoutService, 
    cchar *loggedInPage, cchar *loggedOutPage)
{
    HttpRoute   *loginRoute;
    HttpAuth    *auth;

    auth = route->auth;

    if (loggedInPage) {
        auth->loggedInPage = sclone(loggedInPage);
        createLoginRoute(route, auth->loggedInPage, 0);
    }
    if (loginPage) {
        auth->loginPage = sclone(loginPage);
        createLoginRoute(route, auth->loginPage, 0);
    }
    if (loggedOutPage) {
        if (smatch(loginPage, loggedOutPage)) {
            auth->loggedOutPage = auth->loginPage;
        } else {
            auth->loggedOutPage = sclone(loggedOutPage);
            createLoginRoute(route, auth->loggedOutPage, 0);
        }
    }
    /*
        Put services last so they inherit the auth settings above
     */
    if (loginService) {
        loginRoute = createLoginRoute(route, loginService, loginServiceProc);
        httpAddRouteCondition(loginRoute, "auth", 0, 0);
    }
    if (logoutService) {
        createLoginRoute(route, logoutService, logoutServiceProc);
    }
}


/*
    Can supply a roles or abilities in the "abilities" parameter
 */
PUBLIC void httpSetAuthRequiredAbilities(HttpAuth *auth, cchar *abilities)
{
    char    *ability, *tok;

    GRADUATE_HASH(auth, abilities);
    for (ability = stok(sclone(abilities), " \t,", &tok); abilities; abilities = stok(NULL, " \t,", &tok)) {
        httpComputeRoleAbilities(auth, auth->abilities, ability);
    }
}


PUBLIC void httpSetAuthDeny(HttpAuth *auth, cchar *client)
{
    GRADUATE_HASH(auth, deny);
    mprAddKey(auth->deny, sclone(client), auth);
}


PUBLIC void httpSetAuthOrder(HttpAuth *auth, int order)
{
    auth->flags &= (HTTP_ALLOW_DENY | HTTP_DENY_ALLOW);
    auth->flags |= (order & (HTTP_ALLOW_DENY | HTTP_DENY_ALLOW));
}



/*
    Can also achieve this via abilities
 */
PUBLIC void httpSetAuthPermittedUsers(HttpAuth *auth, cchar *users)
{
    char    *user, *tok;

    GRADUATE_HASH(auth, permittedUsers);
    for (user = stok(sclone(users), " \t,", &tok); users; users = stok(NULL, " \t,", &tok)) {
        if (smatch(user, "*")) {
            auth->permittedUsers = 0;
            break;
        } else {
            mprAddKey(auth->permittedUsers, user, user);
        }
    }
}


PUBLIC void httpSetAuthQop(HttpAuth *auth, cchar *qop)
{
    auth->qop = sclone(qop);
}


PUBLIC void httpSetAuthRealm(HttpAuth *auth, cchar *realm)
{
    auth->realm = sclone(realm);
}


PUBLIC void httpSetAuthStoreSessions(HttpAuthStore *store, bool noSession)
{
    assert(store);
    store->noSession = noSession;
}


PUBLIC void httpSetAuthSession(HttpAuth *auth, bool enable)
{
    auth->flags &= ~HTTP_AUTH_NO_SESSION;
    if (!enable) {
        auth->flags |= HTTP_AUTH_NO_SESSION;
    }
}


PUBLIC int httpSetAuthStore(HttpAuth *auth, cchar *store)
{
    if ((auth->store = mprLookupKey(HTTP->authStores, store)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (smatch(store, "system")) {
#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
        if (auth->type && smatch(auth->type->name, "digest")) {
            mprLog("critical http auth", 0, "Cannot use the PAM password store with digest authentication");
            return MPR_ERR_BAD_ARGS;
        }
#else
        mprLog("critical http auth", 0, "PAM is not supported in the current configuration");
        return MPR_ERR_BAD_ARGS;
#endif
    }
    GRADUATE_HASH(auth, userCache);
    return 0;
}


PUBLIC int httpSetAuthType(HttpAuth *auth, cchar *type, cchar *details)
{
    if (type == 0 || *type == '\0' || smatch(type, "none")) {
        auth->type = 0;
        return 0;
    }
    if ((auth->type = mprLookupKey(HTTP->authTypes, type)) == 0) {
        mprLog("critical http auth", 0, "Cannot find auth type %s", type);
        return MPR_ERR_CANT_FIND;
    }
    if (!auth->store) {
        httpSetAuthStore(auth, "config");
    }
    return 0;
}


/*
    This implements auto-loging without requiring a password
 */
PUBLIC void httpSetAuthUsername(HttpAuth *auth, cchar *username)
{
    auth->username = sclone(username);
}


PUBLIC HttpAuthType *httpLookupAuthType(cchar *type)
{
    return mprLookupKey(HTTP->authTypes, type);
}


/*
    Verify the user password for the "config" store based on the users defined via configuration directives.
    Password may be NULL only if using auto-login.
 */
static bool configVerifyUser(HttpConn *conn, cchar *username, cchar *password)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    bool        success;
    char        *requiredPassword;

    rx = conn->rx;
    auth = rx->route->auth;
    if (!conn->user && (conn->user = mprLookupKey(auth->userCache, username)) == 0) {
        httpTrace(conn, "auth.login.error", "error", "msg: 'Unknown user', username:'%s'", username);
        return 0;
    }
    if (password) {
        if (auth->realm == 0 || *auth->realm == '\0') {
            mprLog("error http auth", 0, "No AuthRealm defined");
        }
        requiredPassword = (rx->passwordDigest) ? rx->passwordDigest : conn->user->password;
        if (sncmp(requiredPassword, "BF", 2) == 0 && slen(requiredPassword) > 4 && isdigit(requiredPassword[2]) &&
                requiredPassword[3] == ':') {
            /* Blowifsh */
            success = mprCheckPassword(sfmt("%s:%s:%s", username, auth->realm, password), conn->user->password);

        } else {
            if (!conn->encoded) {
                password = mprGetMD5(sfmt("%s:%s:%s", username, auth->realm, password));
                conn->encoded = 1;
            }
            success = smatch(password, requiredPassword);
        }
        if (success) {
            httpTrace(conn, "auth.login.authenticated", "context", "msg:'User authenticated', username:'%s'", username);
        } else {
            httpTrace(conn, "auth.login.error", "error", "msg:'Password failed to authenticate', username:'%s'", username);
        }
        return success;
    }
    return 1;
}


/*
    Web form-based authentication callback for the "form" auth protocol.
    Asks the user to login via a web page.
 */
static void formLogin(HttpConn *conn)
{
    if (conn->rx->route->auth && conn->rx->route->auth->loginPage) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, conn->rx->route->auth->loginPage);
    } else {
        httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
    }
}


PUBLIC int formParse(HttpConn *conn, cchar **username, cchar **password)
{
    *username = httpGetParam(conn, "username", 0);
    *password = httpGetParam(conn, "password", 0);
    return 0;
}


#undef  GRADUATE_HASH

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/basic.c ************/


/*
    basic.c - Basic Authorization 

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Code *************************************/
/*
    Parse the client 'Authorization' header and the server 'Www-Authenticate' header
 */
PUBLIC int httpBasicParse(HttpConn *conn, cchar **username, cchar **password)
{
    HttpRx  *rx;
    char    *decoded, *cp;

    rx = conn->rx;
    if (password) {
        *password = NULL;
    }
    if (username) {
        *username = NULL;
    }
    if (!rx->authDetails) {
        return 0;
    }
    if ((decoded = mprDecode64(rx->authDetails)) == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if ((cp = strchr(decoded, ':')) != 0) {
        *cp++ = '\0';
    }
    conn->encoded = 0;
    if (username) {
        *username = sclone(decoded);
    }
    if (password) {
        *password = sclone(cp);
    }
    return 0;
}


/*
    Respond to the request by asking for a client login
    Only called if not logged in
 */
PUBLIC void httpBasicLogin(HttpConn *conn)
{
    HttpAuth    *auth;

    auth = conn->rx->route->auth;
    if (auth->loginPage && !sends(conn->rx->referrer, auth->loginPage)) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    } else {
        httpSetHeader(conn, "WWW-Authenticate", "Basic realm=\"%s\"", auth->realm);
        httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
        httpTrace(conn, "auth.basic.error", "error", "msg:'Access denied, Login required'");
    }
}


/*
    Add the client 'Authorization' header for authenticated requests
    NOTE: Can do this without first getting a 401 response
 */
PUBLIC bool httpBasicSetHeaders(HttpConn *conn, cchar *username, cchar *password)
{
    httpAddHeader(conn, "Authorization", "basic %s", mprEncode64(sfmt("%s:%s", username, password)));
    return 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/cache.c ************/


/*
    cache.c -- Http request route caching

    Caching operates as both a handler and an output filter. If acceptable cached content is found, the
    cacheHandler will serve it instead of the normal handler. If no content is acceptable and caching is enabled
    for the request, the cacheFilter will capture and save the response.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void cacheAtClient(HttpConn *conn);
static bool fetchCachedResponse(HttpConn *conn);
static HttpCache *lookupCacheControl(HttpConn *conn);
static char *makeCacheKey(HttpConn *conn);
static void manageHttpCache(HttpCache *cache, int flags);
static int matchCacheFilter(HttpConn *conn, HttpRoute *route, int dir);
static int matchCacheHandler(HttpConn *conn, HttpRoute *route, int dir);
static void outgoingCacheFilterService(HttpQueue *q);
static void readyCacheHandler(HttpQueue *q);
static void saveCachedResponse(HttpConn *conn);
static cchar *setHeadersFromCache(HttpConn *conn, cchar *content);

/************************************ Code ************************************/

PUBLIC int httpOpenCacheHandler()
{
    HttpStage     *handler, *filter;

    /*
        Create the cache handler to serve cached content
     */
    if ((handler = httpCreateHandler("cacheHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->cacheHandler = handler;
    handler->match = matchCacheHandler;
    handler->ready = readyCacheHandler;

    /*
        Create the cache filter to capture and cache response content
     */
    if ((filter = httpCreateFilter("cacheFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->cacheFilter = filter;
    filter->match = matchCacheFilter;
    filter->outgoingService = outgoingCacheFilterService;
    return 0;
}


/*
    See if there is acceptable cached content to serve
 */
static int matchCacheHandler(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpCache   *cache;

    if ((cache = conn->tx->cache = lookupCacheControl(conn)) == 0) {
        /* Caching not configured for this route */
        return HTTP_ROUTE_REJECT;
    }
    if (cache->flags & HTTP_CACHE_CLIENT) {
        cacheAtClient(conn);
    }
    if (cache->flags & HTTP_CACHE_SERVER) {
        if (!(cache->flags & HTTP_CACHE_MANUAL) && fetchCachedResponse(conn)) {
            /* Found cached content */
            return HTTP_ROUTE_OK;
        }
        /*
            Caching is configured but no acceptable cached content. Create a capture buffer for the cacheFilter.
         */
        conn->tx->cacheBuffer = mprCreateBuf(-1, -1);
    }
    return HTTP_ROUTE_REJECT;
}


static void readyCacheHandler(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    cchar       *data;

    conn = q->conn;
    tx = conn->tx;

    if (tx->cachedContent) {
        if ((data = setHeadersFromCache(conn, tx->cachedContent)) != 0) {
            tx->length = slen(data);
            httpWriteString(q, data);
        }
    }
    httpFinalize(conn);
}


static int matchCacheFilter(HttpConn *conn, HttpRoute *route, int dir)
{
    if ((dir & HTTP_STAGE_TX) && conn->tx->cacheBuffer) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


/*
    This will be enabled when caching is enabled for the route and there is no acceptable cache data to use.
    OR - manual caching has been enabled.
 */
static void outgoingCacheFilterService(HttpQueue *q)
{
    HttpPacket  *packet, *data;
    HttpConn    *conn;
    HttpTx      *tx;
    MprKey      *kp;
    cchar       *cachedData;
    ssize       size;
    int         foundDataPacket;

    conn = q->conn;
    tx = conn->tx;
    foundDataPacket = 0;
    cachedData = 0;

    if (tx->status < 200 || tx->status > 299) {
        tx->cacheBuffer = 0;
    }

    /*
        This routine will save cached responses to tx->cacheBuffer.
        It will also send cached data if the X-SendCache header is present. Normal caching is done by cacheHandler.
     */
    if (mprLookupKey(conn->tx->headers, "X-SendCache") != 0) {
        if (fetchCachedResponse(conn)) {
            httpTrace(conn, "cache.sendcache", "context", "msg:'Using cached content'");
            cachedData = setHeadersFromCache(conn, tx->cachedContent);
            tx->length = slen(cachedData);
        }
    }
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!httpWillNextQueueAcceptPacket(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        if (packet->flags & HTTP_PACKET_HEADER) {
            if (!cachedData && tx->cacheBuffer) {
                /*
                    Add defined headers to the start of the cache buffer. Separate with a double newline.
                 */
                mprPutToBuf(tx->cacheBuffer, "X-Status: %d\n", tx->status);
                for (kp = 0; (kp = mprGetNextKey(tx->headers, kp)) != 0; ) {
                    mprPutToBuf(tx->cacheBuffer, "%s: %s\n", kp->key, (char*) kp->data);
                }
                mprPutCharToBuf(tx->cacheBuffer, '\n');
            }

        } else if (packet->flags & HTTP_PACKET_DATA) {
            if (cachedData) {
                /*
                    Using X-SendCache. Replace the data with the cached response.
                 */
                mprFlushBuf(packet->content);
                mprPutBlockToBuf(packet->content, cachedData, (ssize) tx->length);

            } else if (tx->cacheBuffer) {
                /*
                    Save the response packet to the cache buffer. Will write below in saveCachedResponse.
                 */
                size = mprGetBufLength(packet->content);
                if ((tx->cacheBufferLength + size) < conn->limits->cacheItemSize) {
                    mprPutBlockToBuf(tx->cacheBuffer, mprGetBufStart(packet->content), mprGetBufLength(packet->content));
                    tx->cacheBufferLength += size;
                } else {
                    tx->cacheBuffer = 0;
                    httpTrace(conn, "cache.big", "context", "msg:'Item too big to cache',size:%zu,limit:%zu",
                        tx->cacheBufferLength + size, conn->limits->cacheItemSize);
                }
            }
            foundDataPacket = 1;

        } else if (packet->flags & HTTP_PACKET_END) {
            if (cachedData && !foundDataPacket) {
                /*
                    Using X-SendCache but there was no data packet to replace. So do the write here.
                 */
                data = httpCreateDataPacket((ssize) tx->length);
                mprPutBlockToBuf(data->content, cachedData, (ssize) tx->length);
                httpPutPacketToNext(q, data);

            } else if (tx->cacheBuffer) {
                /*
                    Save the cache buffer to the cache store
                 */
                saveCachedResponse(conn);
            }
        }
        httpPutPacketToNext(q, packet);
    }
}


/*
    Find a qualifying cache control entry. Any configured uri,method,extension,type must match.
 */
static HttpCache *lookupCacheControl(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpCache   *cache;
    cchar       *mimeType, *ukey;
    int         next;

    rx = conn->rx;
    tx = conn->tx;

    /*
        Find first qualifying cache control entry. Any configured uri,method,extension,type must match.
     */
    for (next = 0; (cache = mprGetNextItem(rx->route->caching, &next)) != 0; ) {
        if (cache->uris) {
            if (cache->flags & HTTP_CACHE_HAS_PARAMS) {
                ukey = sfmt("%s?%s", rx->pathInfo, httpGetParamsString(conn));
            } else {
                ukey = rx->pathInfo;
            }
            if (!mprLookupKey(cache->uris, ukey)) {
                continue;
            }
        }
        if (cache->methods && !mprLookupKey(cache->methods, rx->method)) {
            continue;
        }
        if (cache->extensions && !mprLookupKey(cache->extensions, tx->ext)) {
            continue;
        }
        if (cache->types) {
            if ((mimeType = (char*) mprLookupMime(rx->route->mimeTypes, tx->ext)) == 0) {
                continue;
            }
            if (!mprLookupKey(cache->types, mimeType)) {
                continue;
            }
        }
        /* All match */
        break;
    }
    return cache;
}


static void cacheAtClient(HttpConn *conn)
{
    HttpTx      *tx;
    HttpCache   *cache;
    cchar       *value;

    tx = conn->tx;
    cache = conn->tx->cache;

    if (!mprLookupKey(tx->headers, "Cache-Control")) {
        if ((value = mprLookupKey(conn->tx->headers, "Cache-Control")) != 0) {
            if (strstr(value, "max-age") == 0) {
                httpAppendHeader(conn, "Cache-Control", "public, max-age=%lld", cache->clientLifespan / MPR_TICKS_PER_SEC);
            }
        } else {
            httpAddHeader(conn, "Cache-Control", "public, max-age=%lld", cache->clientLifespan / MPR_TICKS_PER_SEC);
            /*
                Old HTTP/1.0 clients don't understand Cache-Control
             */
            httpAddHeaderString(conn, "Expires", mprFormatUniversalTime(MPR_HTTP_DATE,
                mprGetTime() + cache->clientLifespan));
        }
    }
}


/*
    See if there is acceptable cached content for this request. If so, return true.
    Will setup tx->cacheBuffer as a side-effect if the output should be captured and cached.
 */
static bool fetchCachedResponse(HttpConn *conn)
{
    HttpTx      *tx;
    MprTime     modified, when;
    cchar       *value, *key, *tag;
    int         status, cacheOk, canUseClientCache;

    tx = conn->tx;

    /*
        Transparent caching. Manual caching must manually call httpWriteCached()
     */
    key = makeCacheKey(conn);
    if ((value = httpGetHeader(conn, "Cache-Control")) != 0 &&
            (scontains(value, "max-age=0") == 0 || scontains(value, "no-cache") == 0)) {
        httpTrace(conn, "cache.reload", "context", "msg:'Client reload'");

    } else if ((tx->cachedContent = mprReadCache(conn->host->responseCache, key, &modified, 0)) != 0) {
        /*
            See if a NotModified response can be served. This is much faster than sending the response.
            Observe headers:
                If-None-Match: "ec18d-54-4d706a63"
                If-Modified-Since: Fri, 04 Mar 2014 04:28:19 GMT
            Set status to OK when content must be transmitted.
         */
        cacheOk = 1;
        canUseClientCache = 0;
        tag = mprGetMD5(key);
        if ((value = httpGetHeader(conn, "If-None-Match")) != 0) {
            canUseClientCache = 1;
            if (scmp(value, tag) != 0) {
                cacheOk = 0;
            }
        }
        if (cacheOk && (value = httpGetHeader(conn, "If-Modified-Since")) != 0) {
            canUseClientCache = 1;
            mprParseTime(&when, value, 0, 0);
            if (modified > when) {
                cacheOk = 0;
            }
        }
        status = (canUseClientCache && cacheOk) ? HTTP_CODE_NOT_MODIFIED : HTTP_CODE_OK;
        httpTrace(conn, "cache.cached", "context", "msg:'Use cached content',key:'%s',status:%d", key, status);
        httpSetStatus(conn, status);
        httpSetHeaderString(conn, "Etag", mprGetMD5(key));
        httpSetHeaderString(conn, "Last-Modified", mprFormatUniversalTime(MPR_HTTP_DATE, modified));
        return 1;
    }
    httpTrace(conn, "cache.none", "context", "msg:'No cached content',key:'%s'", key);
    return 0;
}


static void saveCachedResponse(HttpConn *conn)
{
    HttpTx      *tx;
    MprBuf      *buf;
    MprTime     modified;

    tx = conn->tx;
    assert(tx->finalizedOutput && tx->cacheBuffer);

    buf = tx->cacheBuffer;
    tx->cacheBuffer = 0;
    /*
        Truncate modified time to get a 1 sec resolution. This is the resolution for If-Modified headers.
     */
    modified = mprGetTime() / MPR_TICKS_PER_SEC * MPR_TICKS_PER_SEC;
    mprWriteCache(conn->host->responseCache, makeCacheKey(conn), mprGetBufStart(buf), modified,
        tx->cache->serverLifespan, 0, 0);
}


PUBLIC ssize httpWriteCached(HttpConn *conn)
{
    MprTime     modified;
    cchar       *cacheKey, *data, *content;

    if (!conn->tx->cache) {
        return MPR_ERR_CANT_FIND;
    }
    cacheKey = makeCacheKey(conn);
    if ((content = mprReadCache(conn->host->responseCache, cacheKey, &modified, 0)) == 0) {
        httpTrace(conn, "cache.none", "context", "msg:'No response data in cache',key:'%s'", cacheKey);
        return 0;
    }
    httpTrace(conn, "cache.cached", "context", "msg:'Used cached response',key:'%s'", cacheKey);
    data = setHeadersFromCache(conn, content);
    httpSetHeaderString(conn, "Etag", mprGetMD5(cacheKey));
    httpSetHeaderString(conn, "Last-Modified", mprFormatUniversalTime(MPR_HTTP_DATE, modified));
    conn->tx->cacheBuffer = 0;
    httpWriteString(conn->writeq, data);
    httpFinalizeOutput(conn);
    return slen(data);
}


PUBLIC ssize httpUpdateCache(HttpConn *conn, cchar *uri, cchar *data, MprTicks lifespan)
{
    cchar   *key;
    ssize   len;

    len = slen(data);
    if (len > conn->limits->cacheItemSize) {
        return MPR_ERR_WONT_FIT;
    }
    if (lifespan <= 0) {
        lifespan = conn->rx->route->lifespan;
    }
    key = sfmt("http::response::%s", uri);
    if (data == 0 || lifespan <= 0) {
        mprRemoveCache(conn->host->responseCache, key);
        return 0;
    }
    return mprWriteCache(conn->host->responseCache, key, data, 0, lifespan, 0, 0);
}


/*
    Add cache configuration to the route. This can be called multiple times.
    Uris, extensions and methods may optionally provide a space or comma separated list of items.
    If URI is NULL or "*", cache all URIs for this route. Otherwise, cache only the given URIs.
    The URIs may contain an ordered set of request parameters. For example: "/user/show?name=john&posts=true"
    Note: the URI should not include the route prefix (scriptName)
    The extensions should not contain ".". The methods may contain "*" for all methods.
 */
PUBLIC void httpAddCache(HttpRoute *route, cchar *methods, cchar *uris, cchar *extensions, cchar *types,
        MprTicks clientLifespan, MprTicks serverLifespan, int flags)
{
    HttpCache   *cache;
    char        *item, *tok;

    cache = 0;
    if (!route->caching) {
        if (route->handler) {
            mprLog("error http cache", 0, 
                "Caching handler disabled because SetHandler used in route %s. Use AddHandler instead", route->name);
        }
        httpAddRouteHandler(route, "cacheHandler", NULL);
        httpAddRouteFilter(route, "cacheFilter", "", HTTP_STAGE_TX);
        route->caching = mprCreateList(0, MPR_LIST_STABLE);

    } else if (flags & HTTP_CACHE_RESET) {
        route->caching = mprCreateList(0, MPR_LIST_STABLE);

    } else if (route->parent && route->caching == route->parent->caching) {
        route->caching = mprCloneList(route->parent->caching);
    }
    if ((cache = mprAllocObj(HttpCache, manageHttpCache)) == 0) {
        return;
    }
    if (extensions) {
        cache->extensions = mprCreateHash(0, MPR_HASH_STABLE);
        for (item = stok(sclone(extensions), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            if (smatch(item, "*")) {
                extensions = 0;
            } else {
                mprAddKey(cache->extensions, item, cache);
            }
        }
    } else if (types) {
        cache->types = mprCreateHash(0, MPR_HASH_STABLE);
        for (item = stok(sclone(types), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            if (smatch(item, "*")) {
                extensions = 0;
            } else {
                mprAddKey(cache->types, item, cache);
            }
        }
    }
    if (methods) {
        cache->methods = mprCreateHash(0, MPR_HASH_CASELESS | MPR_HASH_STABLE);
        for (item = stok(sclone(methods), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            if (smatch(item, "*")) {
                methods = 0;
            } else {
                mprAddKey(cache->methods, item, cache);
            }
        }
    }
    if (uris) {
        cache->uris = mprCreateHash(0, MPR_HASH_STABLE);
        for (item = stok(sclone(uris), " \t,", &tok); item; item = stok(0, " \t,", &tok)) {
            mprAddKey(cache->uris, item, cache);
            if (schr(item, '?')) {
                flags |= HTTP_CACHE_UNIQUE;
            }
        }
    }
    if (clientLifespan <= 0) {
        clientLifespan = route->lifespan;
    }
    cache->clientLifespan = clientLifespan;
    if (serverLifespan <= 0) {
        serverLifespan = route->lifespan;
    }
    cache->serverLifespan = serverLifespan;
    cache->flags = flags;
    mprAddItem(route->caching, cache);
}


static void manageHttpCache(HttpCache *cache, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cache->extensions);
        mprMark(cache->methods);
        mprMark(cache->types);
        mprMark(cache->uris);
    }
}


static char *makeCacheKey(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (conn->tx->cache->flags & HTTP_CACHE_UNIQUE) {
        return sfmt("http::response::%s%s?%s", rx->route->prefix, rx->pathInfo, httpGetParamsString(conn));
    } else {
        return sfmt("http::response::%s%s", rx->route->prefix, rx->pathInfo);
    }
}


/*
    Parse cached content of the form:  headers \n\n data
    Set headers in the current request and return a reference to the data portion
 */
static cchar *setHeadersFromCache(HttpConn *conn, cchar *content)
{
    cchar   *data;
    char    *header, *headers, *key, *value, *tok;

    if ((data = strstr(content, "\n\n")) == 0) {
        data = content;
    } else {
        headers = snclone(content, data - content);
        data += 2;
        for (header = stok(headers, "\n", &tok); header; header = stok(NULL, "\n", &tok)) {
            key = stok(header, ": ", &value);
            if (smatch(key, "X-Status")) {
                conn->tx->status = (int) stoi(value);
            } else {
                httpAddHeaderString(conn, key, value);
            }
        }
    }
    return data;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/chunkFilter.c ************/


/*
    chunkFilter.c - Transfer chunk endociding filter.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static int matchChunk(HttpConn *conn, HttpRoute *route, int dir);
static int openChunk(HttpQueue *q);
static void outgoingChunkService(HttpQueue *q);
static void setChunkPrefix(HttpQueue *q, HttpPacket *packet);

/*********************************** Code *************************************/
/* 
   Loadable module initialization
 */
PUBLIC int httpOpenChunkFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("chunkFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->chunkFilter = filter;
    filter->match = matchChunk; 
    filter->open = openChunk; 
    filter->outgoingService = outgoingChunkService; 
    return 0;
}


/*
    This is called twice: once for TX and once for RX
 */
static int matchChunk(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpTx  *tx;

    tx = conn->tx;

    if (conn->upgraded || (httpClientConn(conn) && tx->parsedUri && tx->parsedUri->webSockets)) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (dir & HTTP_STAGE_TX) {
        /* 
            If content length is defined, don't need chunking. Also disable chunking if explicitly turned off vi 
            the X_APPWEB_CHUNK_SIZE header which may set the chunk size to zero.
         */
        if (tx->length >= 0 || tx->chunkSize == 0) {
            return HTTP_ROUTE_OMIT_FILTER;
        }
        return HTTP_ROUTE_OK;
    } else {
        return HTTP_ROUTE_OK;
    }
}


static int openChunk(HttpQueue *q)
{
    q->packetSize = min(q->conn->limits->bufferSize, q->max);
    return 0;
}


/*
    Filter chunk headers and leave behind pure data. This is called for chunked and unchunked data.
    Chunked data format is:
        Chunk spec <CRLF>
        Data <CRLF>
        Chunk spec (size == 0) <CRLF>
        <CRLF>
    Chunk spec is: "HEX_COUNT; chunk length DECIMAL_COUNT\r\n". The "; chunk length DECIMAL_COUNT is optional.
    As an optimization, use "\r\nSIZE ...\r\n" as the delimiter so that the CRLF after data does not special consideration.
    Achive this by parseHeaders reversing the input start by 2.

    Return number of bytes available to read.
    NOTE: may set rx->eof and return 0 bytes on EOF.
 */
PUBLIC ssize httpFilterChunkData(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpRx      *rx;
    MprBuf      *buf;
    ssize       chunkSize;
    char        *start, *cp;
    int         bad;

    if (!packet) {
        return 0;
    }
    conn = q->conn;
    rx = conn->rx;
    buf = packet->content;
    assert(buf);

    switch (rx->chunkState) {
    case HTTP_CHUNK_UNCHUNKED:
        assert(0);
        return 0;

    case HTTP_CHUNK_DATA:
        if (rx->remainingContent > 0) {
            return (ssize) min(rx->remainingContent, mprGetBufLength(buf));
        }
        /* End of chunk - prep for the next chunk */
        rx->remainingContent = ME_MAX_BUFFER;
        rx->chunkState = HTTP_CHUNK_START;
        /* Fall through */

    case HTTP_CHUNK_START:
        /*
            Validate:  "\r\nSIZE.*\r\n"
         */
        if (mprGetBufLength(buf) < 5) {
            return 0;
        }
        start = mprGetBufStart(buf);
        bad = (start[0] != '\r' || start[1] != '\n');
        for (cp = &start[2]; cp < buf->end && *cp != '\n'; cp++) {}
        if (cp >= buf->end || (*cp != '\n' && (cp - start) < 80)) {
            return 0;
        }
        bad += (cp[-1] != '\r' || cp[0] != '\n');
        if (bad) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
            return 0;
        }
        chunkSize = (int) stoiradix(&start[2], 16, NULL);
        if (!isxdigit((uchar) start[2]) || chunkSize < 0) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
            return 0;
        }
        if (chunkSize == 0) {
            /*
                Last chunk. Consume the final "\r\n".
             */
            if ((cp + 2) >= buf->end) {
                return 0;
            }
            cp += 2;
            bad += (cp[-1] != '\r' || cp[0] != '\n');
            if (bad) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad final chunk specification");
                return 0;
            }
        }
        mprAdjustBufStart(buf, (cp - start + 1));
        /* Remaining content is set to the next chunk size */
        rx->remainingContent = chunkSize;
        rx->chunkState = (chunkSize == 0) ? HTTP_CHUNK_EOF : HTTP_CHUNK_DATA;
        return min(chunkSize, mprGetBufLength(buf));

    default:
        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk state %d", rx->chunkState);
    }
    return 0;
}


static void outgoingChunkService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet, *finalChunk;
    HttpTx      *tx;
    cchar       *value;

    conn = q->conn;
    tx = conn->tx;

    if (!(q->flags & HTTP_QUEUE_SERVICED)) {
        /*
            If we don't know the content length (tx->length < 0) and if the last packet is the end packet. Then
            we have all the data. Thus we can determine the actual content length and can bypass the chunk handler.
         */
        if (tx->length < 0 && (value = mprLookupKey(tx->headers, "Content-Length")) != 0) {
            tx->length = stoi(value);
        }
        if (tx->length < 0 && tx->chunkSize < 0) {
            if (q->last->flags & HTTP_PACKET_END) {
                if (q->count > 0) {
                    tx->length = q->count;
                }
            } else {
                tx->chunkSize = min(conn->limits->chunkSize, q->max);
            }
        }
        if (tx->flags & HTTP_TX_USE_OWN_HEADERS || conn->http10) {
            tx->chunkSize = -1;
        }
    }
    if (tx->chunkSize <= 0 || conn->upgraded) {
        httpDefaultOutgoingServiceStage(q);
    } else {
        for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
            if (packet->flags & HTTP_PACKET_DATA) {
                httpPutBackPacket(q, packet);
                httpJoinPackets(q, tx->chunkSize);
                packet = httpGetPacket(q);
                if (httpGetPacketLength(packet) > tx->chunkSize) {
                    httpResizePacket(q, packet, tx->chunkSize);
                }
            }
            if (!httpWillNextQueueAcceptPacket(q, packet)) {
                httpPutBackPacket(q, packet);
                return;
            }
            if (packet->flags & HTTP_PACKET_DATA) {
                setChunkPrefix(q, packet);

            } else if (packet->flags & HTTP_PACKET_END) {
                /* Insert a packet for the final chunk */
                finalChunk = httpCreateDataPacket(0);
                setChunkPrefix(q, finalChunk);
                httpPutPacketToNext(q, finalChunk);
            }
            httpPutPacketToNext(q, packet);
        }
    }
}


static void setChunkPrefix(HttpQueue *q, HttpPacket *packet)
{
    if (packet->prefix) {
        return;
    }
    packet->prefix = mprCreateBuf(32, 32);
    /*
        NOTE: prefixes don't count in the queue length. No need to adjust q->count
     */
    if (httpGetPacketLength(packet)) {
        mprPutToBuf(packet->prefix, "\r\n%zx\r\n", httpGetPacketLength(packet));
    } else {
        mprPutStringToBuf(packet->prefix, "\r\n0\r\n\r\n");
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/client.c ************/


/*
    client.c -- Client side specific support.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************* Forwards ***********************************/

static void setDefaultHeaders(HttpConn *conn);

/*********************************** Code *************************************/

static HttpConn *openConnection(HttpConn *conn, struct MprSsl *ssl)
{
    Http        *http;
    HttpUri     *uri;
    MprSocket   *sp;
    char        *ip;
    int         port, rc;

    assert(conn);

    http = conn->http;
    uri = conn->tx->parsedUri;

    if (!uri->host) {
        ip = (http->proxyHost) ? http->proxyHost : http->defaultClientHost;
        port = (http->proxyHost) ? http->proxyPort : http->defaultClientPort;
    } else {
        ip = (http->proxyHost) ? http->proxyHost : uri->host;
        port = (http->proxyHost) ? http->proxyPort : uri->port;
    }
    if (port == 0) {
        port = (uri->secure) ? 443 : 80;
    }
    if (conn && conn->sock) {
        if (conn->keepAliveCount-- <= 0 || port != conn->port || strcmp(ip, conn->ip) != 0 ||
                uri->secure != (conn->sock->ssl != 0) || conn->sock->ssl != ssl) {
            /* 
                Cannot reuse current socket. Close and open a new one below.
             */
            mprCloseSocket(conn->sock, 0);
            conn->sock = 0;
        } else {
            httpTrace(conn, "connection.reuse", "context", "keepAlive:%d", conn->keepAliveCount);
        }
    }
    if (conn->sock) {
        return conn;
    }

    /*
        New socket
     */
    if ((sp = mprCreateSocket()) == 0) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Cannot create socket for %s", uri->uri);
        return 0;
    }
    if ((rc = mprConnectSocket(sp, ip, port, MPR_SOCKET_NODELAY)) < 0) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Cannot open socket on %s:%d", ip, port);
        return 0;
    }
    conn->sock = sp;
    conn->ip = sclone(ip);
    conn->port = port;
    conn->keepAliveCount = (conn->limits->keepAliveMax) ? conn->limits->keepAliveMax : 0;

#if ME_COM_SSL
    /*
        Must be done even if using keep alive for repeat SSL requests
     */
    if (uri->secure) {
        char *peerName;
        if (ssl == 0) {
            ssl = mprCreateSsl(0);
        }
        peerName = isdigit(uri->host[0]) ? 0 : uri->host;
        if (mprUpgradeSocket(sp, ssl, peerName) < 0) {
            conn->errorMsg = sp->errorMsg;
            httpTrace(conn, "connection.upgrade.error", "error", "msg:'Cannot perform SSL upgrade. %s'", conn->errorMsg);
            return 0;
        }
        if (sp->peerCert) {
            httpTrace(conn, "context", "connection.ssl",
                "msg:'Connection secured with peer certificate', " \
                "secure:true,cipher:'%s',peerName:'%s',subject:'%s',issuer:'%s'",
                sp->cipher, sp->peerName, sp->peerCert, sp->peerCertIssuer);
        }
    }
#endif
#if ME_HTTP_WEB_SOCKETS
    if (uri->webSockets && httpUpgradeWebSocket(conn) < 0) {
        conn->errorMsg = sp->errorMsg;
        return 0;
    }
#endif
    httpTrace(conn, "connection.peer", "context", "peer:'%s:%d'", conn->ip, conn->port);
    return conn;
}


static void setDefaultHeaders(HttpConn *conn)
{
    HttpAuthType    *ap;

    assert(conn);

    if (smatch(conn->protocol, "HTTP/1.0")) {
        conn->http10 = 1;
    }
    if (conn->username && conn->authType) {
        if ((ap = httpLookupAuthType(conn->authType)) != 0) {
            if ((ap->setAuth)(conn, conn->username, conn->password)) {
                conn->authRequested = 1;
            }
        }
    }
    if (conn->port != 80 && conn->port != 443) {
        httpAddHeader(conn, "Host", "%s:%d", conn->ip, conn->port);
    } else {
        httpAddHeaderString(conn, "Host", conn->ip);
    }
    httpAddHeaderString(conn, "Accept", "*/*");
    if (conn->keepAliveCount > 0) {
        httpSetHeaderString(conn, "Connection", "Keep-Alive");
    } else {
        httpSetHeaderString(conn, "Connection", "close");
    }
}


PUBLIC int httpConnect(HttpConn *conn, cchar *method, cchar *uri, struct MprSsl *ssl)
{
    assert(conn);
    assert(method && *method);
    assert(uri && *uri);

    if (httpServerConn(conn)) {
        httpError(conn, HTTP_CODE_BAD_GATEWAY, "Cannot call connect in a server");
        return MPR_ERR_BAD_STATE;
    }
    if (conn->tx == 0 || conn->state != HTTP_STATE_BEGIN) {
        /* WARNING: this will erase headers */
        httpPrepClientConn(conn, 0);
    }
    assert(conn->state == HTTP_STATE_BEGIN);
    conn->tx->parsedUri = httpCreateUri(uri, HTTP_COMPLETE_URI_PATH);

    if (openConnection(conn, ssl) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    conn->authRequested = 0;
    conn->tx->method = supper(method);
    conn->startMark = mprGetHiResTicks();
    /*
        The receive pipeline is created when parsing the response in parseIncoming()
     */
    httpCreateTxPipeline(conn, conn->http->clientRoute);
    httpSetState(conn, HTTP_STATE_CONNECTED);
    setDefaultHeaders(conn);
    return 0;
}


/*
    Check the response for authentication failures and redirections. Return true if a retry is requried.
 */
PUBLIC bool httpNeedRetry(HttpConn *conn, char **url)
{
    HttpRx          *rx;
    HttpTx          *tx;
    HttpAuthType    *authType;

    assert(conn->rx);

    *url = 0;
    rx = conn->rx;
    tx = conn->tx;

    if (conn->state < HTTP_STATE_FIRST) {
        return 0;
    }
    if (rx->status == HTTP_CODE_UNAUTHORIZED) {
        if (conn->username == 0 || conn->authType == 0) {
            httpError(conn, rx->status, "Authentication required");

        } else if (conn->authRequested && smatch(conn->authType, tx->authType)) {
            httpError(conn, rx->status, "Authentication failed");
        } else {
            assert(httpClientConn(conn));
            if (conn->authType && (authType = httpLookupAuthType(conn->authType)) != 0) {
                (authType->parseAuth)(conn, NULL, NULL);
            }
            return 1;
        }
    } else if (HTTP_CODE_MOVED_PERMANENTLY <= rx->status && rx->status <= HTTP_CODE_MOVED_TEMPORARILY &&
            conn->followRedirects) {
        if (rx->redirect) {
            *url = rx->redirect;
            return 1;
        }
        httpError(conn, rx->status, "Missing location header");
        return 0;
    }
    return 0;
}


/*
    Set the request as being a multipart mime upload. This defines the content type and defines a multipart mime boundary
 */
PUBLIC void httpEnableUpload(HttpConn *conn)
{
    conn->boundary = sfmt("--BOUNDARY--%lld", conn->http->now);
    httpSetHeader(conn, "Content-Type", "multipart/form-data; boundary=%s", &conn->boundary[2]);
}


/*
    Read data. If sync mode, this will block. If async, will never block.
    Will return what data is available up to the requested size.
    Timeout in milliseconds to wait. Set to -1 to use the default inactivity timeout. Set to zero to wait forever.
    Returns a count of bytes read. Returns zero if no data. EOF if returns zero and conn->state is > HTTP_STATE_CONTENT.
 */
PUBLIC ssize httpReadBlock(HttpConn *conn, char *buf, ssize size, MprTicks timeout, int flags)
{
    HttpPacket  *packet;
    HttpQueue   *q;
    MprBuf      *content;
    MprTicks    start, delay;
    ssize       nbytes, len;
    int64       dispatcherMark;

    q = conn->readq;
    assert(q->count >= 0);
    assert(size >= 0);

    if (flags == 0) {
        flags = conn->async ? HTTP_NON_BLOCK : HTTP_BLOCK;
    }
    if (timeout < 0) {
        timeout = conn->limits->inactivityTimeout;
    } else if (timeout == 0) {
        timeout = MPR_MAX_TIMEOUT;
    }
    if (flags & HTTP_BLOCK) {
        start = conn->http->now;
        dispatcherMark = mprGetEventMark(conn->dispatcher);
        while (q->count <= 0 && !conn->error && (conn->state <= HTTP_STATE_CONTENT)) {
            if (httpRequestExpired(conn, -1)) {
                break;
            }
            delay = min(conn->limits->inactivityTimeout, mprGetRemainingTicks(start, timeout));
            httpEnableConnEvents(conn);
            mprWaitForEvent(conn->dispatcher, delay, dispatcherMark);
            if (mprGetRemainingTicks(start, timeout) <= 0) {
                break;
            }
            dispatcherMark = mprGetEventMark(conn->dispatcher);
        }
    }
    for (nbytes = 0; size > 0 && q->count > 0; ) {
        if ((packet = q->first) == 0) {
            break;
        }
        content = packet->content;
        len = mprGetBufLength(content);
        len = min(len, size);
        assert(len <= q->count);
        if (len > 0) {
            len = mprGetBlockFromBuf(content, buf, len);
            assert(len <= q->count);
        }
        buf += len;
        size -= len;
        q->count -= len;
        assert(q->count >= 0);
        nbytes += len;
        if (mprGetBufLength(content) == 0) {
            httpGetPacket(q);
        }
        if (flags & HTTP_NON_BLOCK) {
            break;
        }
    }
    assert(q->count >= 0);
    if (nbytes < size) {
        buf[nbytes] = '\0';
    }
    return nbytes;
}


/*
    Read with standard connection timeouts and in blocking mode for clients, non-blocking for server-side
 */
PUBLIC ssize httpRead(HttpConn *conn, char *buf, ssize size)
{
    return httpReadBlock(conn, buf, size, -1, 0);
}


PUBLIC char *httpReadString(HttpConn *conn)
{
    HttpRx      *rx;
    ssize       sofar, nbytes, remaining;
    char        *content;

    rx = conn->rx;
    remaining = (ssize) min(MAXSSIZE, rx->length);

    if (remaining > 0) {
        if ((content = mprAlloc(remaining + 1)) == 0) {
            return 0;
        }
        sofar = 0;
        while (remaining > 0) {
            nbytes = httpRead(conn, &content[sofar], remaining);
            if (nbytes < 0) {
                return 0;
            }
            sofar += nbytes;
            remaining -= nbytes;
        }
    } else {
        content = mprAlloc(ME_MAX_BUFFER);
        sofar = 0;
        while (1) {
            nbytes = httpRead(conn, &content[sofar], ME_MAX_BUFFER);
            if (nbytes < 0) {
                return 0;
            } else if (nbytes == 0) {
                break;
            }
            sofar += nbytes;
            content = mprRealloc(content, sofar + ME_MAX_BUFFER);
        }
    }
    content[sofar] = '\0';
    return content;
}


/*
    Convenience method to issue a client http request.
    Assumes the Mpr and Http services are created and initialized.
 */
PUBLIC HttpConn *httpRequest(cchar *method, cchar *uri, cchar *data, char **err)
{
    HttpConn        *conn;
    MprDispatcher   *dispatcher;
    ssize           len;

    if (err) {
        *err = 0;
    }
    dispatcher = mprCreateDispatcher("httpRequest", MPR_DISPATCHER_AUTO);
    mprStartDispatcher(dispatcher);

    conn = httpCreateConn(NULL, dispatcher);
    mprAddRoot(conn);

    /*
       Open a connection to issue the request. Then finalize the request output - this forces the request out.
     */
    if (httpConnect(conn, method, uri, NULL) < 0) {
        mprRemoveRoot(conn);
        httpDestroyConn(conn);
        *err = sfmt("Cannot connect to %s", uri);
        return 0;
    }
    if (data) {
        len = slen(data);
        if (httpWriteBlock(conn->writeq, data, len, HTTP_BLOCK) != len) {
            *err = sclone("Cannot write request body data");
        }
    }
    httpFinalizeOutput(conn);
    if (httpWait(conn, HTTP_STATE_CONTENT, MPR_MAX_TIMEOUT) < 0) {
        mprRemoveRoot(conn);
        httpDestroyConn(conn);
        *err = sclone("No response");
        return 0;
    }
    mprRemoveRoot(conn);
    return conn;
}


static int blockingFileCopy(HttpConn *conn, cchar *path)
{
    MprFile     *file;
    char        buf[ME_MAX_BUFFER];
    ssize       bytes, nbytes, offset;

    file = mprOpenFile(path, O_RDONLY | O_BINARY, 0);
    if (file == 0) {
        mprLog("error http client", 0, "Cannot open %s", path);
        return MPR_ERR_CANT_OPEN;
    }
    mprAddRoot(file);
    while ((bytes = mprReadFile(file, buf, sizeof(buf))) > 0) {
        offset = 0;
        while (bytes > 0) {
            if ((nbytes = httpWriteBlock(conn->writeq, &buf[offset], bytes, HTTP_BLOCK)) < 0) {
                mprCloseFile(file);
                mprRemoveRoot(file);
                return MPR_ERR_CANT_WRITE;
            }
            bytes -= nbytes;
            offset += nbytes;
            assert(bytes >= 0);
        }
    }
    httpFlushQueue(conn->writeq, HTTP_BLOCK);
    mprCloseFile(file);
    mprRemoveRoot(file);
    return 0;
}


/*
    Write upload data. This routine blocks. If you need non-blocking ... cut and paste.
 */
PUBLIC ssize httpWriteUploadData(HttpConn *conn, MprList *fileData, MprList *formData)
{
    char    *path, *pair, *key, *value, *name;
    cchar   *type;
    ssize   rc;
    int     next;

    rc = 0;
    if (formData) {
        for (rc = next = 0; rc >= 0 && (pair = mprGetNextItem(formData, &next)) != 0; ) {
            key = stok(sclone(pair), "=", &value);
            rc += httpWrite(conn->writeq, "%s\r\nContent-Disposition: form-data; name=\"%s\";\r\n", conn->boundary, key);
            rc += httpWrite(conn->writeq, "Content-Type: application/x-www-form-urlencoded\r\n\r\n%s\r\n", value);
        }
    }
    if (fileData) {
        for (rc = next = 0; rc >= 0 && (path = mprGetNextItem(fileData, &next)) != 0; ) {
            if (!mprPathExists(path, R_OK)) {
                httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot open %s", path);
                return MPR_ERR_CANT_OPEN;
            }
            name = mprGetPathBase(path);
            rc += httpWrite(conn->writeq, "%s\r\nContent-Disposition: form-data; name=\"file%d\"; filename=\"%s\"\r\n",
                conn->boundary, next - 1, name);
            if ((type = mprLookupMime(MPR->mimeTypes, path)) != 0) {
                rc += httpWrite(conn->writeq, "Content-Type: %s\r\n", mprLookupMime(MPR->mimeTypes, path));
            }
            httpWrite(conn->writeq, "\r\n");
            if (blockingFileCopy(conn, path) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            rc += httpWrite(conn->writeq, "\r\n");
        }
    }
    rc += httpWrite(conn->writeq, "%s--\r\n--", conn->boundary);
    return rc;
}


/*
    Wait for the connection to reach a given state.
    Should only be used on the client side.
    @param state Desired state. Set to zero if you want to wait for one I/O event.
    @param timeout Timeout in msec. If timeout is zero, wait forever. If timeout is < 0, use default inactivity
        and duration timeouts.
 */
PUBLIC int httpWait(HttpConn *conn, int state, MprTicks timeout)
{
    MprTicks    delay, start;
    int64       dispatcherMark;
    int         justOne;

    if (conn->endpoint) {
        assert(!conn->endpoint);
        return MPR_ERR_BAD_STATE;
    }
    if (conn->state <= HTTP_STATE_BEGIN) {
        return MPR_ERR_BAD_STATE;
    }
    if (state == 0) {
        /* Wait for just one I/O event */
        state = HTTP_STATE_FINALIZED;
        justOne = 1;
    } else {
        justOne = 0;
    }
    if (conn->error) {
        if (conn->state >= state) {
            return 0;
        }
        return MPR_ERR_BAD_STATE;
    }
    if (timeout < 0) {
        timeout = conn->limits->requestTimeout;
    } else if (timeout == 0) {
        timeout = MPR_MAX_TIMEOUT;
    }
    if (state > HTTP_STATE_CONTENT) {
        httpFinalizeOutput(conn);
    }
    start = conn->http->now;
    dispatcherMark = mprGetEventMark(conn->dispatcher);
    while (conn->state < state && !conn->error && !mprIsSocketEof(conn->sock)) {
        if (httpRequestExpired(conn, -1)) {
            return MPR_ERR_TIMEOUT;
        }
        httpEnableConnEvents(conn);
        delay = min(conn->limits->inactivityTimeout, mprGetRemainingTicks(start, timeout));
        delay = max(delay, 0);
        mprWaitForEvent(conn->dispatcher, delay, dispatcherMark);
        if (justOne || mprGetRemainingTicks(start, timeout) <= 0) {
            break;
        }
        dispatcherMark = mprGetEventMark(conn->dispatcher);
    }
    if (conn->error) {
        return MPR_ERR_NOT_READY;
    }
    if (conn->state < state) {
        if (mprGetRemainingTicks(start, timeout) <= 0) {
            return MPR_ERR_TIMEOUT;
        }
        if (!justOne) {
            return MPR_ERR_CANT_READ;
        }
    }
    conn->lastActivity = conn->http->now;
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/config.c ************/


/*
    config.c -- Http JSON Configuration File Parsing

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/*********************************** Includes *********************************/



/************************************ Defines *********************************/

#define ITERATE_CONFIG(route, obj, child, index) \
    index = 0, child = obj ? obj->children: 0; obj && index < obj->length && !route->error; child = child->next, index++

/************************************ Forwards ********************************/
static void parseAll(HttpRoute *route, cchar *key, MprJson *prop);
static void parseAuthRoles(HttpRoute *route, cchar *key, MprJson *prop);
static void parseAuthStore(HttpRoute *route, cchar *key, MprJson *prop);
static void postParse(HttpRoute *route);
static void parseRoutes(HttpRoute *route, cchar *key, MprJson *prop);

/************************************** Code **********************************/

PUBLIC HttpParseCallback httpAddConfig(cchar *key, HttpParseCallback callback)
{
    HttpParseCallback   prior;

    prior = mprLookupKey(HTTP->parsers, key);
    mprAddKey(HTTP->parsers, key, callback);
    return prior;
}


static void httpParseError(HttpRoute *route, cchar *fmt, ...)
{
    HttpRoute   *rp;
    va_list     args;
    char        *msg;

    va_start(args, fmt);
    msg = sfmtv(fmt, args);
    mprLog("error http config", 0, "%s", msg);
    va_end(args);
    route->error = 1;
    for (rp = route; rp; rp = rp->parent) {
        rp->error = 1;
    }
}


/*
    Convert a JSON string to a space-separated C string
 */
static cchar *getList(MprJson *prop)
{
    char    *cp, *p;

    if (prop == 0) {
        return 0;
    }
    if ((cp = mprJsonToString(prop, 0)) == 0) {
        return 0;
    }
    if (*cp == '[') {
        cp = strim(cp, "[]", 0);
    }
    for (p = cp; *p; p++) {
        if (*p == '"' || *p == ',') {
            *p = ' ';
        }
    }
    if (*cp == ' ') {
        cp = strim(cp, " \t", 0);
    }
    return cp;
}


static int getint(cchar *value)
{
    int64   num;

    num = httpGetNumber(value);
    if (num >= MAXINT) {
        num = MAXINT;
    }
    return (int) num;
}


static int testConfig(HttpRoute *route, cchar *path)
{
    MprPath     cinfo;

    if (mprGetPathInfo(path, &cinfo) == 0) {
        if (route->config && cinfo.mtime > route->configLoaded) {
            route->config = 0;
        }
        route->configLoaded = cinfo.mtime;
    }
    if (route->config) {
        return 0;
    }
    if (!mprPathExists(path, R_OK)) {
        mprLog("error http config", 0, "Cannot find %s", path);
        return MPR_ERR_CANT_READ;
    }
    return 0;
}


/*
    Blend the app.modes[app.mode] into app
 */
static void blendMode(HttpRoute *route, MprJson *config)
{
    MprJson     *currentMode, *app;
    cchar       *mode;

    mode = mprGetJson(config, "app.mode");
    if (!mode) {
        mode = sclone("debug");
    }
    route->debug = smatch(mode, "debug");
    if ((currentMode = mprGetJsonObj(config, sfmt("app.modes.%s", mode))) != 0) {
        app = mprLookupJsonObj(config, "app");
        mprBlendJson(app, currentMode, MPR_JSON_OVERWRITE);
        mprSetJson(app, "app.mode", mode);
    }
}


PUBLIC int parseFile(HttpRoute *route, cchar *path)
{
    MprJson     *config;
    cchar       *data, *errorMsg;

    if ((data = mprReadPathContents(path, NULL)) == 0) {
        mprLog("error http config", 0, "Cannot read configuration from \"%s\"", path);
        return MPR_ERR_CANT_READ;
    }
    if ((config = mprParseJsonEx(data, 0, 0, 0, &errorMsg)) == 0) {
        mprLog("error http config", 0, "Cannot parse %s: error %s", path, errorMsg);
        return MPR_ERR_CANT_READ;
    }
    if (route->config == 0) {
        blendMode(route, config);
        route->config = config;
    }
    parseAll(route, 0, config);
    return 0;
}


PUBLIC int httpLoadConfig(HttpRoute *route, cchar *name)
{
    cchar       *path;

    lock(route);
    route->error = 0;

    path = mprJoinPath(route->home, name);
    if (testConfig(route, path) < 0) {
        unlock(route);
        return MPR_ERR_CANT_READ;
    }
    if (route->config) {
        unlock(route);
        return 0;
    }
    if (parseFile(route, path) < 0) {
        unlock(route);
        return MPR_ERR_CANT_READ;
    }
    postParse(route);

    if (route->error) {
        route->config = 0;
        unlock(route);
        return MPR_ERR_BAD_STATE;
    }
    unlock(route);
    return 0;
}


static void clientCopy(HttpRoute *route, MprJson *dest, MprJson *obj)
{
    MprJson     *child, *job, *jvalue;
    cchar       *key, *value;
    int         ji;

    for (ITERATE_CONFIG(route, obj, child, ji)) {
        if (child->type & MPR_JSON_OBJ) {
            job = mprCreateJson(MPR_JSON_OBJ);
            clientCopy(route, job, child);
            mprSetJsonObj(dest, child->name, job);
        } else {
            key = child->value;
            if (sends(key, "|time")) {
                key = stok(sclone(key), " \t|", NULL);
                if ((value = mprGetJson(route->config, key)) != 0) {
                    mprSetJson(dest, child->name, itos(httpGetTicks(value)));
                }
            } else {
                if ((jvalue = mprGetJsonObj(route->config, key)) != 0) {
                    mprSetJsonObj(dest, child->name, mprCloneJson(jvalue));
                }
            }
        }
    }
}


static void postParse(HttpRoute *route)
{
    Http        *http;
    HttpHost    *host;
    HttpRoute   *rp;
    MprJson     *mappings, *client;
    int         nextHost, nextRoute;

    if (route->error) {
        return;
    }
    http = route->http;
    route->mode = mprGetJson(route->config, "app.mode");

    /*
        Create a subset, optimized configuration to send to the client
     */
    if ((mappings = mprGetJsonObj(route->config, "app.client.mappings")) != 0) {
        client = mprCreateJson(MPR_JSON_OBJ);
        clientCopy(route, client, mappings);
        mprSetJson(client, "prefix", route->prefix);
        route->client = mprJsonToString(client, MPR_JSON_QUOTES);
    }
    httpAddHostToEndpoints(route->host);

    /*
        Ensure the host home directory is set and the file handler is defined
        Propagate the HttpRoute.client to all child routes.
     */
    for (nextHost = 0; (host = mprGetNextItem(http->hosts, &nextHost)) != 0; ) {
        for (nextRoute = 0; (rp = mprGetNextItem(host->routes, &nextRoute)) != 0; ) {
            if (!mprLookupKey(rp->extensions, "")) {
                if (!rp->handler) {
                    httpAddRouteHandler(rp, "fileHandler", "");
                    httpAddRouteIndex(rp, "index.html");
                }
            }
            if (rp->parent == route) {
                rp->client = route->client;
            }
        }
    }
}


/**************************************** Parser Callbacks ****************************************/


static void parseKey(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpParseCallback   parser;

    key = key ? sjoin(key, ".", prop->name, NULL) : prop->name;
    if ((parser = mprLookupKey(HTTP->parsers, key)) != 0) {
        parser(route, key, prop);
    }
}


static void parseAll(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *routes;
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        parseKey(route, key, child);
    }

    /*
        Property order is not guaranteed, so must ensure routes are processed after all outer properties.
     */
    if ((routes = mprGetJsonObj(prop, "routes")) != 0) {
        parseRoutes(route, key, routes);
    }
}


static void parseDirectories(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (smatch(child->name, "documents")) {
            httpSetRouteDocuments(route, child->value);
        } else if (smatch(child->name, "home")) {
            httpSetRouteHome(route, child->value);
        }
        httpSetDir(route, child->name, child->value);
    }
}


static void parseAuth(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (prop->type & MPR_JSON_STRING) {
        /* Permits auth: "app" to set the store */
        parseAuthStore(route, key, prop);
    } else if (prop->type == MPR_JSON_OBJ) {
        parseAll(route, key, prop);
    }
}


static void parseAuthAutoName(HttpRoute *route, cchar *key, MprJson *prop)
{
    /* Automatic login as this user. Password not required */
    httpSetAuthUsername(route->auth, prop->value);
}


/*
    Parse roles and compute abilities
 */
static void parseAuthAutoRoles(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprHash     *abilities;
    MprKey      *kp;
    MprJson     *child, *job;
    int         ji;

    if ((job = mprGetJsonObj(route->config, "app.http.auth.roles")) != 0) {
        parseAuthRoles(route, "app.http.auth.roles", job);
    }
    abilities = mprCreateHash(0, 0);
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpComputeRoleAbilities(route->auth, abilities, child->value);
    }
    if (mprGetHashLength(abilities) > 0) {
        job = mprCreateJson(MPR_JSON_ARRAY);
        for (ITERATE_KEYS(abilities, kp)) {
            mprSetJson(job, "$", kp->key);
        }
        mprSetJsonObj(route->config, "app.http.auth.auto.abilities", job);
    }
}


static void parseAuthLogin(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetAuthLogin(route->auth, prop->value);
}


static void parseAuthRealm(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetAuthRealm(route->auth, prop->value);
}


static void parseAuthRequireRoles(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpSetAuthRequiredAbilities(route->auth, child->value);
    }
}


static void parseAuthRequireUsers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (smatch(prop->value, "*")) {
            httpSetAuthAnyValidUser(route->auth);
        } else {
            httpSetAuthPermittedUsers(route->auth, prop->value);
        }
    } else if (prop->type & MPR_JSON_OBJ) {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (smatch(prop->value, "*")) {
                httpSetAuthAnyValidUser(route->auth);
                break;
            } else {
                httpSetAuthPermittedUsers(route->auth, getList(child));
            }
        }
    }
}


static void parseAuthRoles(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (httpAddRole(route->auth, child->name, getList(child)) < 0) {
            httpParseError(route, "Cannot add role %s", child->name);
            break;
        }
    }
}


static void parseAuthSessionCookie(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteCookie(route, prop->value);
}


static void parseAuthSessionVisibility(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteSessionVisibility(route, scaselessmatch(prop->value, "visible"));
}


static void parseAuthStore(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (httpSetAuthStore(route->auth, prop->value) < 0) {
        httpParseError(route, "The %s AuthStore is not available on this platform", prop->value);
    }
}


static void parseAuthType(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpAuth    *auth;
    cchar       *type;

    auth = route->auth;
    type = prop->value;

    if (httpSetAuthType(auth, type, 0) < 0) {
        httpParseError(route, "The %s AuthType is not available on this platform", type);
    }
    if (type && !smatch(type, "none")) {
        httpAddRouteCondition(route, "auth", 0, 0);
    }
    if (smatch(type, "basic") || smatch(type, "digest")) {
        /*
            Must not use cookies by default, otherwise, the client cannot logoff.
         */
        httpSetAuthSession(auth, 0);
    }
}


static void parseAuthUsers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *roles, *password;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        password = mprGetJson(child, "password");
        roles = getList(mprGetJsonObj(child, "roles"));
        if (httpAddUser(route->auth, child->name, password, roles) < 0) {
            httpParseError(route, "Cannot add user %s", child->name);
            break;
        }
        if (!route->auth->store) {
            httpSetAuthStore(route->auth, "config");
        }
    }
}


static void parseCache(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    MprTicks    clientLifespan, serverLifespan;
    cchar       *methods, *extensions, *uris, *mimeTypes, *client, *server;
    int         flags, ji;

    clientLifespan = serverLifespan = 0;
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        flags = 0;
        if ((client = mprGetJson(child, "client")) != 0) {
            flags |= HTTP_CACHE_CLIENT;
            clientLifespan = httpGetTicks(client);
        }
        if ((server = mprGetJson(child, "server")) != 0) {
            flags |= HTTP_CACHE_SERVER;
            serverLifespan = httpGetTicks(server);
        }
        methods = getList(mprGetJsonObj(child, "methods"));
        extensions = getList(mprGetJsonObj(child, "extensions"));
        uris = getList(mprGetJsonObj(child, "uris"));
        mimeTypes = getList(mprGetJsonObj(child, "mime"));

        if (smatch(mprGetJson(child, "unique"), "true")) {
            /* Uniquely cache requests with different params */
            flags |= HTTP_CACHE_UNIQUE;
        }
        if (smatch(mprGetJson(child, "manual"), "true")) {
            /* User must manually call httpWriteCache */
            flags |= HTTP_CACHE_MANUAL;
        }
        httpAddCache(route, methods, uris, extensions, mimeTypes, clientLifespan, serverLifespan, flags);
    }
}


static void parseContentCombine(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (smatch(child->value, "c")) {
            route->combine = 1;
            break;
        }
    }
}


static void parseContentCompress(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (mprGetJson(route->config, sfmt("app.http.content.minify[@ = '%s']", child->value))) {
            httpAddRouteMapping(route, child->value, "${1}.gz, min.${1}.gz, min.${1}");
        } else {
            httpAddRouteMapping(route, child->value, "${1}.gz");
        }
    }
}


#if DEPRECATED || 1
static void parseContentKeep(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (mprGetJson(prop, "[@=c]")) {
        route->keepSource = 1;
    }
}
#endif


static void parseContentMinify(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        /*
            Compressed and minified is handled in parseContentCompress
         */
        if (mprGetJson(route->config, sfmt("app.http.content.compress[@ = '%s']", child->value)) == 0) {
            httpAddRouteMapping(route, child->value, "min.${1}");
        }
    }
}


static void parseDatabase(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->database = prop->value;
}


static void parseDeleteUploads(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteAutoDelete(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseDomain(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetHostName(route->host, strim(prop->value, "http://", MPR_TRIM_START));
}


static void parseDocuments(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, X_OK)) {
        httpParseError(route, "Cannot locate documents directory %s", prop->value);
    } else {
        httpSetRouteDocuments(route, prop->value);
    }
}


static void parseErrors(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteErrorDocument(route, (int) stoi(prop->name), prop->value);
    }
}


static void parseFormatsResponse(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->responseFormat = prop->value;
}


static void parseHandler(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (httpSetRouteHandler(route, prop->value) < 0) {
        httpParseError(route, "Cannot add handler %s", prop->value);
    }
}

static void parseHeadersAdd(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, child->name, child->value);
    }
}


static void parseHeadersRemove(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (prop->type & MPR_JSON_ARRAY) {
            httpAddRouteResponseHeader(route, HTTP_ROUTE_REMOVE_HEADER, child->value, 0);
        } else {
            httpAddRouteResponseHeader(route, HTTP_ROUTE_REMOVE_HEADER, child->name, 0);
        }
    }
}


static void parseHeadersSet(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteResponseHeader(route, HTTP_ROUTE_SET_HEADER, child->name, child->value);
    }
}


static void parseHome(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, X_OK)) {
        httpParseError(route, "Cannot locate home directory %s", prop->value);
    } else {
        httpSetRouteHome(route, prop->value);
    }
}


static void parseIndexes(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    httpResetRouteIndexes(route);
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddRouteIndex(route, child->value);
    }
}


static void parseKeep(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->keepSource = (prop->type & MPR_JSON_TRUE) ? 1 : 0;
}


static void parseLanguages(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *path, *prefix, *suffix;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if ((prefix = mprGetJson(child, "prefix")) != 0) {
            httpAddRouteLanguageSuffix(route, child->name, child->value, HTTP_LANG_BEFORE);
        }
        if ((suffix = mprGetJson(child, "suffix")) != 0) {
            httpAddRouteLanguageSuffix(route, child->name, child->value, HTTP_LANG_AFTER);
        }
        if ((path = mprGetJson(child, "path")) != 0) {
            httpAddRouteLanguageDir(route, child->name, mprGetAbsPath(path));
        }
        if (smatch(mprGetJson(child, "default"), "default")) {
            httpSetRouteDefaultLanguage(route, child->name);
        }
    }
}


static void parseLimits(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpGraduateLimits(route, 0);
    parseAll(route, key, prop);
}


static void parseLimitsBuffer(HttpRoute *route, cchar *key, MprJson *prop)
{
    int     size;

    size = getint(prop->value);
    if (size > (1048576)) {
        size = 1048576;
    }
    route->limits->bufferSize = size;
}


static void parseLimitsCache(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetCacheLimits(route->host->responseCache, 0, 0, httpGetNumber(prop->value), 0);
}


static void parseLimitsCacheItem(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->cacheItemSize = getint(prop->value);
}


static void parseLimitsChunk(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->chunkSize = getint(prop->value);
}


static void parseLimitsClients(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->clientMax = getint(prop->value);
}


static void parseLimitsConnections(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->connectionsMax = getint(prop->value);
}


static void parseLimitsFiles(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetFilesLimit(getint(prop->value));
}


static void parseLimitsKeepAlive(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->keepAliveMax = getint(prop->value);
}


static void parseLimitsMemory(HttpRoute *route, cchar *key, MprJson *prop)
{
    ssize   maxMem;

    maxMem = (ssize) httpGetNumber(prop->value);
    mprSetMemLimits(maxMem / 100 * 85, maxMem, -1);
}


static void parseLimitsProcesses(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->processMax = getint(prop->value);
}


static void parseLimitsRequests(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->requestsPerClientMax = getint(prop->value);
}


static void parseLimitsRequestBody(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->receiveBodySize = httpGetNumber(prop->value);
}


static void parseLimitsRequestForm(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->receiveFormSize = httpGetNumber(prop->value);
}


static void parseLimitsRequestHeader(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->headerSize = getint(prop->value);
}


static void parseLimitsResponseBody(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->transmissionBodySize = httpGetNumber(prop->value);
}


static void parseLimitsSessions(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->sessionMax = getint(prop->value);
}


static void parseLimitsUri(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->uriSize = getint(prop->value);
}


static void parseLimitsUpload(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->uploadSize = httpGetNumber(prop->value);
}


static void parseLimitsWebSockets(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsMax = getint(prop->value);
}


static void parseLimitsWebSocketsMessage(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsMessageSize = getint(prop->value);
}


static void parseLimitsWebSocketsFrame(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsFrameSize = getint(prop->value);
}


static void parseLimitsWebSocketsPacket(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->limits->webSocketsPacketSize = getint(prop->value);
}


static void parseLimitsWorkers(HttpRoute *route, cchar *key, MprJson *prop)
{
    int     count;

    count = atoi(prop->value);
    if (count < 1) {
        count = MAXINT;
    }
    mprSetMaxWorkers(count);
}


static void parseMethods(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteMethods(route, getList(prop));
}


static void parseMode(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->mode = prop->value;
}


static void parseName(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteName(route, prop->value);
}


/*
    Match route only if param matches
 */
static void parseParams(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *name, *value;
    int         not, ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        name = mprGetJson(child, "name");
        value = mprGetJson(child, "value");
        not = smatch(mprGetJson(child, "equals"), "true") ? 0 : HTTP_ROUTE_NOT;
        httpAddRouteParam(route, name, value, not);
    }
}


static void parsePattern(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRoutePattern(route, prop->value, 0);
}


static void parsePipelineFilters(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *name, *extensions;
    int         flags, ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        if (child->type & MPR_JSON_STRING) {
            flags = HTTP_STAGE_RX | HTTP_STAGE_TX;
            extensions = 0;
            name = child->value;
        } else {
            name = mprGetJson(child, "name");
            extensions = getList(mprGetJsonObj(child, "extensions"));
#if KEEP
            direction = mprGetJson(child, "direction");
            flags |= smatch(direction, "input") ? HTTP_STAGE_RX : 0;
            flags |= smatch(direction, "output") ? HTTP_STAGE_TX : 0;
            flags |= smatch(direction, "both") ? HTTP_STAGE_RX | HTTP_STAGE_TX : 0;
#else
            flags = HTTP_STAGE_RX | HTTP_STAGE_TX;
#endif
        }
        if (httpAddRouteFilter(route, name, extensions, flags) < 0) {
            httpParseError(route, "Cannot add filter %s", name);
            break;
        }
    }
}


/*
    pipeline: {
        handlers: 'espHandler',                     //  For all extensions
        handlers: {
            espHandler: [ '*.esp, '*.xesp' ],
        },
    },
 */
static void parsePipelineHandlers(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (httpAddRouteHandler(route, prop->value, "") < 0) {
            httpParseError(route, "Cannot add handler %s", prop->value);
        }

    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (httpAddRouteHandler(route, child->name, getList(child)) < 0) {
                httpParseError(route, "Cannot add handler %s", child->name);
                break;
            }
        }
    }
}


static void parsePrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRoutePrefix(route, sjoin(route->prefix, prop->value, 0));
}


static void createRedirectAlias(HttpRoute *route, int status, cchar *from, cchar *to)
{
    HttpRoute   *alias;
    cchar       *pattern;

    if (from == 0 || *from == '\0') {
        from = "/";
    }
    if (sends(from, "/")) {
        pattern = sfmt("^%s%s(.*)$", route->prefix, from);
    } else {
        /* Add a non-capturing optional trailing "/" */
        pattern = sfmt("^%s%s(?:/)*(.*)$", route->prefix, from);
    }
    alias = httpCreateAliasRoute(route, pattern, 0, 0);
    httpSetRouteName(alias, "redirect");
    httpSetRouteMethods(alias, "*");
    httpSetRouteTarget(alias, "redirect", sfmt("%d %s/$1", status, to));
    if (sstarts(to, "https")) {
        httpAddRouteCondition(alias, "secure", to, HTTP_ROUTE_REDIRECT);
    }
    httpFinalizeRoute(alias);
}


static void parseRedirect(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *from, *status, *to;
    int         ji;

    if (prop->type & MPR_JSON_STRING) {
        if (smatch(prop->value, "secure") ||smatch(prop->value, "https://")) {
            httpAddRouteCondition(route, "secure", "https://", HTTP_ROUTE_REDIRECT);
        } else {
            createRedirectAlias(route, 0, "/", prop->value);
        }

    } else {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (child->type & MPR_JSON_STRING) {
                from = "/";
                to = child->value;
                status = "302";
            } else {
                from = mprGetJson(child, "from");
                to = mprGetJson(child, "to");
                status = mprGetJson(child, "status");
            }
            if (smatch(child->value, "secure")) {
                httpAddRouteCondition(route, "secure", "https://", HTTP_ROUTE_REDIRECT);
            } else {
                createRedirectAlias(route, (int) stoi(status), from, to);
            }
        }
    }
}


/*
    Create RESTful routes
 */
static void parseResources(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child, *groups, *singletons, *sets;
    int         ji;

    if ((sets = mprGetJsonObj(prop, "sets")) != 0) {
        for (ITERATE_CONFIG(route, sets, child, ji)) {
            httpAddRouteSet(route, child->value);
        }
    }
    if ((groups = mprGetJsonObj(prop, "groups")) != 0) {
        for (ITERATE_CONFIG(route, groups, child, ji)) {
            httpAddResourceGroup(route, route->serverPrefix, child->value);
        }
    }
    if ((singletons = mprGetJsonObj(prop, "singletons")) != 0) {
        for (ITERATE_CONFIG(route, singletons, child, ji)) {
            httpAddResource(route, route->serverPrefix, child->value);
        }
    }
}


PUBLIC HttpRouteSetProc httpDefineRouteSet(cchar *name, HttpRouteSetProc fn)
{
    HttpRouteSetProc    prior;

    prior = mprLookupKey(HTTP->routeSets, name);
    mprAddKey(HTTP->routeSets, name, fn);
    return prior;
}


PUBLIC void httpAddRouteSet(HttpRoute *route, cchar *set)
{
    HttpRouteSetProc    proc;

    if (set == 0 || *set == 0) {
        return;
    }
    if ((proc = mprLookupKey(route->http->routeSets, set)) != 0) {
        (proc)(route, set);
    } else {
        mprLog("error http config", 0, "Cannot find route set \"%s\"", set);
    }
}


static void setConfigDefaults(HttpRoute *route)
{
    route->mode = mprGetJson(route->config, "app.mode");
    if (smatch(route->mode, "debug")) {
        httpSetRouteShowErrors(route, 1);
        route->keepSource = 1;
    }
}


static void parseHttp(HttpRoute *route, cchar *key, MprJson *prop)
{

    setConfigDefaults(route);
    parseAll(route, key, prop);
}


/*
    Must only be called directly via parseHttp as all other http.* keys must have already been processed.
 */
static void parseRoutes(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    HttpRoute   *newRoute;
    cchar       *pattern;
    int         ji;

    if (route->loaded) {
        mprLog("warn http config", 1, "Skip reloading routes - must reboot if routes are modified");
        return;
    }
    if (prop->type & MPR_JSON_STRING) {
        httpAddRouteSet(route, prop->value);

    } else if (prop->type & MPR_JSON_ARRAY) {
        key = sreplace(key, ".routes", "");
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            if (child->type & MPR_JSON_STRING) {
                httpAddRouteSet(route, child->value);

            } else if (child->type & MPR_JSON_OBJ) {
                newRoute = 0;
                pattern = mprLookupJson(child, "pattern");
                if (pattern) {
                    newRoute = httpLookupRouteByPattern(route->host, pattern);
                    if (!newRoute) {
                        newRoute = httpCreateInheritedRoute(route);
                        httpSetRouteHost(newRoute, route->host);
                    }
                } else {
                    newRoute = route;
                }
                parseAll(newRoute, key, child);
                if (newRoute->error) {
                    break;
                }
                if (pattern) {
                    httpFinalizeRoute(newRoute);
                }
            }
        }
    }
}


static void parseScheme(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (sstarts(prop->value, "https")) {
        httpAddRouteCondition(route, "secure", 0, 0);
    }
}


/*
    The server collection is only parsed for utilities and not if hosted
 */
static void parseServer(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (route->http->flags & HTTP_UTILITY) {
        parseAll(route, key, prop);
    }
}


static void parseServerAccount(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar       *value;

    if ((value = mprGetJson(prop, "user")) != 0) {
        if (!smatch(value, "_unchanged_") && !mprGetDebugMode()) {
            httpSetGroupAccount(value);
        }
    }
    if ((value = mprGetJson(prop, "user")) != 0) {
        if (!smatch(value, "_unchanged_") && !mprGetDebugMode()) {
            httpSetUserAccount(value);
        }
    }
}


static void parseServerChroot(HttpRoute *route, cchar *key, MprJson *prop)
{
#if ME_UNIX_LIKE
    char        *home;

    home = httpMakePath(route, 0, prop->value);
    if (chdir(home) < 0) {
        httpParseError(route, "Cannot change working directory to %s", home);
        return;
    }
    if (route->http->flags & HTTP_UTILITY) {
        /* Not running a web server but rather a utility like the "esp" generator program */
        mprLog("info http config", 2, "Change directory to: \"%s\"", home);
    } else {
        if (chroot(home) < 0) {
            if (errno == EPERM) {
                httpParseError(route, "Must be super user to use chroot\n");
            } else {
                httpParseError(route, "Cannot change change root directory to %s, errno %d\n", home, errno);
            }
            return;
        }
        mprLog("info http config", 2, "Chroot to: \"%s\"", home);
    }
#else
    mprLog("info http config", 2, "Chroot directive not supported on this operating system\n");
#endif
}


static void parseServerDefenses(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        httpAddDefenseFromJson(child->name, 0, child);
    }
}


static void parseServerListen(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpEndpoint    *endpoint;
    HttpHost        *host;
    MprJson         *child;
    char            *ip;
    int             ji, port, secure;

    host = route->host;
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        mprParseSocketAddress(child->value, &ip, &port, &secure, 80);
        if (port == 0) {
            httpParseError(route, "Bad or missing port %d in Listen directive", port);
            return;
        }
        endpoint = httpCreateEndpoint(ip, port, NULL);
        if (!host->defaultEndpoint) {
            httpSetHostDefaultEndpoint(host, endpoint);
        }
        if (secure) {
            if (route->ssl == 0) {
                if (route->parent && route->parent->ssl) {
                    route->ssl = mprCloneSsl(route->parent->ssl);
                } else {
                    route->ssl = mprCreateSsl(1);
                }
            }
            httpSecureEndpoint(endpoint, route->ssl);
            if (!host->secureEndpoint) {
                httpSetHostSecureEndpoint(host, endpoint);
            }
        }
        /*
            Single stack networks cannot support IPv4 and IPv6 with one socket. So create a specific IPv6 endpoint.
            This is currently used by VxWorks and Windows versions prior to Vista (i.e. XP)
         */
        if (!schr(prop->value, ':') && mprHasIPv6() && !mprHasDualNetworkStack()) {
            mprAddItem(route->http->endpoints, httpCreateEndpoint("::", port, NULL));
            httpSecureEndpoint(endpoint, route->ssl);
        }
    }
}


/*
    log: {
        location: 'stdout',
        level: '2',
        backup: 5,
        anew: true,
        size: '10MB',
        timestamp: '1hr',
    }
 */
static void parseServerLog(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprTicks    timestamp;
    cchar       *location;
    ssize       size;
    int         level, anew, backup;

    if (mprGetCmdlineLogging()) {
        mprLog("warn http config", 4, "Already logging. Ignoring log configuration");
        return;
    }
    location = mprGetJson(prop, "location");
    level = (int) stoi(mprGetJson(prop, "level"));
    backup = (int) stoi(mprGetJson(prop, "backup"));
    anew = smatch(mprGetJson(prop, "anew"), "true");
    size = (ssize) httpGetNumber(mprGetJson(prop, "size"));
    timestamp = httpGetNumber(mprGetJson(prop, "timestamp"));

    if (size < HTTP_TRACE_MIN_LOG_SIZE) {
        size = HTTP_TRACE_MIN_LOG_SIZE;
    }
    if (location == 0) {
        httpParseError(route, "Missing location");
        return;
    }
    if (!smatch(location, "stdout") && !smatch(location, "stderr")) {
        location = httpMakePath(route, 0, location);
    }
    mprSetLogBackup(size, backup, anew ? MPR_LOG_ANEW : 0);

    if (mprStartLogging(location, 0) < 0) {
        httpParseError(route, "Cannot write to error log: %s", location);
        return;
    }
    mprSetLogLevel(level);
    mprLogConfig();
    if (timestamp) {
        httpSetTimestamp(timestamp);
    }
}


static void parseServerMonitors(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    MprTicks    period;
    cchar       *counter, *expression, *limit, *relation, *defenses;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        defenses = mprGetJson(child, "defenses");
        expression = mprGetJson(child, "expression");
        period = httpGetTicks(mprGetJson(child, "period"));

        if (!httpTokenize(route, expression, "%S %S %S", &counter, &relation, &limit)) {
            httpParseError(route, "Cannot add monitor: %s", prop->name);
            break;
        }
        if (httpAddMonitor(counter, relation, getint(limit), period, defenses) < 0) {
            httpParseError(route, "Cannot add monitor: %s", prop->name);
            break;
        }
    }
}


static void parseServerPrefix(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteServerPrefix(route, prop->value);
}


static void parseShowErrors(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteShowErrors(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseSource(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteSource(route, prop->value);
}


static void parseSsl(HttpRoute *route, cchar *key, MprJson *prop)
{
    HttpRoute   *parent;

    parent = route->parent;
    if (route->ssl == 0) {
        if (parent && parent->ssl) {
            route->ssl = mprCloneSsl(parent->ssl);
        } else {
            route->ssl = mprCreateSsl(1);
        }
    } else {
        if (parent && route->ssl == parent->ssl) {
            route->ssl = mprCloneSsl(parent->ssl);
        }
    }
    parseAll(route, key, prop);
}


static void parseSslAuthorityFile(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslCaFile(route->ssl, prop->value);
    }
}


static void parseSslAuthorityDirectory(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslCaPath(route->ssl, prop->value);
    }
}


static void parseSslCertificate(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslCertFile(route->ssl, prop->value);
    }
}


static void parseSslCiphers(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprAddSslCiphers(route->ssl, getList(prop));
}


static void parseSslKey(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (!mprPathExists(prop->value, R_OK)) {
        httpParseError(route, "Cannot find file %s", prop->value);
    } else {
        mprSetSslKeyFile(route->ssl, prop->value);
    }
}


static void parseSslProtocols(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    cchar       *value;
    int         bit, clear, ji, mask;

    mask = 0;
    for (ITERATE_CONFIG(route, prop, child, ji)) {
        value = child->value;
        clear = 0;
        if (sstarts(value, "+")) {
            value++;
        } else if (sstarts(value, "-")) {
            clear = 1;
            value++;
        }
        bit = 0;
        if (scaselessmatch(value, "all")) {
            /* Do not include insecure SSLv2 and SSLv3 */
            bit = MPR_PROTO_TLSV1 | MPR_PROTO_TLSV1_2;
        } else if (scaselessmatch(value, "sslv2")) {
            /* SSLv2 is insecure */
            bit = MPR_PROTO_SSLV2;
        } else if (scaselessmatch(value, "sslv3")) {
            /* SSLv3 is insecure */
            bit = MPR_PROTO_SSLV3;
        } else if (scaselessmatch(value, "tlsv1") || scaselessmatch(value, "tls")) {
            bit = MPR_PROTO_TLSV1;
        } else if (scaselessmatch(value, "tlsv1.1")) {
            bit = MPR_PROTO_TLSV1_1;
        } else if (scaselessmatch(value, "tlsv1.2")) {
            bit = MPR_PROTO_TLSV1_2;
        }
        if (clear) {
            mask &= ~bit;
        } else {
            mask |= bit;
        }
    }
    mprSetSslProtocols(route->ssl, mask);
}


static void parseSslProvider(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetSslProvider(route->ssl, prop->value);
}


static void parseSslVerifyClient(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprVerifySslPeer(route->ssl, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseSslVerifyIssuer(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprVerifySslIssuer(route->ssl, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseStealth(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteStealth(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


/*
    Names: "close", "redirect", "run", "write"
    Rules:
        close:      [immediate]
        redirect:   status URI
        run:        ${DOCUMENT_ROOT}/${request:uri}
        run:        ${controller}-${name}
        write:      [-r] status "Hello World\r\n"
*/
static void parseTarget(HttpRoute *route, cchar *key, MprJson *prop)
{
    cchar   *name, *args;

    if (prop->type & MPR_JSON_OBJ) {
        name = mprGetJson(prop, "operation");
        args = mprGetJson(prop, "args");
    } else {
        name = "run";
        args = prop->value;
    }
    httpSetRouteTarget(route, name, args);
}


static void parseTimeouts(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpGraduateLimits(route, 0);
    parseAll(route, key, prop);
}


static void parseTimeoutsExit(HttpRoute *route, cchar *key, MprJson *prop)
{
    mprSetExitTimeout(httpGetTicks(prop->value));
}


static void parseTimeoutsParse(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->requestParseTimeout = httpGetTicks(prop->value);
    }
}


static void parseTimeoutsInactivity(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->inactivityTimeout = httpGetTicks(prop->value);
    }
}


static void parseTimeoutsRequest(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->requestTimeout = httpGetTicks(prop->value);
    }
}


static void parseTimeoutsSession(HttpRoute *route, cchar *key, MprJson *prop)
{
    if (! mprGetDebugMode()) {
        route->limits->sessionTimeout = httpGetTicks(prop->value);
    }
}


static void parseTrace(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *levels, *child;
    cchar       *location;
    ssize       size, maxContent;
    cchar       *format, *formatter;
    char        level;
    int         anew, backup, ji;

    if (route->trace && route->trace->flags & MPR_LOG_CMDLINE) {
        mprLog("info http config", 4, "Already tracing. Ignoring trace configuration");
        return;
    }
    size = (ssize) httpGetNumber(mprGetJson(prop, "size"));
    format = mprGetJson(prop, "format");
    formatter = mprGetJson(prop, "formatter");
    location = mprGetJson(prop, "location");
    level = (char) stoi(mprGetJson(prop, "level"));
    backup = (int) stoi(mprGetJson(prop, "backup"));
    anew = smatch(mprGetJson(prop, "anew"), "true");
    maxContent = (ssize) httpGetNumber(mprGetJson(prop, "content"));

    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    if (size < (10 * 1000)) {
        httpParseError(route, "Trace log size is too small. Must be larger than 10K");
        return;
    }
    if (location == 0) {
        httpParseError(route, "Missing trace filename");
        return;
    }
    if (!smatch(location, "stdout") && !smatch(location, "stderr")) {
        location = httpMakePath(route, 0, location);
    }
    if ((levels = mprGetJsonObj(prop, "levels")) != 0) {
        for (ITERATE_CONFIG(route, prop, child, ji)) {
            httpSetTraceEventLevel(route->trace, child->name, (int) stoi(child->value));
        }
    }
    route->trace = httpCreateTrace(route->trace);
    httpSetTraceFormatterName(route->trace, formatter);
    httpSetTraceLogFile(route->trace, location, size, backup, format, anew ? MPR_LOG_ANEW : 0);
    httpSetTraceFormat(route->trace, format);
    httpSetTraceContentSize(route->trace, maxContent);
    httpSetTraceLevel(level);
}


static void parseUpdate(HttpRoute *route, cchar *key, MprJson *prop)
{
    route->update = (prop->type & MPR_JSON_TRUE) ? 1 : 0;
}


static void parseXsrf(HttpRoute *route, cchar *key, MprJson *prop)
{
    httpSetRouteXsrf(route, (prop->type & MPR_JSON_TRUE) ? 1 : 0);
}


static void parseInclude(HttpRoute *route, cchar *key, MprJson *prop)
{
    MprJson     *child;
    int         ji;

    for (ITERATE_CONFIG(route, prop, child, ji)) {
        parseFile(route, child->value);
    }
}


PUBLIC int httpInitParser()
{
    HTTP->parsers = mprCreateHash(0, MPR_HASH_STATIC_VALUES);

    httpAddConfig("app", parseAll);
    httpAddConfig("app.http", parseHttp);
    httpAddConfig("app.http.auth", parseAuth);
    httpAddConfig("app.http.auth.auto", parseAll);
    httpAddConfig("app.http.auth.auto.name", parseAuthAutoName);
    httpAddConfig("app.http.auth.auto.roles", parseAuthAutoRoles);
    httpAddConfig("app.http.auth.login", parseAuthLogin);
    httpAddConfig("app.http.auth.realm", parseAuthRealm);
    httpAddConfig("app.http.auth.require", parseAll);
    httpAddConfig("app.http.auth.require.roles", parseAuthRequireRoles);
    httpAddConfig("app.http.auth.require.users", parseAuthRequireUsers);
    httpAddConfig("app.http.auth.roles", parseAuthRoles);
    httpAddConfig("app.http.auth.session.cookie", parseAuthSessionCookie);
    httpAddConfig("app.http.auth.session.vibility", parseAuthSessionVisibility);
    httpAddConfig("app.http.auth.store", parseAuthStore);
    httpAddConfig("app.http.auth.type", parseAuthType);
    httpAddConfig("app.http.auth.users", parseAuthUsers);
    httpAddConfig("app.http.cache", parseCache);
    httpAddConfig("app.http.content", parseAll);
    httpAddConfig("app.http.content.combine", parseContentCombine);
    httpAddConfig("app.http.content.minify", parseContentMinify);
    httpAddConfig("app.http.content.compress", parseContentCompress);
#if DEPRECATED || 1
    httpAddConfig("app.http.content.keep", parseContentKeep);
#endif
    httpAddConfig("app.http.database", parseDatabase);
    httpAddConfig("app.http.deleteUploads", parseDeleteUploads);
    httpAddConfig("app.http.directories", parseDirectories);
    httpAddConfig("app.http.documents", parseDocuments);
    httpAddConfig("app.http.domain", parseDomain);
    httpAddConfig("app.http.errors", parseErrors);
    httpAddConfig("app.http.formats", parseAll);
    httpAddConfig("app.http.formats.response", parseFormatsResponse);
    httpAddConfig("app.http.handler", parseHandler);
    httpAddConfig("app.http.headers", parseAll);
    httpAddConfig("app.http.headers.add", parseHeadersAdd);
    httpAddConfig("app.http.headers.remove", parseHeadersRemove);
    httpAddConfig("app.http.headers.set", parseHeadersSet);
    httpAddConfig("app.http.home", parseHome);
    httpAddConfig("app.http.indexes", parseIndexes);
    httpAddConfig("app.http.keep", parseKeep);
    httpAddConfig("app.http.languages", parseLanguages);
    httpAddConfig("app.http.limits", parseLimits);
    httpAddConfig("app.http.limits.buffer", parseLimitsBuffer);
    httpAddConfig("app.http.limits.cache", parseLimitsCache);
    httpAddConfig("app.http.limits.cacheItem", parseLimitsCacheItem);
    httpAddConfig("app.http.limits.chunk", parseLimitsChunk);
    httpAddConfig("app.http.limits.clients", parseLimitsClients);
    httpAddConfig("app.http.limits.connections", parseLimitsConnections);
    httpAddConfig("app.http.limits.keepAlive", parseLimitsKeepAlive);
    httpAddConfig("app.http.limits.files", parseLimitsFiles);
    httpAddConfig("app.http.limits.memory", parseLimitsMemory);
    httpAddConfig("app.http.limits.requestBody", parseLimitsRequestBody);
    httpAddConfig("app.http.limits.requestForm", parseLimitsRequestForm);
    httpAddConfig("app.http.limits.requestHeader", parseLimitsRequestHeader);
    httpAddConfig("app.http.limits.responseBody", parseLimitsResponseBody);
    httpAddConfig("app.http.limits.processes", parseLimitsProcesses);
    httpAddConfig("app.http.limits.requests", parseLimitsRequests);
    httpAddConfig("app.http.limits.sessions", parseLimitsSessions);
    httpAddConfig("app.http.limits.upload", parseLimitsUpload);
    httpAddConfig("app.http.limits.uri", parseLimitsUri);
    httpAddConfig("app.http.limits.webSockets", parseLimitsWebSockets);
    httpAddConfig("app.http.limits.webSocketsMessage", parseLimitsWebSocketsMessage);
    httpAddConfig("app.http.limits.webSocketsPacket", parseLimitsWebSocketsPacket);
    httpAddConfig("app.http.limits.webSocketsFrame", parseLimitsWebSocketsFrame);
    httpAddConfig("app.http.limits.workers", parseLimitsWorkers);
    httpAddConfig("app.http.methods", parseMethods);
    httpAddConfig("app.http.mode", parseMode);
    httpAddConfig("app.http.name", parseName);
    httpAddConfig("app.http.params", parseParams);
    httpAddConfig("app.http.pattern", parsePattern);
    httpAddConfig("app.http.pipeline", parseAll);
    httpAddConfig("app.http.pipeline.filters", parsePipelineFilters);
    httpAddConfig("app.http.pipeline.handlers", parsePipelineHandlers);
    httpAddConfig("app.http.prefix", parsePrefix);
    httpAddConfig("app.http.redirect", parseRedirect);
    httpAddConfig("app.http.resources", parseResources);
    httpAddConfig("app.http.scheme", parseScheme);

    httpAddConfig("app.http.server", parseServer);
    httpAddConfig("app.http.server.account", parseServerAccount);
    httpAddConfig("app.http.server.chroot", parseServerChroot);
    httpAddConfig("app.http.server.defenses", parseServerDefenses);
    httpAddConfig("app.http.server.listen", parseServerListen);
    httpAddConfig("app.http.server.log", parseServerLog);
    httpAddConfig("app.http.server.monitors", parseServerMonitors);
    httpAddConfig("app.http.server.ssl", parseSsl);
    httpAddConfig("app.http.server.ssl.authority", parseAll);
    httpAddConfig("app.http.server.ssl.authority.file", parseSslAuthorityFile);
    httpAddConfig("app.http.server.ssl.authority.directory", parseSslAuthorityDirectory);
    httpAddConfig("app.http.server.ssl.certificate", parseSslCertificate);
    httpAddConfig("app.http.server.ssl.ciphers", parseSslCiphers);
    httpAddConfig("app.http.server.ssl.key", parseSslKey);
    httpAddConfig("app.http.server.ssl.provider", parseSslProvider);
    httpAddConfig("app.http.server.ssl.protocols", parseSslProtocols);
    httpAddConfig("app.http.server.ssl.verify", parseAll);
    httpAddConfig("app.http.server.ssl.verify.client", parseSslVerifyClient);
    httpAddConfig("app.http.server.ssl.verify.issuer", parseSslVerifyIssuer);

    httpAddConfig("app.http.showErrors", parseShowErrors);
    httpAddConfig("app.http.source", parseSource);
    httpAddConfig("app.http.serverPrefix", parseServerPrefix);
    httpAddConfig("app.http.stealth", parseStealth);
    httpAddConfig("app.http.target", parseTarget);
    httpAddConfig("app.http.timeouts", parseTimeouts);
    httpAddConfig("app.http.timeouts.exit", parseTimeoutsExit);
    httpAddConfig("app.http.timeouts.parse", parseTimeoutsParse);
    httpAddConfig("app.http.timeouts.inactivity", parseTimeoutsInactivity);
    httpAddConfig("app.http.timeouts.request", parseTimeoutsRequest);
    httpAddConfig("app.http.timeouts.session", parseTimeoutsSession);
    httpAddConfig("app.http.trace", parseTrace);
    httpAddConfig("app.http.update", parseUpdate);
    httpAddConfig("app.http.xsrf", parseXsrf);
    httpAddConfig("directories", parseDirectories);
    httpAddConfig("include", parseInclude);

    return 0;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/conn.c ************/


/*
    conn.c -- Connection module to handle individual HTTP connections.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/***************************** Forward Declarations ***************************/

static HttpPacket *getPacket(HttpConn *conn, ssize *bytesToRead);
static void manageConn(HttpConn *conn, int flags);
static bool prepForNext(HttpConn *conn);

/*********************************** Code *************************************/
/*
    Create a new connection object
 */
PUBLIC HttpConn *httpCreateConn(HttpEndpoint *endpoint, MprDispatcher *dispatcher)
{
    HttpConn    *conn;
    HttpHost    *host;
    HttpRoute   *route;

    if ((conn = mprAllocObj(HttpConn, manageConn)) == 0) {
        return 0;
    }
    conn->protocol = HTTP->protocol;
    conn->http = HTTP;
    conn->port = -1;
    conn->retries = HTTP_RETRIES;
    conn->endpoint = endpoint;
    conn->lastActivity = HTTP->now;
    conn->ioCallback = httpIOEvent;

    if (endpoint) {
        conn->notifier = endpoint->notifier;
        host = mprGetFirstItem(endpoint->hosts);
        if (host && (route = host->defaultRoute) != 0) {
            conn->limits = route->limits;
            conn->trace = route->trace;
        } else {
            conn->limits = HTTP->serverLimits;
            conn->trace = HTTP->trace;
        }
    } else {
        conn->limits = HTTP->clientLimits;
        conn->trace = HTTP->trace;
    }
    conn->keepAliveCount = conn->limits->keepAliveMax;
    conn->serviceq = httpCreateQueueHead(conn, "serviceq");

    if (dispatcher) {
        conn->dispatcher = dispatcher;
    } else if (endpoint) {
        conn->dispatcher = endpoint->dispatcher;
    } else {
        conn->dispatcher = mprGetDispatcher();
    }
    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);
    httpSetState(conn, HTTP_STATE_BEGIN);
    httpAddConn(conn);
    return conn;
}


/*
    Destroy a connection. This removes the connection from the list of connections.
 */
PUBLIC void httpDestroyConn(HttpConn *conn)
{
    if (!conn->destroyed && !conn->borrowed) {
        HTTP_NOTIFY(conn, HTTP_EVENT_DESTROY, 0);
        if (httpServerConn(conn)) {
            httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_CONNECTIONS, -1);
            if (conn->activeRequest) {
                httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
                conn->activeRequest = 0;
            }
        }
        httpRemoveConn(conn);
        conn->input = 0;
        if (conn->tx) {
            httpClosePipeline(conn);
        }
        if (conn->sock) {
            mprCloseSocket(conn->sock, 0);
        }
        if (conn->dispatcher && conn->dispatcher->flags & MPR_DISPATCHER_AUTO) {
            mprDestroyDispatcher(conn->dispatcher);
        }
        conn->destroyed = 1;
    }
}


static void manageConn(HttpConn *conn, int flags)
{
    assert(conn);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(conn->rx);
        mprMark(conn->tx);
        mprMark(conn->readq);
        mprMark(conn->writeq);
        mprMark(conn->sock);
        mprMark(conn->limits);
        mprMark(conn->http);
        mprMark(conn->dispatcher);
        mprMark(conn->newDispatcher);
        mprMark(conn->oldDispatcher);
        mprMark(conn->address);
        mprMark(conn->serviceq);
        mprMark(conn->currentq);
        mprMark(conn->endpoint);
        mprMark(conn->host);
        mprMark(conn->input);
        mprMark(conn->connectorq);
        mprMark(conn->timeoutEvent);
        mprMark(conn->workerEvent);
        mprMark(conn->context);
        mprMark(conn->ejs);
        mprMark(conn->pool);
        mprMark(conn->mark);
        mprMark(conn->reqData);
        mprMark(conn->data);
        mprMark(conn->grid);
        mprMark(conn->record);
        mprMark(conn->boundary);
        mprMark(conn->errorMsg);
        mprMark(conn->ip);
        mprMark(conn->protocol);
        mprMark(conn->protocols);
        mprMark(conn->trace);
        mprMark(conn->authType);
        mprMark(conn->authData);
        mprMark(conn->username);
        mprMark(conn->password);
        mprMark(conn->user);
        mprMark(conn->headersCallbackArg);
    }
}


PUBLIC void httpDisconnect(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (conn->sock) {
        mprDisconnectSocket(conn->sock);
    }
    conn->connError++;
    conn->error++;
    conn->keepAliveCount = 0;
    if (tx) {
        tx->finalized = 1;
        tx->finalizedOutput = 1;
        tx->finalizedConnector = 1;
        tx->responded = 1;
    }
    if (conn->rx) {
        httpSetEof(conn);
    }
}


static void connTimeout(HttpConn *conn, MprEvent *mprEvent)
{
    HttpLimits  *limits;
    cchar       *event, *msg, *prefix;

    if (conn->destroyed) {
        return;
    }
    assert(conn->tx);
    assert(conn->rx);

    msg = 0;
    event = 0;
    limits = conn->limits;
    assert(limits);

    if (conn->timeoutCallback) {
        (conn->timeoutCallback)(conn);
    }
    if (!conn->connError) {
        prefix = (conn->state == HTTP_STATE_BEGIN) ? "Idle connection" : "Request";
        if (conn->timeout == HTTP_PARSE_TIMEOUT) {
            msg = sfmt("%s exceeded parse headers timeout of %lld sec", prefix, limits->requestParseTimeout  / 1000);
            event = "timeout.parse";

        } else if (conn->timeout == HTTP_INACTIVITY_TIMEOUT) {
            msg = sfmt("%s exceeded inactivity timeout of %lld sec", prefix, limits->inactivityTimeout / 1000);
            event = "timeout.inactivity";

        } else if (conn->timeout == HTTP_REQUEST_TIMEOUT) {
            msg = sfmt("%s exceeded timeout %lld sec", prefix, limits->requestTimeout / 1000);
            event = "timeout.duration";
        }
        if (conn->state < HTTP_STATE_FIRST) {
            httpDisconnect(conn);
            if (msg) {
                httpTrace(conn, event, "error", "msg:'%s'", msg);
            }
        } else {
            httpError(conn, HTTP_CODE_REQUEST_TIMEOUT, "%s", msg);
        }
    }
    if (httpClientConn(conn)) {
        httpDestroyConn(conn);
    } else {
        httpEnableConnEvents(conn);
    }
}


PUBLIC void httpScheduleConnTimeout(HttpConn *conn)
{
    if (!conn->timeoutEvent && !conn->destroyed) {
        /*
            Will run on the HttpConn dispatcher unless shutting down and it is destroyed already
         */
        conn->timeoutEvent = mprCreateEvent(conn->dispatcher, "connTimeout", 0, connTimeout, conn, 0);
    }
}


static void commonPrep(HttpConn *conn)
{
    if (conn->timeoutEvent) {
        mprRemoveEvent(conn->timeoutEvent);
        conn->timeoutEvent = 0;
    }
    conn->lastActivity = conn->http->now;
    conn->error = 0;
    conn->errorMsg = 0;
    conn->state = 0;
    conn->authRequested = 0;
    httpSetState(conn, HTTP_STATE_BEGIN);
    httpInitSchedulerQueue(conn->serviceq);
}


/*
    Prepare for another request
    Return true if there is another request ready for serving
 */
static bool prepForNext(HttpConn *conn)
{
    assert(conn->endpoint);
    assert(conn->state == HTTP_STATE_COMPLETE);

    if (conn->borrowed) {
        return 0;
    }
    if (conn->keepAliveCount <= 0) {
        conn->state = HTTP_STATE_BEGIN;
        return 0;
    }
    if (conn->tx) {
        assert(conn->tx->finalized && conn->tx->finalizedConnector && conn->tx->finalizedOutput);
        conn->tx->conn = 0;
    }
    if (conn->rx) {
        conn->rx->conn = 0;
    }
    conn->authType = 0;
    conn->username = 0;
    conn->password = 0;
    conn->user = 0;
    conn->authData = 0;
    conn->encoded = 0;
    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);
    commonPrep(conn);
    assert(conn->state == HTTP_STATE_BEGIN);
    return conn->input && (httpGetPacketLength(conn->input) > 0) && !conn->connError;
}


#if KEEP
/*
    Eat remaining input incase last request did not consume all data
 */
static void consumeLastRequest(HttpConn *conn)
{
    char    junk[4096];

    if (conn->state >= HTTP_STATE_FIRST) {
        while (!httpIsEof(conn) && !httpRequestExpired(conn, 0)) {
            if (httpRead(conn, junk, sizeof(junk)) <= 0) {
                break;
            }
        }
    }
    if (HTTP_STATE_CONNECTED <= conn->state && conn->state < HTTP_STATE_COMPLETE) {
        conn->keepAliveCount = 0;
    }
}
#endif


PUBLIC void httpPrepClientConn(HttpConn *conn, bool keepHeaders)
{
    MprHash     *headers;

    assert(conn);
    if (conn->keepAliveCount > 0 && conn->sock) {
        if (!httpIsEof(conn)) {
            conn->sock = 0;
        }
    } else {
        conn->input = 0;
    }
    conn->connError = 0;
    if (conn->tx) {
        conn->tx->conn = 0;
    }
    if (conn->rx) {
        conn->rx->conn = 0;
    }
    headers = (keepHeaders && conn->tx) ? conn->tx->headers: NULL;
    conn->tx = httpCreateTx(conn, headers);
    conn->rx = httpCreateRx(conn);
    commonPrep(conn);
}


/*
    Accept a new client connection on a new socket.
    This will come in on a worker thread with a new dispatcher dedicated to this connection.
 */
PUBLIC HttpConn *httpAcceptConn(HttpEndpoint *endpoint, MprEvent *event)
{
    Http        *http;
    HttpConn    *conn;
    HttpAddress *address;
    MprSocket   *sock;
    int64       value;

    assert(event);
    assert(event->dispatcher);
    assert(endpoint);

    sock = event->sock;
    http = endpoint->http;

    if (mprShouldDenyNewRequests()) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    if ((conn = httpCreateConn(endpoint, event->dispatcher)) == 0) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    conn->notifier = endpoint->notifier;
    conn->async = endpoint->async;
    conn->endpoint = endpoint;
    conn->sock = sock;
    conn->port = sock->port;
    conn->ip = sclone(sock->ip);

    if ((value = httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_CONNECTIONS, 1)) > conn->limits->connectionsMax) {
        httpTrace(conn, "connection.accept.error", "error", "msg:'Too many concurrent connections',active:%d,max:%d", 
            (int) value, conn->limits->connectionsMax);
        httpDestroyConn(conn);
        return 0;
    }
    if (mprGetHashLength(http->addresses) > conn->limits->clientMax) {
        httpTrace(conn, "connection.accept.error", "error", "msg:'Too many concurrent clients',active:%d,max:%d", 
            mprGetHashLength(http->addresses), conn->limits->clientMax);
        httpDestroyConn(conn);
        return 0;
    }
    address = conn->address;
    if (address && address->banUntil) {
        if (address->banUntil < http->now) {
            httpTrace(conn, "monitor.ban.stop", "context", "client:'%s'", conn->ip);
            address->banUntil = 0;
        } else {
            if (address->banStatus) {
                httpError(conn, HTTP_CLOSE | address->banStatus,
                    "Connection refused, client banned: %s", address->banMsg ? address->banMsg : "");
            } else {
                httpDestroyConn(conn);
                return 0;
            }
        }
    }
    if (endpoint->ssl) {
        if (mprUpgradeSocket(sock, endpoint->ssl, 0) < 0) {
            httpDisconnect(conn);
            httpTrace(conn, "connection.upgrade.error", "error", "msg:'Cannot upgrade socket. %s'", sock->errorMsg);
            httpMonitorEvent(conn, HTTP_COUNTER_SSL_ERRORS, 1);
            httpDestroyConn(conn);
            return 0;
        }
    }
    assert(conn->state == HTTP_STATE_BEGIN);
    httpSetState(conn, HTTP_STATE_CONNECTED);

    httpTrace(conn, "connection.accept.new", "context", "peer:'%s',endpoint:'%s:%d'", 
        conn->ip, sock->acceptIp, sock->acceptPort);
    
    event->mask = MPR_READABLE;
    event->timestamp = conn->http->now;
    (conn->ioCallback)(conn, event);
    return conn;
}


/*
    Read data from the peer. This will use the existing conn->input packet or allocate a new packet if required to 
    hold the data. The number of bytes read is stored in conn->lastRead. SSL connections are traced.
    Socket error messages are stored in conn->errorMsg.
 */
static void readPeerData(HttpConn *conn)
{
    HttpPacket  *packet;
    ssize       size;

    if ((packet = getPacket(conn, &size)) != 0) {
        conn->lastRead = mprReadSocket(conn->sock, mprGetBufEnd(packet->content), size);
        if (conn->lastRead > 0) {
            mprAdjustBufEnd(packet->content, conn->lastRead);
        } else if (conn->lastRead < 0 && mprIsSocketEof(conn->sock)) {
            conn->errorMsg = conn->sock->errorMsg;
            conn->keepAliveCount = 0;
            conn->lastRead = 0;
            if (conn->errorMsg) {
                httpTrace(conn, "connection.close", "context", "msg:'%s'", conn->errorMsg);
            } else {
                httpTrace(conn, "connection.close", "context", NULL);
            }
        }
    }
}


/*
    Handle IO on the connection. Initially the conn->dispatcher will be set to the server->dispatcher and the first 
    I/O event will be handled on the server thread (or main thread). A request handler may create a new 
    conn->dispatcher and transfer execution to a worker thread if required.
 */
PUBLIC void httpIO(HttpConn *conn, int eventMask)
{
    MprSocket   *sp;

    sp = conn->sock;
    if (conn->destroyed) {
        /* Connection has been destroyed */
        return;
    }
    assert(conn->tx);
    assert(conn->rx);

    if ((eventMask & MPR_WRITABLE) && conn->connectorq) {
        httpResumeQueue(conn->connectorq);
    }
    if (eventMask & MPR_READABLE) {
        readPeerData(conn);
    }
    if (sp->secured && !conn->secure) {
        conn->secure = 1;
        if (sp->peerCert) {
            httpTrace(conn, "connection.ssl", "context", "msg:'Connection secured with peer certificate'," \
                "secure:true,cipher:'%s',peerName:'%s',subject:'%s',issuer:'%s'", 
                sp->cipher, sp->peerName, sp->peerCert, sp->peerCertIssuer);
        } else {
            httpTrace(conn, "connection.ssl", "context",
                "msg:'Connection secured without peer certificate',secure:true,cipher:'%s'", sp->cipher);
        }
    }
    /*
        Process one or more complete requests in the packet
     */
    do {
        /* This is and must be the only place httpProtocol is ever called */
        httpProtocol(conn);
    } while (conn->endpoint && conn->state == HTTP_STATE_COMPLETE && prepForNext(conn));

    /*
        When a request completes, prepForNext will reset the state to HTTP_STATE_BEGIN
     */
    if (conn->endpoint && conn->keepAliveCount <= 0 && conn->state < HTTP_STATE_PARSED) {
        httpDestroyConn(conn);
    } else if (conn->async && !mprIsSocketEof(conn->sock) && !conn->delay) {
        httpEnableConnEvents(conn);
    }
}


/*
    Handle an IO event on the connection. This is invoked by the wait subsystem in response to I/O events. 
    It is also invoked via relay when an accept event is received by the server. 
*/
PUBLIC void httpIOEvent(HttpConn *conn, MprEvent *event)
{
    httpIO(conn, event->mask);
}


PUBLIC int httpGetConnEventMask(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;
    MprSocket   *sp;
    int         eventMask;

    sp = conn->sock;
    rx = conn->rx;
    tx = conn->tx;

    eventMask = 0;
    if (rx) {
        if (conn->connError || (tx->writeBlocked) ||
           (conn->connectorq && (conn->connectorq->count > 0 || conn->connectorq->ioCount > 0)) ||
           (httpQueuesNeedService(conn)) ||
           (mprSocketHasBufferedWrite(sp)) ||
           (rx->eof && tx->finalized && conn->state < HTTP_STATE_FINALIZED)) {
            if (!mprSocketHandshaking(sp)) {
                /* Must not pollute the data stream if the SSL stack is still doing manual handshaking */
                eventMask |= MPR_WRITABLE;
            }
        }
        q = conn->readq;
        if (!rx->eof && (q->count < q->max || rx->form || mprSocketHasBufferedRead(sp))) {
            eventMask |= MPR_READABLE;
        }
    } else {
        eventMask |= MPR_READABLE;
    }
    return eventMask;
}


PUBLIC void httpEnableConnEvents(HttpConn *conn)
{
    if (mprShouldAbortRequests() || conn->borrowed) {
        return;
    }
    /*
        Used by ejs
     */
    if (conn->workerEvent) {
        MprEvent *event = conn->workerEvent;
        conn->workerEvent = 0;
        mprQueueEvent(conn->dispatcher, event);
        return;
    }
    httpSetupWaitHandler(conn, httpGetConnEventMask(conn));
}


/*
    Used by ejs
 */
PUBLIC void httpUseWorker(HttpConn *conn, MprDispatcher *dispatcher, MprEvent *event)
{
    lock(conn->http);
    conn->oldDispatcher = conn->dispatcher;
    conn->dispatcher = dispatcher;
    conn->worker = 1;
    assert(!conn->workerEvent);
    conn->workerEvent = event;
    unlock(conn->http);
}


PUBLIC void httpUsePrimary(HttpConn *conn)
{
    lock(conn->http);
    assert(conn->worker);
    assert(conn->state == HTTP_STATE_BEGIN);
    assert(conn->oldDispatcher && conn->dispatcher != conn->oldDispatcher);
    conn->dispatcher = conn->oldDispatcher;
    conn->oldDispatcher = 0;
    conn->worker = 0;
    unlock(conn->http);
}


PUBLIC void httpBorrowConn(HttpConn *conn)
{
    assert(!conn->borrowed);
    if (!conn->borrowed) {
        mprAddRoot(conn);
        conn->borrowed = 1;
    }
}


PUBLIC void httpReturnConn(HttpConn *conn)
{
    assert(conn->borrowed);
    if (conn->borrowed) {
        conn->borrowed = 0;
        mprRemoveRoot(conn);
        httpEnableConnEvents(conn);
    }
}


/*
    Steal the socket object from a connection. This disconnects the socket from management by the Http service.
    It is the callers responsibility to call mprCloseSocket when required.
    Harder than it looks. We clone the socket, steal the socket handle and set the connection socket handle to invalid.
    This preserves the HttpConn.sock object for the connection and returns a new MprSocket for the caller.
 */
PUBLIC MprSocket *httpStealSocket(HttpConn *conn)
{
    MprSocket   *sock;

    assert(conn->sock);
    assert(!conn->destroyed);

    if (!conn->destroyed && !conn->borrowed) {
        lock(conn->http);
        sock = mprCloneSocket(conn->sock);
        (void) mprStealSocketHandle(conn->sock);
        mprRemoveSocketHandler(conn->sock);
        httpRemoveConn(conn);
        httpDiscardData(conn, HTTP_QUEUE_TX);
        httpDiscardData(conn, HTTP_QUEUE_RX);
        httpSetState(conn, HTTP_STATE_COMPLETE);
        /* This will cause httpIOEvent to regard this as a client connection and not destroy this connection */
        conn->endpoint = 0;
        conn->async = 0;
        unlock(conn->http);
        return sock;
    }
    return 0;
}


/*
    Steal the O/S socket handle a connection's socket. This disconnects the socket handle from management by the connection.
    It is the callers responsibility to call close() on the Socket when required.
    Note: this does not change the state of the connection.
 */
PUBLIC Socket httpStealSocketHandle(HttpConn *conn)
{
    return mprStealSocketHandle(conn->sock);
}


PUBLIC void httpSetupWaitHandler(HttpConn *conn, int eventMask)
{
    MprSocket   *sp;

    sp = conn->sock;
    if (eventMask) {
        if (sp->handler == 0) {
            mprAddSocketHandler(sp, eventMask, conn->dispatcher, conn->ioCallback, conn, 0);
        } else {
            mprSetSocketDispatcher(sp, conn->dispatcher);
            mprEnableSocketEvents(sp, eventMask);
        }
    } else if (sp->handler) {
        mprWaitOn(sp->handler, eventMask);
    }
}


PUBLIC void httpFollowRedirects(HttpConn *conn, bool follow)
{
    conn->followRedirects = follow;
}


/*
    Get the packet into which to read data. Return in *size the length of data to attempt to read.
 */
static HttpPacket *getPacket(HttpConn *conn, ssize *size)
{
    HttpPacket  *packet;
    MprBuf      *content;
    ssize       psize;

    if ((packet = conn->input) == NULL) {
        /*
            Boost the size of the packet if we have already read a largish amount of data
         */
        psize = (conn->rx && conn->rx->bytesRead > ME_MAX_BUFFER) ? ME_MAX_BUFFER * 8 : ME_MAX_BUFFER;
        conn->input = packet = httpCreateDataPacket(psize);
    } else {
        content = packet->content;
        mprResetBufIfEmpty(content);
        if (mprGetBufSpace(content) < ME_MAX_BUFFER && mprGrowBuf(content, ME_MAX_BUFFER) < 0) {
            conn->keepAliveCount = 0;
            conn->state = HTTP_STATE_BEGIN;
            return 0;
        }
    }
    *size = mprGetBufSpace(packet->content);
    assert(*size > 0);
    return packet;
}


PUBLIC int httpGetAsync(HttpConn *conn)
{
    return conn->async;
}


PUBLIC ssize httpGetChunkSize(HttpConn *conn)
{
    if (conn->tx) {
        return conn->tx->chunkSize;
    }
    return 0;
}


PUBLIC void *httpGetConnContext(HttpConn *conn)
{
    return conn->context;
}


PUBLIC void *httpGetConnHost(HttpConn *conn)
{
    return conn->host;
}


PUBLIC ssize httpGetWriteQueueCount(HttpConn *conn)
{
    return conn->writeq ? conn->writeq->count : 0;
}


PUBLIC void httpResetCredentials(HttpConn *conn)
{
    conn->authType = 0;
    conn->username = 0;
    conn->password = 0;
    httpRemoveHeader(conn, "Authorization");
}


PUBLIC void httpSetAsync(HttpConn *conn, int enable)
{
    conn->async = (enable) ? 1 : 0;
}


PUBLIC void httpSetConnNotifier(HttpConn *conn, HttpNotifier notifier)
{
    conn->notifier = notifier;
    if (conn->readq->first) {
        /* Test first rather than count because we want a readable event for the end packet */
        HTTP_NOTIFY(conn, HTTP_EVENT_READABLE, 0);
    }
}


/*
    password and authType can be null
    User may be a combined user:password
 */
PUBLIC void httpSetCredentials(HttpConn *conn, cchar *username, cchar *password, cchar *authType)
{
    char    *ptok;

    httpResetCredentials(conn);
    if (password == NULL && strchr(username, ':') != 0) {
        conn->username = stok(sclone(username), ":", &ptok);
        conn->password = sclone(ptok);
    } else {
        conn->username = sclone(username);
        conn->password = sclone(password);
    }
    if (authType) {
        conn->authType = sclone(authType);
    }
}


PUBLIC void httpSetKeepAliveCount(HttpConn *conn, int count)
{
    conn->keepAliveCount = count;
}


PUBLIC void httpSetChunkSize(HttpConn *conn, ssize size)
{
    if (conn->tx) {
        conn->tx->chunkSize = size;
    }
}


PUBLIC void httpSetHeadersCallback(HttpConn *conn, HttpHeadersCallback fn, void *arg)
{
    conn->headersCallback = fn;
    conn->headersCallbackArg = arg;
}


PUBLIC void httpSetIOCallback(HttpConn *conn, HttpIOCallback fn)
{
    conn->ioCallback = fn;
}


PUBLIC void httpSetConnContext(HttpConn *conn, void *context)
{
    conn->context = context;
}


PUBLIC void httpSetConnHost(HttpConn *conn, void *host)
{
    conn->host = host;
}


/*
    Set the protocol to use for outbound requests
 */
PUBLIC void httpSetProtocol(HttpConn *conn, cchar *protocol)
{
    if (conn->state < HTTP_STATE_CONNECTED) {
        conn->protocol = sclone(protocol);
    }
}


PUBLIC void httpSetRetries(HttpConn *conn, int count)
{
    conn->retries = count;
}


PUBLIC void httpSetState(HttpConn *conn, int targetState)
{
    int     state;

    if (targetState == conn->state) {
        return;
    }
    if (targetState < conn->state) {
        /* Prevent regressions */
        return;
    }
    for (state = conn->state + 1; state <= targetState; state++) {
        conn->state = state;
        HTTP_NOTIFY(conn, HTTP_EVENT_STATE, state);
    }
}


PUBLIC void httpNotify(HttpConn *conn, int event, int arg)
{
    if (conn->notifier) {
        (conn->notifier)(conn, event, arg);
    }
}


/*
    Set each timeout arg to -1 to skip. Set to zero for no timeout. Otherwise set to number of msecs.
 */
PUBLIC void httpSetTimeout(HttpConn *conn, MprTicks requestTimeout, MprTicks inactivityTimeout)
{
    if (requestTimeout >= 0) {
        if (requestTimeout == 0) {
            conn->limits->requestTimeout = MAXINT;
        } else {
            conn->limits->requestTimeout = requestTimeout;
        }
    }
    if (inactivityTimeout >= 0) {
        if (inactivityTimeout == 0) {
            conn->limits->inactivityTimeout = MAXINT;
        } else {
            conn->limits->inactivityTimeout = inactivityTimeout;
        }
    }
}


PUBLIC HttpLimits *httpSetUniqueConnLimits(HttpConn *conn)
{
    HttpLimits      *limits;

    if ((limits = mprAllocStruct(HttpLimits)) != 0) {
        *limits = *conn->limits;
        conn->limits = limits;
    }
    return limits;
}


/*
    Test if a request has expired relative to the default inactivity and request timeout limits.
    Set timeout to a non-zero value to apply an overriding smaller timeout
    Set timeout to a value in msec. If timeout is zero, override default limits and wait forever.
    If timeout is < 0, use default inactivity and duration timeouts. If timeout is > 0, then use this timeout as an additional
    timeout.
 */
PUBLIC bool httpRequestExpired(HttpConn *conn, MprTicks timeout)
{
    HttpLimits  *limits;
    MprTicks    inactivityTimeout, requestTimeout;

    limits = conn->limits;
    if (mprGetDebugMode() || timeout == 0) {
        inactivityTimeout = requestTimeout = MPR_MAX_TIMEOUT;
    } else if (timeout < 0) {
        inactivityTimeout = limits->inactivityTimeout;
        requestTimeout = limits->requestTimeout;
    } else {
        inactivityTimeout = min(limits->inactivityTimeout, timeout);
        requestTimeout = min(limits->requestTimeout, timeout);
    }
    if (mprGetRemainingTicks(conn->started, requestTimeout) < 0) {
        if (requestTimeout != timeout) {
            httpTrace(conn, "timeout.duration", "error",
                "msg:'Request cancelled exceeded max duration',timeout:%lld", requestTimeout / 1000);
        }
        return 1;
    }
    if (mprGetRemainingTicks(conn->lastActivity, inactivityTimeout) < 0) {
        if (inactivityTimeout != timeout) {
            httpTrace(conn, "timeout.inactivity", "error", 
                "msg:'Request cancelled due to inactivity',timeout:%lld", inactivityTimeout / 1000);
        }
        return 1;
    }
    return 0;
}


PUBLIC void httpSetConnData(HttpConn *conn, void *data)
{
    conn->data = data;
}


PUBLIC void httpSetConnReqData(HttpConn *conn, void *data)
{
    conn->reqData = data;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/digest.c ************/


/*
    digest.c - Digest Authorization

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Locals ************************************/
/*
    Per-request digest authorization data
    @see HttpAuth
    @ingroup HttpAuth
    @stability Evolving
 */
typedef struct HttpDigest {
    char    *algorithm;
    char    *cnonce;
    char    *domain;
    char    *nc;
    char    *nonce;
    char    *opaque;
    char    *qop;
    char    *realm;
    char    *stale;
    char    *uri;
} HttpDigest;


/********************************** Forwards **********************************/

static char *calcDigest(HttpConn *conn, HttpDigest *dp, cchar *username);
static char *createDigestNonce(HttpConn *conn, cchar *secret, cchar *realm);
static void manageDigestData(HttpDigest *dp, int flags);
static int parseDigestNonce(char *nonce, cchar **secret, cchar **realm, MprTime *when);

/*********************************** Code *************************************/
/*
    Parse the client 'Authorization' header and the server 'Www-Authenticate' header
 */
PUBLIC int httpDigestParse(HttpConn *conn, cchar **username, cchar **password)
{
    HttpRx      *rx;
    HttpDigest  *dp;
    MprTime     when;
    char        *value, *tok, *key, *cp, *sp;
    cchar       *secret, *realm;
    int         seenComma;

    rx = conn->rx;
    if (password) {
        *password = NULL;
    }
    if (username) {
        *username = NULL;
    }
    if (!rx->authDetails) {
        return 0;
    }
    dp = conn->authData = mprAllocObj(HttpDigest, manageDigestData);
    key = sclone(rx->authDetails);

    while (*key) {
        while (*key && isspace((uchar) *key)) {
            key++;
        }
        tok = key;
        while (*tok && !isspace((uchar) *tok) && *tok != ',' && *tok != '=') {
            tok++;
        }
        if (*tok) {
            *tok++ = '\0';
        }
        while (isspace((uchar) *tok)) {
            tok++;
        }
        seenComma = 0;
        if (*tok == '\"') {
            value = ++tok;
            while (*tok && *tok != '\"') {
                tok++;
            }
        } else {
            value = tok;
            while (*tok && *tok != ',') {
                tok++;
            }
            seenComma++;
        }
        if (*tok) {
            *tok++ = '\0';
        }

        /*
            Handle back-quoting
         */
        if (strchr(value, '\\')) {
            for (cp = sp = value; *sp; sp++) {
                if (*sp == '\\') {
                    sp++;
                }
                *cp++ = *sp++;
            }
            *cp = '\0';
        }

        /*
            user, response, oqaque, uri, realm, nonce, nc, cnonce, qop
         */
        switch (tolower((uchar) *key)) {
        case 'a':
            if (scaselesscmp(key, "algorithm") == 0) {
                dp->algorithm = sclone(value);
                break;
            } else if (scaselesscmp(key, "auth-param") == 0) {
                break;
            }
            break;

        case 'c':
            if (scaselesscmp(key, "cnonce") == 0) {
                dp->cnonce = sclone(value);
            }
            break;

        case 'd':
            if (scaselesscmp(key, "domain") == 0) {
                dp->domain = sclone(value);
                break;
            }
            break;

        case 'n':
            if (scaselesscmp(key, "nc") == 0) {
                dp->nc = sclone(value);
            } else if (scaselesscmp(key, "nonce") == 0) {
                dp->nonce = sclone(value);
            }
            break;

        case 'o':
            if (scaselesscmp(key, "opaque") == 0) {
                dp->opaque = sclone(value);
            }
            break;

        case 'q':
            if (scaselesscmp(key, "qop") == 0) {
                dp->qop = sclone(value);
            }
            break;

        case 'r':
            if (scaselesscmp(key, "realm") == 0) {
                dp->realm = sclone(value);
            } else if (scaselesscmp(key, "response") == 0) {
                /* Store the response digest in the password field. This is MD5(user:realm:password) */
                if (password) {
                    *password = sclone(value);
                }
                conn->encoded = 1;
            }
            break;

        case 's':
            if (scaselesscmp(key, "stale") == 0) {
                break;
            }

        case 'u':
            if (scaselesscmp(key, "uri") == 0) {
                dp->uri = sclone(value);
            } else if (scaselesscmp(key, "username") == 0 || scaselesscmp(key, "user") == 0) {
                if (username) {
                    *username = sclone(value);
                }
            }
            break;

        default:
            /*  Just ignore keywords we don't understand */
            ;
        }
        key = tok;
        if (!seenComma) {
            while (*key && *key != ',') {
                key++;
            }
            if (*key) {
                key++;
            }
        }
    }
    if (username && *username == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (password && *password == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (dp->realm == 0 || dp->nonce == 0 || dp->uri == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (dp->qop && (dp->cnonce == 0 || dp->nc == 0)) {
        return MPR_ERR_BAD_FORMAT;
    }
    if (httpServerConn(conn)) {
        realm = secret = 0;
        when = 0;
        parseDigestNonce(dp->nonce, &secret, &realm, &when);
        if (!smatch(secret, secret)) {
            httpTrace(conn, "auth.digest.error", "error", "msg:'Access denied, Nonce mismatch'");
            return MPR_ERR_BAD_STATE;

        } else if (!smatch(realm, rx->route->auth->realm)) {
            httpTrace(conn, "auth.digest.error", "error", "msg:'Access denied, Realm mismatch'");
            return MPR_ERR_BAD_STATE;

        } else if (dp->qop && !smatch(dp->qop, "auth")) {
            httpTrace(conn, "auth.digest.error", "error", "msg:'Access denied, Bad qop'");
            return MPR_ERR_BAD_STATE;

        } else if ((when + (5 * 60)) < time(0)) {
            httpTrace(conn, "auth.digest.error", "error", "msg:'Access denied, Nonce is stale'");
            return MPR_ERR_BAD_STATE;
        }
        rx->passwordDigest = calcDigest(conn, dp, *username);
    } else {
        if (dp->domain == 0 || dp->opaque == 0 || dp->algorithm == 0 || dp->stale == 0) {
            return MPR_ERR_BAD_FORMAT;
        }
    }
    return 0;
}


static void manageDigestData(HttpDigest *dp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(dp->algorithm);
        mprMark(dp->cnonce);
        mprMark(dp->domain);
        mprMark(dp->nc);
        mprMark(dp->nonce);
        mprMark(dp->opaque);
        mprMark(dp->qop);
        mprMark(dp->realm);
        mprMark(dp->stale);
        mprMark(dp->uri);
    }
}


/*
    Respond to the request by asking for a client login
    Only called if not logged in.
 */
PUBLIC void httpDigestLogin(HttpConn *conn)
{
    HttpAuth    *auth;
    char        *nonce, *opaque;

    auth = conn->rx->route->auth;

    if (auth->loginPage && !sends(conn->rx->referrer, auth->loginPage)) {
        httpRedirect(conn, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    } else {
        nonce = createDigestNonce(conn, conn->http->secret, auth->realm);
        /* Opaque is unused, set to anything */
        opaque = "799d5";

        if (smatch(auth->qop, "none")) {
            httpSetHeader(conn, "WWW-Authenticate", "Digest realm=\"%s\", nonce=\"%s\"", auth->realm, nonce);
        } else {
            /* qop value of null defaults to "auth" */
            httpSetHeader(conn, "WWW-Authenticate", "Digest realm=\"%s\", domain=\"%s\", "
                "qop=\"auth\", nonce=\"%s\", opaque=\"%s\", algorithm=\"MD5\", stale=\"FALSE\"",
                auth->realm, "/", nonce, opaque);
        }
        httpSetContentType(conn, "text/plain");
        httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
    }
}


/*
    Add the client 'Authorization' header for authenticated requests
    Must first get a 401 response to get the authData.
 */
PUBLIC bool httpDigestSetHeaders(HttpConn *conn, cchar *username, cchar *password)
{
    Http        *http;
    HttpTx      *tx;
    HttpDigest  *dp;
    char        *ha1, *ha2, *digest, *cnonce;

    http = conn->http;
    tx = conn->tx;
    if ((dp = conn->authData) == 0) {
        /* Need to await a failing auth response */
        return 0;
    }
    cnonce = sfmt("%s:%s:%x", http->secret, dp->realm, (int) http->now);
    ha1 = mprGetMD5(sfmt("%s:%s:%s", username, dp->realm, password));
    ha2 = mprGetMD5(sfmt("%s:%s", tx->method, tx->parsedUri->path));
    if (smatch(dp->qop, "auth")) {
        digest = mprGetMD5(sfmt("%s:%s:%s:%s:%s:%s", ha1, dp->nonce, dp->nc, cnonce, dp->qop, ha2));
        httpAddHeader(conn, "Authorization", "Digest username=\"%s\", realm=\"%s\", domain=\"%s\", "
            "algorithm=\"MD5\", qop=\"%s\", cnonce=\"%s\", nc=\"%s\", nonce=\"%s\", opaque=\"%s\", "
            "stale=\"FALSE\", uri=\"%s\", response=\"%s\"", username, dp->realm, dp->domain, dp->qop,
            cnonce, dp->nc, dp->nonce, dp->opaque, tx->parsedUri->path, digest);
    } else {
        digest = mprGetMD5(sfmt("%s:%s:%s", ha1, dp->nonce, ha2));
        httpAddHeader(conn, "Authorization", "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
            "uri=\"%s\", response=\"%s\"", username, dp->realm, dp->nonce, tx->parsedUri->path, digest);
    }
    return 1;
}


/*
    Create a nonce value for digest authentication (RFC 2617)
 */
static char *createDigestNonce(HttpConn *conn, cchar *secret, cchar *realm)
{
    static int64 next = 0;

    assert(realm && *realm);
    return mprEncode64(sfmt("%s:%s:%llx:%llx", secret, realm, mprGetTime(), next++));
}


static int parseDigestNonce(char *nonce, cchar **secret, cchar **realm, MprTime *when)
{
    char    *tok, *decoded, *whenStr;

    if ((decoded = mprDecode64(nonce)) == 0) {
        return MPR_ERR_CANT_READ;
    }
    *secret = stok(decoded, ":", &tok);
    *realm = stok(NULL, ":", &tok);
    whenStr = stok(NULL, ":", &tok);
    *when = (MprTime) stoiradix(whenStr, 16, NULL);
    return 0;
}


/*
    Get a password digest using the MD5 algorithm -- See RFC 2617 to understand this code.
 */
static char *calcDigest(HttpConn *conn, HttpDigest *dp, cchar *username)
{
    HttpAuth    *auth;
    char        *digestBuf, *ha1, *ha2;

    auth = conn->rx->route->auth;
    if (!conn->user) {
        conn->user = mprLookupKey(auth->userCache, username);
    }
    assert(conn->user && conn->user->password);
    if (conn->user == 0 || conn->user->password == 0) {
        return 0;
    }

    /*
        Compute HA1. Password is already expected to be in the HA1 format MD5(username:realm:password).
     */
    ha1 = sclone(conn->user->password);

    /*
        HA2
     */
#if PROTOTYPE || 1
    if (conn->rx->route->flags & HTTP_ROUTE_DOTNET_DIGEST_FIX) {
        char *uri = stok(sclone(dp->uri), "?", 0);
        ha2 = mprGetMD5(sfmt("%s:%s", conn->rx->method, uri));
    } else {
        ha2 = mprGetMD5(sfmt("%s:%s", conn->rx->method, dp->uri));
    }
#else
    ha2 = mprGetMD5(sfmt("%s:%s", conn->rx->method, dp->uri));
#endif

    /*
        H(HA1:nonce:HA2)
     */
    if (scmp(dp->qop, "auth") == 0) {
        digestBuf = sfmt("%s:%s:%s:%s:%s:%s", ha1, dp->nonce, dp->nc, dp->cnonce, dp->qop, ha2);
    } else {
        digestBuf = sfmt("%s:%s:%s", ha1, dp->nonce, ha2);
    }
    return mprGetMD5(digestBuf);
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/dirHandler.c ************/


/*
    dirHandler.c - Directory listing handler

    The dirHandler is unusual in that is is called (only) from the fileHandler.
    The fileHandler tests if the request is for a directory and then examines if redirection
    to an index, or rendering a directory listing is required. If a listing, the request is
    relayed here.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/



/********************************** Defines ***********************************/

#define DIR_NAME "dirHandler"

/****************************** Forward Declarations **************************/

static void filterDirList(HttpConn *conn, MprList *list);
static int  matchDirPattern(cchar *pattern, cchar *file);
static void outputFooter(HttpQueue *q);
static void outputHeader(HttpQueue *q, cchar *dir, int nameSize);
static void outputLine(HttpQueue *q, MprDirEntry *ep, cchar *dir, int nameSize);
static void parseQuery(HttpConn *conn);
static void sortList(HttpConn *conn, MprList *list);

/************************************* Code ***********************************/
/*
    Test if this request is for a directory listing. This routine is called directly by the fileHandler.
    Directory listings are enabled in a route via "Options Indexes".
 */
PUBLIC bool httpRenderDirListing(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpDir     *dir;

    tx = conn->tx;
    rx = conn->rx;
    assert(tx->filename);
    assert(tx->fileInfo.checked);

    if ((dir = httpGetRouteData(rx->route, DIR_NAME)) == 0) {
        return 0;
    }
    if (dir->enabled && tx->fileInfo.isDir && sends(rx->pathInfo, "/")) {
        conn->reqData = dir;
        return 1;
    }
    return 0;
}


/*
    Start the request (and complete it)
 */
static void startDir(HttpQueue *q)
{
    HttpConn        *conn;
    HttpTx          *tx;
    HttpRx          *rx;
    MprList         *list;
    MprDirEntry     *dp;
    HttpDir         *dir;
    cchar           *path;
    uint            nameSize;
    int             next;

    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;
    if ((dir = conn->reqData) == 0) {
        httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot get directory listing");
        return;
    }
    assert(tx->filename);

    if (!(rx->flags & (HTTP_GET | HTTP_HEAD))) {
        httpError(conn, HTTP_CODE_BAD_METHOD, "Bad method");
        return;
    }
    httpSetHeaderString(conn, "Cache-Control", "no-cache");
    httpSetHeaderString(conn, "Last-Modified", conn->http->currentDate);
    httpSetHeaderString(conn, "Content-Type", "text/html");
    parseQuery(conn);

    if ((list = mprGetPathFiles(tx->filename, MPR_PATH_RELATIVE)) == 0) {
        httpWrite(q, "<h2>Cannot get file list</h2>\r\n");
        outputFooter(q);
        return;
    }
    if (dir->pattern) {
        filterDirList(conn, list);
    }
    sortList(conn, list);
    /*
        Get max filename size
     */
    nameSize = 0;
    for (next = 0; (dp = mprGetNextItem(list, &next)) != 0; ) {
        nameSize = max((int) strlen(dp->name), nameSize);
    }
    nameSize = max(nameSize, 22);

    path = rx->route->prefix ? sjoin(rx->route->prefix, rx->pathInfo, NULL) : rx->pathInfo;
    outputHeader(q, path, nameSize);
    for (next = 0; (dp = mprGetNextItem(list, &next)) != 0; ) {
        outputLine(q, dp, tx->filename, nameSize);
    }
    outputFooter(q);
    httpFinalize(conn);
}
 

static void parseQuery(HttpConn *conn)
{
    HttpRx      *rx;
    HttpDir     *dir;
    char        *value, *query, *next, *tok, *field;

    rx = conn->rx;
    dir = conn->reqData;
    
    query = sclone(rx->parsedUri->query);
    if (query == 0) {
        return;
    }
    tok = stok(query, ";&", &next);
    while (tok) {
        if ((value = strchr(tok, '=')) != 0) {
            *value++ = '\0';
            if (*tok == 'C') {                  /* Sort column */
                field = 0;
                if (*value == 'N') {
                    field = "Name";
                } else if (*value == 'M') {
                    field = "Date";
                } else if (*value == 'S') {
                    field = "Size";
                }
                if (field) {
                    dir->sortField = sclone(field);
                }

            } else if (*tok == 'O') {           /* Sort order */
                if (*value == 'A') {
                    dir->sortOrder = 1;
                } else if (*value == 'D') {
                    dir->sortOrder = -1;
                }

            } else if (*tok == 'F') {           /* Format */ 
                if (*value == '0') {
                    dir->fancyIndexing = 0;
                } else if (*value == '1') {
                    dir->fancyIndexing = 1;
                } else if (*value == '2') {
                    dir->fancyIndexing = 2;
                }

            } else if (*tok == 'P') {           /* Pattern */ 
                dir->pattern = sclone(value);
            }
        }
        tok = stok(next, ";&", &next);
    }
}


static void sortList(HttpConn *conn, MprList *list)
{
    MprDirEntry *tmp, **items;
    HttpDir     *dir;
    int         count, i, j, rc;

    dir = conn->reqData;
    
    if (dir->sortField == 0) {
        return;
    }
    count = mprGetListLength(list);
    items = (MprDirEntry**) list->items;
    if (scaselessmatch(dir->sortField, "Name")) {
        for (i = 1; i < count; i++) {
            for (j = 0; j < i; j++) {
                rc = strcmp(items[i]->name, items[j]->name);
                if (dir->foldersFirst) {
                    if (items[i]->isDir && !items[j]->isDir) {
                        rc = -dir->sortOrder;
                    } else if (items[j]->isDir && !items[i]->isDir) {
                        rc = dir->sortOrder;
                    } 
                }
                rc *= dir->sortOrder;
                if (rc < 0) {
                    tmp = items[i];
                    items[i] = items[j];
                    items[j] = tmp;
                }
            }
        }

    } else if (scaselessmatch(dir->sortField, "Size")) {
        for (i = 1; i < count; i++) {
            for (j = 0; j < i; j++) {
                rc = (items[i]->size < items[j]->size) ? -1 : 1;
                if (dir->foldersFirst) {
                    if (items[i]->isDir && !items[j]->isDir) {
                        rc = -dir->sortOrder;
                    } else if (items[j]->isDir && !items[i]->isDir) {
                        rc = dir->sortOrder;
                    }
                }
                rc *= dir->sortOrder;
                if (rc < 0) {
                    tmp = items[i];
                    items[i] = items[j];
                    items[j] = tmp;
                }
            }
        }

    } else if (scaselessmatch(dir->sortField, "Date")) {
        for (i = 1; i < count; i++) {
            for (j = 0; j < i; j++) {
                rc = (items[i]->lastModified < items[j]->lastModified) ? -1: 1;
                if (dir->foldersFirst) {
                    if (items[i]->isDir && !items[j]->isDir) {
                        rc = -dir->sortOrder;
                    } else if (items[j]->isDir && !items[i]->isDir) {
                        rc = dir->sortOrder;
                    }
                }
                rc *= dir->sortOrder;
                if (rc < 0) {
                    tmp = items[i];
                    items[i] = items[j];
                    items[j] = tmp;
                }
            }
        }
    }
}


static void outputHeader(HttpQueue *q, cchar *path, int nameSize)
{
    HttpDir *dir;
    char    *parent, *parentSuffix;
    int     reverseOrder, fancy, isRootDir;

    dir = q->conn->reqData;
    fancy = 1;
    path = mprEscapeHtml(path);

    httpWrite(q, "<!DOCTYPE HTML PUBLIC \"-/*W3C//DTD HTML 3.2 Final//EN\">\r\n");
    httpWrite(q, "<html>\r\n <head>\r\n  <title>Index of %s</title>\r\n", path);
    httpWrite(q, " </head>\r\n");
    httpWrite(q, "<body>\r\n");

    httpWrite(q, "<h1>Index of %s</h1>\r\n", path);

    if (dir->sortOrder > 0) {
        reverseOrder = 'D';
    } else {
        reverseOrder = 'A';
    }
    if (dir->fancyIndexing == 0) {
        fancy = '0';
    } else if (dir->fancyIndexing == 1) {
        fancy = '1';
    } else if (dir->fancyIndexing == 2) {
        fancy = '2';
    }
    parent = mprGetPathDir(path);
    if (parent[strlen(parent) - 1] != '/') {
        parentSuffix = "/";
    } else {
        parentSuffix = "";
    }
    isRootDir = (strcmp(path, "/") == 0);

    if (dir->fancyIndexing == 2) {
        httpWrite(q, "<table><tr><th><img src=\"/icons/blank.gif\" alt=\"[ICO]\" /></th>");

        httpWrite(q, "<th><a href=\"?C=N;O=%c;F=%c\">Name</a></th>", reverseOrder, fancy);
        httpWrite(q, "<th><a href=\"?C=M;O=%c;F=%c\">Last modified</a></th>", reverseOrder, fancy);
        httpWrite(q, "<th><a href=\"?C=S;O=%c;F=%c\">Size</a></th>", reverseOrder, fancy);
        httpWrite(q, "<th><a href=\"?C=D;O=%c;F=%c\">Description</a></th>\r\n", reverseOrder, fancy);

        httpWrite(q, "</tr><tr><th colspan=\"5\"><hr /></th></tr>\r\n");

        if (! isRootDir) {
            httpWrite(q, "<tr><td valign=\"top\"><img src=\"/icons/back.gif\"");
            httpWrite(q, "alt=\"[DIR]\" /></td><td><a href=\"%s%s\">", parent, parentSuffix);
            httpWrite(q, "Parent Directory</a></td>");
            httpWrite(q, "<td align=\"right\">  - </td></tr>\r\n");
        }

    } else if (dir->fancyIndexing == 1) {
        httpWrite(q, "<pre><img src=\"/icons/space.gif\" alt=\"Icon\" /> ");

        httpWrite(q, "<a href=\"?C=N;O=%c;F=%c\">Name</a>%*s", reverseOrder, fancy, nameSize - 3, " ");
        httpWrite(q, "<a href=\"?C=M;O=%c;F=%c\">Last modified</a>       ", reverseOrder, fancy);
        httpWrite(q, "<a href=\"?C=S;O=%c;F=%c\">Size</a>               ", reverseOrder, fancy);
        httpWrite(q, "<a href=\"?C=D;O=%c;F=%c\">Description</a>\r\n", reverseOrder, fancy);

        httpWrite(q, "<hr />");

        if (! isRootDir) {
            httpWrite(q, "<img src=\"/icons/parent.gif\" alt=\"[DIR]\" />");
            httpWrite(q, " <a href=\"%s%s\">Parent Directory</a>\r\n", parent, parentSuffix);
        }

    } else {
        httpWrite(q, "<ul>\n");
        if (! isRootDir) {
            httpWrite(q, "<li><a href=\"%s%s\"> Parent Directory</a></li>\r\n", parent, parentSuffix);
        }
    }
}


static void fmtNum(char *buf, int bufsize, int num, int divisor, char *suffix)
{
    int     whole, point;

    whole = num / divisor;
    point = (num % divisor) / (divisor / 10);

    if (point == 0) {
        fmt(buf, bufsize, "%6d%s", whole, suffix);
    } else {
        fmt(buf, bufsize, "%4d.%d%s", whole, point, suffix);
    }
}


static void outputLine(HttpQueue *q, MprDirEntry *ep, cchar *path, int nameSize)
{
    MprPath     info;
    MprTime     when;
    HttpDir     *dir;
    char        *newPath, sizeBuf[16], timeBuf[48], *icon;
    struct tm   tm;
    bool        isDir;
    int         len;
    cchar       *ext, *mimeType;
    char        *dirSuffix;
    char        *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    path = mprEscapeHtml(path);
    dir = q->conn->reqData;
    if (ep->size >= (1024 * 1024 * 1024)) {
        fmtNum(sizeBuf, sizeof(sizeBuf), (int) ep->size, 1024 * 1024 * 1024, "G");

    } else if (ep->size >= (1024 * 1024)) {
        fmtNum(sizeBuf, sizeof(sizeBuf), (int) ep->size, 1024 * 1024, "M");

    } else if (ep->size >= 1024) {
        fmtNum(sizeBuf, sizeof(sizeBuf), (int) ep->size, 1024, "K");

    } else {
        fmt(sizeBuf, sizeof(sizeBuf), "%6d", (int) ep->size);
    }
    newPath = mprJoinPath(path, ep->name);

    if (mprGetPathInfo(newPath, &info) < 0) {
        when = mprGetTime();
        isDir = 0;
    } else {
        isDir = info.isDir ? 1 : 0;
        when = (MprTime) info.mtime * MPR_TICKS_PER_SEC;
    }
    if (isDir) {
        icon = "folder";
        dirSuffix = "/";
    } else {
        ext = mprGetPathExt(ep->name);
        if (ext && (mimeType = mprLookupMime(q->conn->rx->route->mimeTypes, ext)) != 0) {
            if (strcmp(ext, "es") == 0 || strcmp(ext, "ejs") == 0 || strcmp(ext, "php") == 0) {
                icon = "text";
            } else if (strstr(mimeType, "text") != 0) {
                icon = "text";
            } else {
                icon = "compressed";
            }
        } else {
            icon = "compressed";
        }
        dirSuffix = "";
    }
    mprDecodeLocalTime(&tm, when);

    fmt(timeBuf, sizeof(timeBuf), "%02d-%3s-%4d %02d:%02d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, 
        tm.tm_hour,  tm.tm_min);
    len = (int) strlen(ep->name) + (int) strlen(dirSuffix);

    if (dir->fancyIndexing == 2) {
        httpWrite(q, "<tr><td valign=\"top\">");
        httpWrite(q, "<img src=\"/icons/%s.gif\" alt=\"[   ]\", /></td>", icon);
        httpWrite(q, "<td><a href=\"%s%s\">%s%s</a></td>", ep->name, dirSuffix, ep->name, dirSuffix);
        httpWrite(q, "<td>%s</td><td>%s</td></tr>\r\n", timeBuf, sizeBuf);

    } else if (dir->fancyIndexing == 1) {
        httpWrite(q, "<img src=\"/icons/%s.gif\" alt=\"[   ]\", /> ", icon);
        httpWrite(q, "<a href=\"%s%s\">%s%s</a>%-*s %17s %4s\r\n", ep->name, dirSuffix, ep->name, dirSuffix, 
            nameSize - len, "", timeBuf, sizeBuf);

    } else {
        httpWrite(q, "<li><a href=\"%s%s\"> %s%s</a></li>\r\n", ep->name, dirSuffix, ep->name, dirSuffix);
    }
}


static void outputFooter(HttpQueue *q)
{
    HttpConn    *conn;
    MprSocket   *sock;
    HttpDir     *dir;
    
    conn = q->conn;
    dir = conn->reqData;
    
    if (dir->fancyIndexing == 2) {
        httpWrite(q, "<tr><th colspan=\"5\"><hr /></th></tr>\r\n</table>\r\n");
        
    } else if (dir->fancyIndexing == 1) {
        httpWrite(q, "<hr /></pre>\r\n");
    } else {
        httpWrite(q, "</ul>\r\n");
    }
    sock = conn->sock->listenSock;
    httpWrite(q, "<address>%s %s at %s Port %d</address>\r\n", ME_TITLE, ME_VERSION, sock->ip, sock->port);
    httpWrite(q, "</body></html>\r\n");
}


static void filterDirList(HttpConn *conn, MprList *list)
{
    HttpDir         *dir;
    MprDirEntry     *dp;
    int             next;

    dir = conn->reqData;
    
    /*
        Do pattern matching. Entries that don't match, free the name to mark
     */
    for (ITERATE_ITEMS(list, dp, next)) {
        if (!matchDirPattern(dir->pattern, dp->name)) {
            mprRemoveItem(list, dp);
            next--;
        }
    }
}


/*
    Return true if the file matches the pattern. Supports '?' and '*'
 */
static int matchDirPattern(cchar *pattern, cchar *file)
{
    cchar   *pp, *fp;

    if (pattern == 0 || *pattern == '\0') {
        return 1;
    }
    if (file == 0 || *file == '\0') {
        return 0;
    }
    for (pp = pattern, fp = file; *pp; ) {
        if (*fp == '\0') {
            if (*pp == '*' && pp[1] == '\0') {
                /* Trailing wild card */
                return 1;
            }
            return 0;
        }
        if (*pp == '*') {
            if (matchDirPattern(&pp[1], &fp[0])) {
                return 1;
            }
            fp++;
            continue;

        } else if (*pp == '?' || *pp == *fp) {
            fp++;

        } else {
            return 0;
        }
        pp++;
    }
    if (*fp == '\0') {
        /* Match */
        return 1;
    }
    return 0;
}


#if KEEP
static int addIconDirective(MaState *state, cchar *key, cchar *value)
{
    if (!maTokenize(state, value, "%S %W", &path, &dir->extList)) {
        return MPR_ERR_BAD_SYNTAX;
    }
    return 0;
}


static int defaultIconDirective(MaState *state, cchar *key, cchar *value)
{
    state->dir->defaultIcon = sclone(value);
    return 0;
}

/*  
    IndexIgnore pat ... 
 */
static int indexIgnoreDirective(MaState *state, cchar *key, cchar *value)
{
    if (!maTokenize(state, value, "%W", &dir->ignoreList)) {
        return MPR_ERR_BAD_SYNTAX;
    }
    return 0;
}
#endif


static void manageDir(HttpDir *dir, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
#if KEEP
        mprMark(dir->dirList);
        mprMark(dir->defaultIcon);
        mprMark(dir->extList);
        mprMark(dir->ignoreList);
#endif
        mprMark(dir->pattern);
        mprMark(dir->sortField);
    }
}


static HttpDir *allocDir(HttpRoute *route)
{
    HttpDir *dir;

    if ((dir = mprAllocObj(HttpDir, manageDir)) == 0) {
        return 0;
    }
    httpSetRouteData(route, DIR_NAME, dir);
    return dir;
}


static HttpDir *cloneDir(HttpDir *parent, HttpRoute *route)
{
    HttpDir *dir;

    if ((dir = mprAllocObj(HttpDir, manageDir)) == 0) {
        return 0;
    }
    dir->enabled = parent->enabled;
    dir->fancyIndexing = parent->fancyIndexing;
    dir->foldersFirst = parent->foldersFirst;
    dir->pattern = parent->pattern;
    dir->sortField = parent->sortField;
    dir->sortOrder = parent->sortOrder;
    httpSetRouteData(route, DIR_NAME, dir);
    return dir;
}


PUBLIC HttpDir *httpGetDirObj(HttpRoute *route)
{
    HttpDir     *dir, *parent;

    dir = httpGetRouteData(route, DIR_NAME);
    if (route->parent) {
        /*
            If the parent route has the same route data, then force a clone so the parent route does not get modified
         */ 
        parent = httpGetRouteData(route->parent, DIR_NAME);
        if (dir == parent) {
            dir = 0;
        }
    }
    if (dir == 0) {
        if (route->parent && (parent = httpGetRouteData(route->parent, DIR_NAME)) != 0) {
            dir = cloneDir(parent, route);
        } else {
            dir = allocDir(route);
        }
    }
    assert(dir);
    return dir;
}


/*
    Loadable module initialization
 */
PUBLIC int httpOpenDirHandler()
{
    HttpStage   *handler;
    HttpDir     *dir;

    if ((handler = httpCreateHandler("dirHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    if ((handler->stageData = dir = mprAllocObj(HttpDir, manageDir)) == 0) {
        return MPR_ERR_MEMORY;
    }
    handler->flags |= HTTP_STAGE_INTERNAL;
    handler->start = startDir; 
    HTTP->dirHandler = handler;
    dir->sortOrder = 1;
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/endpoint.c ************/


/*
    endpoint.c -- Create and manage listening endpoints.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void acceptConn(HttpEndpoint *endpoint);
static int manageEndpoint(HttpEndpoint *endpoint, int flags);

/************************************ Code ************************************/
/*
    Create a listening endpoint on ip:port. NOTE: ip may be empty which means bind to all addresses.
 */
PUBLIC HttpEndpoint *httpCreateEndpoint(cchar *ip, int port, MprDispatcher *dispatcher)
{
    HttpEndpoint    *endpoint;

    if ((endpoint = mprAllocObj(HttpEndpoint, manageEndpoint)) == 0) {
        return 0;
    }
    endpoint->http = HTTP;
    endpoint->async = 1;
    endpoint->port = port;
    endpoint->ip = sclone(ip);
    endpoint->dispatcher = dispatcher;
    endpoint->hosts = mprCreateList(-1, MPR_LIST_STABLE);
    endpoint->mutex = mprCreateLock();
    httpAddEndpoint(endpoint);
    return endpoint;
}


PUBLIC void httpDestroyEndpoint(HttpEndpoint *endpoint)
{
    if (endpoint->sock) {
        mprCloseSocket(endpoint->sock, 0);
        endpoint->sock = 0;
    }
    httpRemoveEndpoint(endpoint);
}


static int manageEndpoint(HttpEndpoint *endpoint, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(endpoint->http);
        mprMark(endpoint->hosts);
        mprMark(endpoint->ip);
        mprMark(endpoint->context);
        mprMark(endpoint->limits);
        mprMark(endpoint->sock);
        mprMark(endpoint->dispatcher);
        mprMark(endpoint->ssl);
        mprMark(endpoint->mutex);
    }
    return 0;
}


/*
    Convenience function to create and configure a new endpoint without using a config file.
 */
PUBLIC HttpEndpoint *httpCreateConfiguredEndpoint(HttpHost *host, cchar *home, cchar *documents, cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    HttpRoute       *route;

    if (host == 0) {
        host = httpGetDefaultHost();
    }
    if (host == 0) {
        return 0;
    }
    if (ip == 0 && port <= 0) {
        /*
            If no IP:PORT specified, find the first endpoint
         */
        if ((endpoint = mprGetFirstItem(HTTP->endpoints)) != 0) {
            ip = endpoint->ip;
            port = endpoint->port;
        } else {
            ip = "localhost";
            if (port <= 0) {
                port = ME_HTTP_PORT;
            }
            if ((endpoint = httpCreateEndpoint(ip, port, NULL)) == 0) {
                return 0;
            }
        }
    } else if ((endpoint = httpCreateEndpoint(ip, port, NULL)) == 0) {
        return 0;
    }
    route = host->defaultRoute;
    httpAddHostToEndpoint(endpoint, host);
    if (documents) {
        httpSetRouteDocuments(route, documents);
    }
    if (home) {
        httpSetRouteHome(route, home);
    }
    httpFinalizeRoute(route);
    return endpoint;
}


/*
    Add the default host to the unassigned endpoints
 */
PUBLIC void httpAddHostToEndpoints(HttpHost *host)
{
    HttpEndpoint    *endpoint;
    int             next;

    if (host == 0) {
        return;
    }
    for (next = 0; (endpoint = mprGetNextItem(HTTP->endpoints, &next)) != 0; ) {
        httpAddHostToEndpoint(endpoint, host);
        if (!host->name) {
            httpSetHostName(host, sfmt("%s:%d", endpoint->ip, endpoint->port));
        }
    }
}


static bool validateEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    HttpRoute   *route;
    int         nextRoute;

    if ((host = mprGetFirstItem(endpoint->hosts)) == 0) {
        host = httpGetDefaultHost();
        httpAddHostToEndpoint(endpoint, host);
        if (!host->name) {
            httpSetHostName(host, sfmt("%s:%d", endpoint->ip, endpoint->port));
        }
        for (nextRoute = 0; (route = mprGetNextItem(host->routes, &nextRoute)) != 0; ) {
            if (!route->handler && !mprLookupKey(route->extensions, "")) {
                httpAddRouteHandler(route, "fileHandler", "");
                httpAddRouteIndex(route, "index.html");
            }
        }
    }
    return 1;
}


PUBLIC int httpStartEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    cchar       *proto, *ip;
    int         next;

    if (!validateEndpoint(endpoint)) {
        return MPR_ERR_BAD_ARGS;
    }
    for (ITERATE_ITEMS(endpoint->hosts, host, next)) {
        httpStartHost(host);
    }
    if ((endpoint->sock = mprCreateSocket()) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (mprListenOnSocket(endpoint->sock, endpoint->ip, endpoint->port, 
                MPR_SOCKET_NODELAY | MPR_SOCKET_THREAD) == SOCKET_ERROR) {
        if (mprGetError() == EADDRINUSE) {
            mprLog("error http", 0, "Cannot open a socket on %s:%d, socket already bound.", 
                *endpoint->ip ? endpoint->ip : "*", endpoint->port);
        } else {
            mprLog("error http", 0, "Cannot open a socket on %s:%d", *endpoint->ip ? endpoint->ip : "*", endpoint->port);
        }
        return MPR_ERR_CANT_OPEN;
    }
    if (endpoint->http->listenCallback && (endpoint->http->listenCallback)(endpoint) < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    if (endpoint->async && !endpoint->sock->handler) {
        mprAddSocketHandler(endpoint->sock, MPR_SOCKET_READABLE, endpoint->dispatcher, acceptConn, endpoint, 
            (endpoint->dispatcher ? 0 : MPR_WAIT_NEW_DISPATCHER) | MPR_WAIT_IMMEDIATE);
    } else {
        mprSetSocketBlockingMode(endpoint->sock, 1);
    }
    proto = endpoint->ssl ? "HTTPS" : "HTTP";
    ip = *endpoint->ip ? endpoint->ip : "*";
    if (mprIsSocketV6(endpoint->sock)) {
        mprLog("info http", HTTP->startLevel, "Started %s service on [%s]:%d", proto, ip, endpoint->port);
    } else {
        mprLog("info http", HTTP->startLevel, "Started %s service on %s:%d", proto, ip, endpoint->port);
    }
    return 0;
}


PUBLIC void httpStopEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    int         next;

    for (ITERATE_ITEMS(endpoint->hosts, host, next)) {
        httpStopHost(host);
    }
    if (endpoint->sock) {
        mprCloseSocket(endpoint->sock, 0);
        endpoint->sock = 0;
    }
}


/*
    This routine runs using the service event thread. It accepts the socket and creates an event on a new dispatcher to 
    manage the connection. When it returns, it immediately can listen for new connections without having to modify the 
    event listen masks.
 */
static void acceptConn(HttpEndpoint *endpoint)
{
    MprDispatcher   *dispatcher;
    MprEvent        *event;
    MprSocket       *sock;
    MprWaitHandler  *wp;

    if ((sock = mprAcceptSocket(endpoint->sock)) == 0) {
        return;
    }
    wp = endpoint->sock->handler;
    if (wp->flags & MPR_WAIT_NEW_DISPATCHER) {
        dispatcher = mprCreateDispatcher("IO", MPR_DISPATCHER_AUTO);
    } else if (wp->dispatcher) {
        dispatcher = wp->dispatcher;
    } else {
        dispatcher = mprGetDispatcher();
    }
    event = mprCreateEvent(dispatcher, "AcceptConn", 0, httpAcceptConn, endpoint, MPR_EVENT_DONT_QUEUE);
    event->mask = wp->presentMask;
    event->sock = sock;
    event->handler = wp;
    /*
        Optimization to wake the event service in this amount of time. This ensures that when the HttpTimer is scheduled,
        it won't need to awaken the notifier.
     */
    mprSetEventServiceSleep(HTTP_TIMER_PERIOD);
    mprQueueEvent(dispatcher, event);
}


PUBLIC void httpMatchHost(HttpConn *conn)
{ 
    MprSocket       *listenSock;
    HttpEndpoint    *endpoint;
    HttpHost        *host;

    listenSock = conn->sock->listenSock;

    if ((endpoint = httpLookupEndpoint(listenSock->ip, listenSock->port)) == 0) {
        conn->host = mprGetFirstItem(endpoint->hosts);
        httpError(conn, HTTP_CODE_NOT_FOUND, "No listening endpoint for request from %s:%d", 
            listenSock->ip, listenSock->port);
        return;
    }
    host = httpLookupHostOnEndpoint(endpoint, conn->rx->hostHeader);
    if (host == 0) {
        conn->host = mprGetFirstItem(endpoint->hosts);
        httpError(conn, HTTP_CODE_NOT_FOUND, "No host to serve request. Searching for %s", conn->rx->hostHeader);
        return;
    }
    conn->host = host;
}


PUBLIC void *httpGetEndpointContext(HttpEndpoint *endpoint)
{
    assert(endpoint);
    if (endpoint) {
        return endpoint->context;
    }
    return 0;
}


PUBLIC int httpIsEndpointAsync(HttpEndpoint *endpoint) 
{
    assert(endpoint);
    if (endpoint) {
        return endpoint->async;
    }
    return 0;
}


PUBLIC int httpSetEndpointAddress(HttpEndpoint *endpoint, cchar *ip, int port)
{
    assert(endpoint);

    if (ip) {
        endpoint->ip = sclone(ip);
    }
    if (port >= 0) {
        endpoint->port = port;
    }
    if (endpoint->sock) {
        httpStopEndpoint(endpoint);
        if (httpStartEndpoint(endpoint) < 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    return 0;
}


PUBLIC void httpSetEndpointAsync(HttpEndpoint *endpoint, int async)
{
    if (endpoint->sock) {
        if (endpoint->async && !async) {
            mprSetSocketBlockingMode(endpoint->sock, 1);
        }
        if (!endpoint->async && async) {
            mprSetSocketBlockingMode(endpoint->sock, 0);
        }
    }
    endpoint->async = async;
}


PUBLIC void httpSetEndpointContext(HttpEndpoint *endpoint, void *context)
{
    assert(endpoint);
    endpoint->context = context;
}


PUBLIC void httpSetEndpointNotifier(HttpEndpoint *endpoint, HttpNotifier notifier)
{
    assert(endpoint);
    endpoint->notifier = notifier;
}


PUBLIC int httpSecureEndpoint(HttpEndpoint *endpoint, struct MprSsl *ssl)
{
#if ME_COM_SSL
    endpoint->ssl = ssl;
    return 0;
#else
    mprLog("error http", 0, "Configuration lacks SSL support");
    return MPR_ERR_BAD_STATE;
#endif
}


PUBLIC int httpSecureEndpointByName(cchar *name, struct MprSsl *ssl)
{
    HttpEndpoint    *endpoint;
    char            *ip;
    int             port, next, count;

    mprParseSocketAddress(name, &ip, &port, NULL, -1);
    if (ip == 0) {
        ip = "";
    }
    for (count = 0, next = 0; (endpoint = mprGetNextItem(HTTP->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assert(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                httpSecureEndpoint(endpoint, ssl);
                count++;
            }
        }
    }
    return (count == 0) ? MPR_ERR_CANT_FIND : 0;
}


PUBLIC void httpAddHostToEndpoint(HttpEndpoint *endpoint, HttpHost *host)
{
    if (mprLookupItem(endpoint->hosts, host) < 0) {
        mprAddItem(endpoint->hosts, host);
    }
    if (endpoint->limits == 0) {
        endpoint->limits = host->defaultRoute->limits;
    }
}


PUBLIC HttpHost *httpLookupHostOnEndpoint(HttpEndpoint *endpoint, cchar *hostHeader)
{
    HttpHost    *host;
    int         next;

    if (hostHeader == 0 || *hostHeader == '\0' || mprGetListLength(endpoint->hosts) <= 1) {
        return mprGetFirstItem(endpoint->hosts);
    }
    for (next = 0; (host = mprGetNextItem(endpoint->hosts, &next)) != 0; ) {
        if (smatch(host->name, hostHeader)) {
            return host;
        }
        if (*host->name == '\0') {
            /* Match all hosts */
            return host;
        }
        if (host->flags & HTTP_HOST_WILD_STARTS) {
            if (sstarts(hostHeader, host->name)) {
                return host;
            }
        } else if (host->flags & HTTP_HOST_WILD_CONTAINS) {
            if (scontains(hostHeader, host->name)) {
                return host;
            }
        }
    }
    return 0;
}


PUBLIC void httpSetInfoLevel(int level)
{
    HTTP->startLevel = level;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/error.c ************/


/*
    error.c -- Http error handling
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void errorv(HttpConn *conn, int flags, cchar *fmt, va_list args);
static char *formatErrorv(HttpConn *conn, int status, cchar *fmt, va_list args);

/*********************************** Code *************************************/

PUBLIC void httpBadRequestError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    if (httpServerConn(conn)) {
        httpMonitorEvent(conn, HTTP_COUNTER_BAD_REQUEST_ERRORS, 1);
    }
    errorv(conn, flags, fmt, args);
    va_end(args);
}


PUBLIC void httpLimitError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    if (httpServerConn(conn)) {
        httpMonitorEvent(conn, HTTP_COUNTER_LIMIT_ERRORS, 1);
    }
    errorv(conn, flags, fmt, args);
    va_end(args);
}


PUBLIC void httpError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    errorv(conn, flags, fmt, args);
    va_end(args);
}


static void errorRedirect(HttpConn *conn, cchar *uri)
{
    HttpTx      *tx;

    /*
        If the response has started or it is an external redirect ... do a redirect
     */
    tx = conn->tx;
    if (sstarts(uri, "http") || tx->flags & HTTP_TX_HEADERS_CREATED) {
        httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, uri);
    } else {
        /*
            No response started and it is an internal redirect, so we can rerun the request.
            Set finalized to "cap" any output. processCompletion() in rx.c will rerun the request using the errorDocument.
         */
        tx->errorDocument = uri;
        tx->finalized = tx->finalizedOutput = tx->finalizedConnector = 1;
    }
}


static void makeAltBody(HttpConn *conn, int status)
{
    HttpRx      *rx;
    HttpTx      *tx;
    cchar       *statusMsg, *msg;

    rx = conn->rx;
    tx = conn->tx;
    assert(rx && tx);

    statusMsg = httpLookupStatus(status);
    msg = "";
    if (rx && (!rx->route || rx->route->flags & HTTP_ROUTE_SHOW_ERRORS)) {
        msg = conn->errorMsg;
    }
    if (rx && scmp(rx->accept, "text/plain") == 0) {
        tx->altBody = sfmt("Access Error: %d -- %s\r\n%s\r\n", status, statusMsg, msg);
    } else {
        httpSetContentType(conn, "text/html");
        tx->altBody = sfmt("<!DOCTYPE html>\r\n"
            "<head>\r\n"
            "    <title>%s</title>\r\n"
            "    <link rel=\"shortcut icon\" href=\"data:image/x-icon;,\" type=\"image/x-icon\">\r\n"
            "</head>\r\n"
            "<body>\r\n<h2>Access Error: %d -- %s</h2>\r\n<pre>%s</pre>\r\n</body>\r\n</html>\r\n",
            statusMsg, status, statusMsg, mprEscapeHtml(msg));
    }
    tx->length = slen(tx->altBody);
}


/*
    The current request has an error and cannot complete as normal. This call sets the Http response status and 
    overrides the normal output with an alternate error message. If the output has alread started (headers sent), then
    the connection MUST be closed so the client can get some indication the request failed.
 */
static void errorv(HttpConn *conn, int flags, cchar *fmt, va_list args)
{
    HttpRx      *rx;
    HttpTx      *tx;
    cchar       *uri;
    int         status;

    rx = conn->rx;
    tx = conn->tx;

    if (conn == 0 || fmt == 0) {
        return;
    }
    status = flags & HTTP_CODE_MASK;
    if (status == 0) {
        status = HTTP_CODE_INTERNAL_SERVER_ERROR;
    }
    if (flags & (HTTP_ABORT | HTTP_CLOSE)) {
        conn->keepAliveCount = 0;
    }
    if (flags & HTTP_ABORT) {
        conn->connError++;
    }
    if (!conn->error) {
        conn->error++;
        httpOmitBody(conn);
        conn->errorMsg = formatErrorv(conn, status, fmt, args);
        httpTrace(conn, "request.error", "error", "msg:'%s'", conn->errorMsg);
        HTTP_NOTIFY(conn, HTTP_EVENT_ERROR, 0);
        if (httpServerConn(conn)) {
            if (status == HTTP_CODE_NOT_FOUND) {
                httpMonitorEvent(conn, HTTP_COUNTER_NOT_FOUND_ERRORS, 1);
            }
            httpMonitorEvent(conn, HTTP_COUNTER_ERRORS, 1);
        }
        httpAddHeaderString(conn, "Cache-Control", "no-cache");
        if (httpServerConn(conn) && tx && rx) {
            if (tx->flags & HTTP_TX_HEADERS_CREATED) {
                /* 
                    If the response headers have been sent, must let the other side of the failure ... aborting
                    the request is the only way as the status has been sent.
                 */
                flags |= HTTP_ABORT;
            } else {
                if (rx->route && (uri = httpLookupRouteErrorDocument(rx->route, tx->status)) && !smatch(uri, rx->uri)) {
                    errorRedirect(conn, uri);
                } else {
                    makeAltBody(conn, status);
                }
            }
        }
        httpFinalize(conn);
    }
    if (flags & HTTP_ABORT) {
        httpDisconnect(conn);
    }
}


/*
    Just format conn->errorMsg and set status - nothing more
    NOTE: this is an internal API. Users should use httpError()
 */
static char *formatErrorv(HttpConn *conn, int status, cchar *fmt, va_list args)
{
    if (conn->errorMsg == 0) {
        conn->errorMsg = sfmtv(fmt, args);
        if (status) {
            if (status < 0) {
                status = HTTP_CODE_INTERNAL_SERVER_ERROR;
            }
            if (httpServerConn(conn) && conn->tx) {
                conn->tx->status = status;
            } else if (conn->rx) {
                conn->rx->status = status;
            }
        }
    }
    return conn->errorMsg;
}


PUBLIC cchar *httpGetError(HttpConn *conn)
{
    if (conn->errorMsg) {
        return conn->errorMsg;
    } else if (conn->state >= HTTP_STATE_FIRST) {
        return httpLookupStatus(conn->rx->status);
    } else {
        return "";
    }
}


PUBLIC void httpMemoryError(HttpConn *conn)
{
    httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Memory allocation error");
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/fileHandler.c ************/


/*
    fileHandler.c -- Static file content handler

    This handler manages static file based content such as HTML, GIF /or JPEG pages. It supports all methods including:
    GET, PUT, DELETE, OPTIONS and TRACE. It is event based and does not use worker threads.

    The fileHandler also manages requests for directories that require redirection to an index or responding with
    a directory listing. 

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/***************************** Forward Declarations ***************************/

static void handleDeleteRequest(HttpQueue *q);
static void handlePutRequest(HttpQueue *q);
static int handleDirectory(HttpConn *conn);
static ssize readFileData(HttpQueue *q, HttpPacket *packet, MprOff pos, ssize size);

/*********************************** Code *************************************/
/*
    Rewrite the request for directories, indexes and compressed content. 
 */
static int rewriteFileHandler(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprPath     *info;

    rx = conn->rx;
    tx = conn->tx;
    info = &tx->fileInfo;

    httpMapFile(conn);
    assert(info->checked);

    if (rx->flags & (HTTP_DELETE | HTTP_PUT)) {
        return HTTP_ROUTE_OK;
    }
    if (info->isDir) {
        return handleDirectory(conn);
    }
    if (rx->flags & (HTTP_GET | HTTP_HEAD | HTTP_POST) && info->valid && tx->length < 0) {
        /*
            The sendFile connector is optimized on some platforms to use the sendfile() system call.
            Set the entity length for the sendFile connector to utilize.
         */
        httpSetEntityLength(conn, tx->fileInfo.size);
    }
    return HTTP_ROUTE_OK;
}



static int openFileHandler(HttpQueue *q)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpConn    *conn;
    MprPath     *info;
    char        *date, dbuf[16];
    MprHash     *dateCache;

    conn = q->conn;
    tx = conn->tx;
    rx = conn->rx;
    info = &tx->fileInfo;

    if (conn->error) {
        return MPR_ERR_CANT_OPEN;
    }
    if (rx->flags & (HTTP_GET | HTTP_HEAD | HTTP_POST)) {
        if (!(info->valid || info->isDir)) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot find document");
            return 0;
        } 
        if (!tx->etag) {
            /* Set the etag for caching in the client */
            tx->etag = sfmt("\"%llx-%llx-%llx\"", (int64) info->inode, (int64) info->size, (int64) info->mtime);
        }
        if (info->mtime) {
            dateCache = conn->http->dateCache;
            if ((date = mprLookupKey(dateCache, itosbuf(dbuf, sizeof(dbuf), (int64) info->mtime, 10))) == 0) {
                if (!dateCache || mprGetHashLength(dateCache) > 128) {
                    conn->http->dateCache = dateCache = mprCreateHash(0, 0);
                }
                date = httpGetDateString(&tx->fileInfo);
                mprAddKey(dateCache, itosbuf(dbuf, sizeof(dbuf), (int64) info->mtime, 10), date);
            }
            httpSetHeaderString(conn, "Last-Modified", date);
        }
        if (httpContentNotModified(conn)) {
            httpSetStatus(conn, HTTP_CODE_NOT_MODIFIED);
            httpOmitBody(conn);
            tx->length = -1;
        }
        if (!tx->fileInfo.isReg && !tx->fileInfo.isLink) {
            httpTrace(conn, "request.document.error", "error", "msg:'Document is not a regular file',filename:'%s'", 
                tx->filename);
            httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot serve document");
            
        } else if (tx->fileInfo.size > conn->limits->transmissionBodySize) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                "Http transmission aborted. File size exceeds max body of %'lld bytes",
                    conn->limits->transmissionBodySize);
            
        } else if (!(tx->connector == conn->http->sendConnector)) {
            /*
                If using the net connector, open the file if a body must be sent with the response. The file will be
                automatically closed when the request completes.
             */
            if (!(tx->flags & HTTP_TX_NO_BODY)) {
                tx->file = mprOpenFile(tx->filename, O_RDONLY | O_BINARY, 0);
                if (tx->file == 0) {
                    if (rx->referrer && *rx->referrer) {
                        httpTrace(conn, "request.document.error", "error", 
                            "msg:'Cannot open document',filename:'%s',referrer:'%s'", 
                            tx->filename, rx->referrer);
                    } else {
                        httpTrace(conn, "request.document.error", "error", 
                            "msg:'Cannot open document',filename:'%s'", tx->filename);
                    }
                    httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot open document");
                }
            }
        }
    } else if (rx->flags & (HTTP_DELETE | HTTP_OPTIONS | HTTP_PUT)) {
        ;
    } else {
        httpError(conn, HTTP_CODE_BAD_METHOD, "Unsupported method");
    }
    return 0;
}


static void closeFileHandler(HttpQueue *q)
{
    HttpTx  *tx;

    tx = q->conn->tx;
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
}


static void startFileHandler(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;
    HttpPacket  *packet;

    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;
    
    if (tx->finalized || conn->error) {
        return;

    } else if (rx->flags & HTTP_PUT) {
        handlePutRequest(q);
        
    } else if (rx->flags & HTTP_DELETE) {
        handleDeleteRequest(q);
        
    } else if (rx->flags & HTTP_OPTIONS) {
        httpHandleOptions(q->conn);
        
    } else if (!(tx->flags & HTTP_TX_NO_BODY)) {
        /* Create a single data packet based on the entity length */
        packet = httpCreateEntityPacket(0, tx->entityLength, readFileData);
        if (!tx->outputRanges) {
            /* Can set a content length */
            tx->length = tx->entityLength;
        }
        /* Add to the output service queue */
        httpPutForService(q, packet, 0);
    }
}


/*
    The ready callback is invoked when all body data has been received
 */
static void readyFileHandler(HttpQueue *q)
{
    /*
        The queue already contains a single data packet representing all the output data.
     */
    httpFinalize(q->conn);
}


/*  
    Populate a packet with file data. Return the number of bytes read or a negative error code. Will not return with
    a short read.
 */
static ssize readFileData(HttpQueue *q, HttpPacket *packet, MprOff pos, ssize size)
{
    HttpConn    *conn;
    HttpTx      *tx;
    ssize       nbytes;

    conn = q->conn;
    tx = conn->tx;

    if (packet->content == 0 && (packet->content = mprCreateBuf(size, -1)) == 0) {
        return MPR_ERR_MEMORY;
    }
    assert(size <= mprGetBufSpace(packet->content));    
    if (pos >= 0) {
        mprSeekFile(tx->file, SEEK_SET, pos);
    }
    if ((nbytes = mprReadFile(tx->file, mprGetBufStart(packet->content), size)) != size) {
        /*  
            As we may have sent some data already to the client, the only thing we can do is abort and hope the client 
            notices the short data.
         */
        httpError(conn, HTTP_CODE_SERVICE_UNAVAILABLE, "Cannot read file %s", tx->filename);
        return MPR_ERR_CANT_READ;
    }
    mprAdjustBufEnd(packet->content, nbytes);
    packet->esize -= nbytes;
    assert(packet->esize == 0);
    return nbytes;
}


/*  
    Prepare a data packet for sending downstream. This involves reading file data into a suitably sized packet. Return
    the 1 if the packet was sent entirely, return zero if the packet could not be completely sent. Return a negative
    error code for write errors. This may split the packet if it exceeds the downstreams maximum packet size.
 */
static int prepPacket(HttpQueue *q, HttpPacket *packet)
{
    HttpQueue   *nextQ;
    ssize       size, nbytes;

    if (mprNeedYield()) {
        httpScheduleQueue(q);
        return 0;
    }
    nextQ = q->nextQ;
    if (packet->esize > nextQ->packetSize) {
        httpPutBackPacket(q, httpSplitPacket(packet, nextQ->packetSize));
        size = nextQ->packetSize;
    } else {
        size = (ssize) packet->esize;
    }
    if ((size + nextQ->count) > nextQ->max) {
        /*  
            The downstream queue is full, so disable the queue and service downstream queue.
            Will re-enable via a writable event on the connection.
         */
        httpSuspendQueue(q);
        if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
            httpScheduleQueue(nextQ);
        }
        return 0;
    }
    if ((nbytes = readFileData(q, packet, q->ioPos, size)) != size) {
        return MPR_ERR_CANT_READ;
    }
    q->ioPos += nbytes;
    return 1;
}


/*  
    The service callback will be invoked to service outgoing packets on the service queue. It will only be called 
    once all incoming data has been received and then when the downstream queues drain sufficiently to absorb 
    more data. This routine may flow control if the downstream stage cannot accept all the file data. It will
    then be re-called as required to send more data.
 */
static void outgoingFileService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    HttpPacket  *packet;
    bool        usingSend;
    int         rc;

    conn = q->conn;
    tx = conn->tx;
    usingSend = (tx->connector == conn->http->sendConnector);
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!usingSend && !tx->outputRanges && packet->esize) {
            if ((rc = prepPacket(q, packet)) < 0) {
                return;
            } else if (rc == 0) {
                httpPutBackPacket(q, packet);
                return;
            }
        }
        httpPutPacketToNext(q, packet);
    }
}


/*
    The incoming callback is invoked to receive body data 
 */
static void incomingFile(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpRange   *range;
    MprBuf      *buf;
    MprFile     *file;
    ssize       len;

    conn = q->conn;
    tx = conn->tx;
    rx = conn->rx;
    file = (MprFile*) q->queueData;
    
    if (file == 0) {
        /*  Not a PUT so just ignore the incoming data.  */
        return;
    }
    if (httpGetPacketLength(packet) == 0) {
        /* End of input */
        if (file) {
            mprCloseFile(file);
        }
        q->queueData = 0;
        if (!tx->etag) {
            /* Set the etag for caching in the client */
            mprGetPathInfo(tx->filename, &tx->fileInfo);
            tx->etag = sfmt("\"%llx-%llx-%llx\"", tx->fileInfo.inode, tx->fileInfo.size, tx->fileInfo.mtime);
        }
        return;
    }
    buf = packet->content;
    len = mprGetBufLength(buf);
    assert(len > 0);

    range = rx->inputRange;
    if (range && mprSeekFile(file, SEEK_SET, range->start) != range->start) {
        httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot seek to range start to %lld", range->start);

    } else if (mprWriteFile(file, mprGetBufStart(buf), len) != len) {
        httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot PUT to %s", tx->filename);
    }
}


/*  
    This is called to setup for a HTTP PUT request. It is called before receiving the post data via incomingFileData
 */
static void handlePutRequest(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    MprFile     *file;
    cchar       *path;

    assert(q->pair->queueData == 0);

    conn = q->conn;
    tx = conn->tx;
    assert(tx->filename);
    assert(tx->fileInfo.checked);

    path = tx->filename;
    if (tx->outputRanges) {
        /*  
            Open an existing file with fall-back to create
         */
        if ((file = mprOpenFile(path, O_BINARY | O_WRONLY, 0644)) == 0) {
            if ((file = mprOpenFile(path, O_CREAT | O_TRUNC | O_BINARY | O_WRONLY, 0644)) == 0) {
                httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot create the put URI");
                return;
            }
        } else {
            mprSeekFile(file, SEEK_SET, 0);
        }
    } else {
        if ((file = mprOpenFile(path, O_CREAT | O_TRUNC | O_BINARY | O_WRONLY, 0644)) == 0) {
            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot create the put URI");
            return;
        }
    }
    if (!tx->fileInfo.isReg) {
        httpSetHeaderString(conn, "Location", conn->rx->uri);
    }
    httpSetStatus(conn, tx->fileInfo.isReg ? HTTP_CODE_NO_CONTENT : HTTP_CODE_CREATED);
    q->pair->queueData = (void*) file;
}


static void handleDeleteRequest(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;
    assert(tx->filename);
    assert(tx->fileInfo.checked);

    if (!tx->fileInfo.isReg) {
        httpError(conn, HTTP_CODE_NOT_FOUND, "Document not found");
        return;
    }
    if (mprDeletePath(tx->filename) < 0) {
        httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot remove document");
        return;
    }
    httpSetStatus(conn, HTTP_CODE_NO_CONTENT);
}


static int handleDirectory(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    HttpUri     *req;
    MprPath     *info;
    cchar       *index, *pathInfo, *uri;
    char        *path;
    int         next;

    rx = conn->rx;
    tx = conn->tx;
    req = rx->parsedUri;
    route = rx->route;
    info = &tx->fileInfo;

    /*
        Manage requests for directories
     */
    if (!sends(req->path, "/")) {
        /*
           Append "/" and do an external redirect. Use the original request URI.
         */
        pathInfo = sjoin(req->path, "/", NULL);
        uri = httpFormatUri(req->scheme, req->host, req->port, pathInfo, req->reference, req->query, 0);
        httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, uri);
        return HTTP_ROUTE_OK;
    }
    if (route->indexes) {
        /*
            Ends with a "/" so do internal redirection to an index file
         */
        for (ITERATE_ITEMS(route->indexes, index, next)) {
            /*
                Internal directory redirections. Transparently append index. Test indexes in order.
             */
            path = mprJoinPath(tx->filename, index);
            if (mprPathExists(path, R_OK)) {
                pathInfo = sjoin(rx->scriptName, rx->pathInfo, index, NULL);
                uri = httpFormatUri(req->scheme, req->host, req->port, pathInfo, req->reference, req->query, 0);
                httpSetUri(conn, uri);
                tx->filename = path;
                tx->ext = httpGetExt(conn);
                mprGetPathInfo(tx->filename, info);
                return HTTP_ROUTE_REROUTE;
            }
        }
    }
#if ME_COM_DIR
    /*
        Directory Listing. If a directory, test if a directory listing should be rendered. If so, delegate to the
        dirHandler. Cannot use the sendFile handler and must use the netConnector.
     */
    if (info->isDir && httpRenderDirListing(conn)) {
        tx->handler = conn->http->dirHandler;
        tx->connector = conn->http->netConnector;
        return HTTP_ROUTE_OK;
    }
#endif
    return HTTP_ROUTE_OK;
}


/*  
    Loadable module initialization
 */
PUBLIC int httpOpenFileHandler()
{
    HttpStage     *handler;

    /* 
        This handler serves requests without using thread workers.
     */
    if ((handler = httpCreateHandler("fileHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    handler->rewrite = rewriteFileHandler;
    handler->open = openFileHandler;
    handler->close = closeFileHandler;
    handler->start = startFileHandler;
    handler->ready = readyFileHandler;
    handler->outgoingService = outgoingFileService;
    handler->incoming = incomingFile;
    HTTP->fileHandler = handler;
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/host.c ************/


/*
    host.c -- Host class for all HTTP hosts

    The Host class is used for the default HTTP server and for all virtual hosts (including SSL hosts).
    Many objects are controlled at the host level. Eg. URL handlers.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Locals ***********************************/

static HttpHost *defaultHost;

/********************************** Forwards **********************************/

static void manageHost(HttpHost *host, int flags);

/*********************************** Code *************************************/

PUBLIC HttpHost *httpCreateHost()
{
    HttpHost    *host;

    if ((host = mprAllocObj(HttpHost, manageHost)) == 0) {
        return 0;
    }
    if ((host->responseCache = mprCreateCache(MPR_CACHE_SHARED)) == 0) {
        return 0;
    }
    mprSetCacheLimits(host->responseCache, 0, ME_MAX_CACHE_DURATION, 0, 0);

    host->routes = mprCreateList(-1, MPR_LIST_STABLE);
    host->flags = HTTP_HOST_NO_TRACE;
    host->streams = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STABLE);
    httpSetStreaming(host, "application/x-www-form-urlencoded", NULL, 0);
    httpSetStreaming(host, "application/json", NULL, 0);
    httpAddHost(host);
    return host;
}


PUBLIC HttpHost *httpCloneHost(HttpHost *parent)
{
    HttpHost    *host;

    if ((host = mprAllocObj(HttpHost, manageHost)) == 0) {
        return 0;
    }
    /*
        The dirs and routes are all copy-on-write.
        Don't clone ip, port and name
     */
    host->parent = parent;
    host->responseCache = parent->responseCache;
    host->routes = parent->routes;
    host->flags = parent->flags | HTTP_HOST_VHOST;
    host->streams = parent->streams;
    host->secureEndpoint = parent->secureEndpoint;
    host->defaultEndpoint = parent->defaultEndpoint;
    httpAddHost(host);
    return host;
}


static void manageHost(HttpHost *host, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(host->name);
        mprMark(host->parent);
        mprMark(host->responseCache);
        mprMark(host->routes);
        mprMark(host->defaultRoute);
        mprMark(host->defaultEndpoint);
        mprMark(host->secureEndpoint);
        mprMark(host->streams);
    }
}


PUBLIC HttpHost *httpCreateDefaultHost() 
{
    HttpHost    *host;
    HttpRoute   *route;

    if (defaultHost) {
        return defaultHost;
    }
    defaultHost = host = httpCreateHost();
    route = httpCreateRoute(host);
    httpSetRouteName(route, "default");
    httpSetHostDefaultRoute(host, route);
    route->limits = route->http->serverLimits;
    return host;
}


PUBLIC int httpStartHost(HttpHost *host)
{
    HttpRoute   *route;
    int         next;

    for (ITERATE_ITEMS(host->routes, route, next)) {
        httpStartRoute(route);
    }
    for (ITERATE_ITEMS(host->routes, route, next)) {
        if (!route->trace && route->parent && route->parent->trace) {
            route->trace = route->parent->trace;
        }
    }
    return 0;
}


PUBLIC void httpStopHost(HttpHost *host)
{
    HttpRoute   *route;
    int         next;

    for (ITERATE_ITEMS(host->routes, route, next)) {
        httpStopRoute(route);
    }
}


PUBLIC HttpRoute *httpGetHostDefaultRoute(HttpHost *host)
{
    return host->defaultRoute;
}


static void printRoute(HttpRoute *route, int next, bool full)
{
    HttpRoute   *rp;
    HttpRouteOp *condition;
    HttpStage   *handler;
    HttpAuth    *auth;
    MprKey      *kp;
    cchar       *methods, *pattern, *target, *index;
    int         authLen, methodsLen, nameLen, patternLen, nextIndex;

    if (route->flags & HTTP_ROUTE_HIDDEN) {
        return;
    }
    if (!full) {
        if (next == 0) {
            authLen = methodsLen = nameLen = patternLen = 0;
            for (next = 0; (rp = mprGetNextItem(route->host->routes, &next)) != 0; ) {
                authLen = (int) max(authLen, rp->auth->type ? slen(rp->auth->type->name) : 0);
                nameLen = (int) max(nameLen, slen(rp->name));
                patternLen = (int) max(patternLen, slen(rp->pattern));
                methodsLen = (int) max(methodsLen, slen(httpGetRouteMethods(rp)));
            }
            printf("%-*s %-*s %-*s %-*s %-14s\n", nameLen, "Route Name", methodsLen, "Methods", 
                authLen, "Auth", patternLen, "Pattern", "Target");
        }
    }
    auth = route->auth;
    methods = httpGetRouteMethods(route);
    methods = methods ? methods : "*";
    pattern = (route->pattern && *route->pattern) ? route->pattern : "^/";
    target = (route->target && *route->target) ? route->target : "$&";
    if (full) {
        printf("\n Route [%d]. %s\n", next, route->name);
        printf("    Pattern:      %s\n", pattern);
        if (route->prefix && *route->prefix) {
            printf("    RegExp:       %s\n", route->optimizedPattern);
            printf("    Prefix:       %s\n", route->prefix);
        }
        printf("    Methods:      %s\n", methods);
        printf("    Target:       %s\n", target);
        printf("    Auth:         %s\n", auth->type ? auth->type->name : "-");
        printf("    Home:         %s\n", route->home);
        printf("    Documents:    %s\n", route->documents);
        if (route->sourceName) {
            printf("    Source:       %s\n", route->sourceName);
        }
        if (route->tplate) {
            printf("    Template:     %s\n", route->tplate);
        }
        if (route->indexes) {
            for (ITERATE_ITEMS(route->indexes, index, nextIndex)) {
                printf("    Indexes:      %s \n", index);
            }
        }
        if (route->conditions) {
            for (next = 0; (condition = mprGetNextItem(route->conditions, &next)) != 0; ) {
                printf("    Condition:    %s %s\n", condition->name, condition->details ? condition->details : "");
            }
        }
        if (route->handler) {
            printf("    Handler:      %s\n", route->handler->name);
        }
        if (route->extensions) {
            for (ITERATE_KEYS(route->extensions, kp)) {
                handler = (HttpStage*) kp->data;
                printf("    Extension:    \"%s\" => %s\n", kp->key, handler->name);
            }
        }
        if (route->handlers) {
            for (ITERATE_ITEMS(route->handlers, handler, nextIndex)) {
                printf("    Handler:      %s\n", handler->name);
            }
        }
    } else {
        printf("%-*s %-*s %-*s %-*s %-14s\n", nameLen, route->name, methodsLen, methods ? methods : "*", 
            authLen, auth->type ? auth->type->name : "none", patternLen, pattern, target);
    }
}


PUBLIC void httpLogRoutes(HttpHost *host, bool full)
{
    HttpRoute   *route;
    int         next, foundDefault;

    if (!host) {
        host = httpGetDefaultHost();
    }
    printf("\n");
    for (foundDefault = next = 0; (route = mprGetNextItem(host->routes, &next)) != 0; ) {
        printRoute(route, next - 1, full);
        if (route == host->defaultRoute) {
            foundDefault++;
        }
    }
    /*
        Add the default so LogRoutes can print the default route which has yet been added to host->routes
     */
    if (!foundDefault && host->defaultRoute) {
        printRoute(host->defaultRoute, next - 1, full);
    }
    printf("\n");
}


PUBLIC void httpSetHostName(HttpHost *host, cchar *name)
{
    if (!name || *name == '\0') {
        mprLog("error http", 0, "Host name is empty");
    }
    if (sends(name, "*")) {
        host->flags |= HTTP_HOST_WILD_STARTS;

    } else if (name && *name == '*') {
        host->flags |= HTTP_HOST_WILD_CONTAINS;
    }
    host->name = strim(name, "*", 0);
}


PUBLIC int httpAddRoute(HttpHost *host, HttpRoute *route)
{
    HttpRoute   *prev, *item, *lastRoute;
    int         i, thisRoute;

    assert(route);

    if (host->parent && host->routes == host->parent->routes) {
        host->routes = mprCloneList(host->parent->routes);
    }
    if (mprLookupItem(host->routes, route) < 0) {
        if (route->pattern[0] && (lastRoute = mprGetLastItem(host->routes)) && lastRoute->pattern[0] == '\0') {
            /* 
                Insert non-default route before last default route 
             */
            thisRoute = mprInsertItemAtPos(host->routes, mprGetListLength(host->routes) - 1, route);
        } else {
            thisRoute = mprAddItem(host->routes, route);
        }
        if (thisRoute > 0) {
            prev = mprGetItem(host->routes, thisRoute - 1);
            if (!smatch(prev->startSegment, route->startSegment)) {
                prev->nextGroup = thisRoute;
                for (i = thisRoute - 2; i >= 0; i--) {
                    item = mprGetItem(host->routes, i);
                    if (smatch(item->startSegment, prev->startSegment)) {
                        item->nextGroup = thisRoute;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    httpSetRouteHost(route, host);
    return 0;
}


PUBLIC HttpRoute *httpLookupRoute(HttpHost *host, cchar *name)
{
    HttpRoute   *route;
    int         next;

    if (name == 0 || *name == '\0') {
        name = "default";
    }
    if (!host && (host = httpGetDefaultHost()) == 0) {
        return 0;
    }
    for (next = 0; (route = mprGetNextItem(host->routes, &next)) != 0; ) {
        assert(route->name);
        if (smatch(route->name, name)) {
            return route;
        }
    }
    return 0;
}


PUBLIC HttpRoute *httpLookupRouteByPattern(HttpHost *host, cchar *pattern)
{
    HttpRoute   *route;
    int         next;

    if (smatch(pattern, "/") || smatch(pattern, "^/") || smatch(pattern, "^/$")) {
        pattern = "";
    }
    if (!host && (host = httpGetDefaultHost()) == 0) {
        return 0;
    }
    for (next = 0; (route = mprGetNextItem(host->routes, &next)) != 0; ) {
        assert(route->pattern);
        if (smatch(route->pattern, pattern)) {
            return route;
        }
    }
    return 0;
}


PUBLIC void httpResetRoutes(HttpHost *host)
{
    host->routes = mprCreateList(-1, MPR_LIST_STABLE);
}


PUBLIC void httpSetHostDefaultRoute(HttpHost *host, HttpRoute *route)
{
    host->defaultRoute = route;
}


PUBLIC void httpSetDefaultHost(HttpHost *host)
{
    defaultHost = host;
}


PUBLIC void httpSetHostSecureEndpoint(HttpHost *host, HttpEndpoint *endpoint)
{
    host->secureEndpoint = endpoint;
}


PUBLIC void httpSetHostDefaultEndpoint(HttpHost *host, HttpEndpoint *endpoint)
{
    host->defaultEndpoint = endpoint;
}


PUBLIC HttpHost *httpGetDefaultHost()
{
    return defaultHost;
}


PUBLIC HttpRoute *httpGetDefaultRoute(HttpHost *host)
{
    if (host) {
        return host->defaultRoute;
    } else if (defaultHost) {
        return defaultHost->defaultRoute;
    }
    return 0;
}


PUBLIC bool httpGetStreaming(HttpHost *host, cchar *mime, cchar *uri)
{
    MprKey      *kp;

    assert(host);
    assert(host->streams);

    if (schr(mime, ';')) {
        mime = stok(sclone(mime), ";", 0);
    }
    if ((kp = mprLookupKeyEntry(host->streams, mime)) != 0) {
        if (kp->data == NULL || sstarts(uri, kp->data)) {
            /* Type is set to the enable value */
            return kp->type;
        }
    }
    return 1;
}


PUBLIC void httpSetStreaming(HttpHost *host, cchar *mime, cchar *uri, bool enable)
{
    MprKey  *kp;

    assert(host);
    if ((kp = mprAddKey(host->streams, mime, uri)) != 0) {
        /*
            We store the enable value in the key type to save an allocation
         */
        kp->type = enable;
    }
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/monitor.c ************/


/*
    monitor.c -- Monitor and defensive management.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    A note on locking. Unlike most of appweb which effectively runs single-threaded due to the dispatcher,
    this module typically runs the httpMonitorEvent and checkMonitor routines multi-threaded.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static MprTicks lookupTicks(MprHash *args, cchar *key, MprTicks defaultValue);
static void stopMonitors();

/************************************ Code ************************************/

PUBLIC int httpAddCounter(cchar *name)
{
    return mprAddItem(HTTP->counters, sclone(name));
}


PUBLIC void httpAddCounters()
{
    Http    *http;

    http = HTTP;
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_CLIENTS, sclone("ActiveClients"));
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_CONNECTIONS, sclone("ActiveConnections"));
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_REQUESTS, sclone("ActiveRequests"));
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_PROCESSES, sclone("ActiveProcesses"));
    mprSetItem(http->counters, HTTP_COUNTER_BAD_REQUEST_ERRORS, sclone("BadRequestErrors"));
    mprSetItem(http->counters, HTTP_COUNTER_ERRORS, sclone("Errors"));
    mprSetItem(http->counters, HTTP_COUNTER_LIMIT_ERRORS, sclone("LimitErrors"));
    mprSetItem(http->counters, HTTP_COUNTER_MEMORY, sclone("Memory"));
    mprSetItem(http->counters, HTTP_COUNTER_NOT_FOUND_ERRORS, sclone("NotFoundErrors"));
    mprSetItem(http->counters, HTTP_COUNTER_NETWORK_IO, sclone("NetworkIO"));
    mprSetItem(http->counters, HTTP_COUNTER_REQUESTS, sclone("Requests"));
    mprSetItem(http->counters, HTTP_COUNTER_SSL_ERRORS, sclone("SSLErrors"));
}


static void invokeDefenses(HttpMonitor *monitor, MprHash *args)
{
    Http            *http;
    HttpDefense     *defense;
    HttpRemedyProc  remedyProc;
    MprKey          *kp;
    MprHash         *extra;
    int             next;

    http = monitor->http;
    mprHold(args);

    for (ITERATE_ITEMS(monitor->defenses, defense, next)) {
        if ((remedyProc = mprLookupKey(http->remedies, defense->remedy)) == 0) {
            continue;
        }
        extra = mprCloneHash(defense->args);
        for (ITERATE_KEYS(extra, kp)) {
            kp->data = stemplate(kp->data, args);
        }
        mprBlendHash(args, extra);

        if (defense->suppressPeriod) {
            typedef struct SuppressDefense {
                MprTicks    suppressUntil;
            } SuppressDefense;

            SuppressDefense *sd;
            cchar *str = mprHashToString(args, "");
            if (!defense->suppress) {
                defense->suppress = mprCreateHash(0, 0);
            }
            if ((sd = mprLookupKey(defense->suppress, str)) != 0) {
                if (sd->suppressUntil > http->now) {
                    continue;
                }
                sd->suppressUntil = http->now + defense->suppressPeriod;
            } else {
                if ((sd = mprAllocStruct(SuppressDefense)) != 0) {
                    mprAddKey(defense->suppress, str, sd);
                }
                sd->suppressUntil = http->now + defense->suppressPeriod;
            }
        }
        httpTrace(0, "monitor.defense.invoke", "context", "defense:'%s',remedy:'%s'", defense->name, defense->remedy);

        /*  WARNING: yields */
        remedyProc(args);

#if FUTURE
        if (http->monitorCallback) {
            (http->monitorCallback)(monitor, defense, args);
        }
#endif
    }
    mprRelease(args);
}


static void checkCounter(HttpMonitor *monitor, HttpCounter *counter, cchar *ip)
{
    MprHash     *args;
    cchar       *address, *fmt, *msg, *subject;
    uint64      period;

    fmt = 0;

    if (monitor->expr == '>') {
        if (counter->value > monitor->limit) {
            fmt = "Monitor%s for \"%s\". Value %lld per %lld secs exceeds limit of %lld.";
        }

    } else if (monitor->expr == '>') {
        if (counter->value < monitor->limit) {
            fmt = "Monitor%s for \"%s\". Value %lld per %lld secs outside limit of %lld.";
        }
    }
    if (fmt) {
        period = monitor->period / 1000;
        address = ip ? sfmt(" %s", ip) : "";
        msg = sfmt(fmt, address, monitor->counterName, counter->value, period, monitor->limit);
        httpTrace(0, "monitor.check", "context", "msg:'%s'", msg);

        subject = sfmt("Monitor %s Alert", monitor->counterName);
        args = mprDeserialize(
            sfmt("{ COUNTER: '%s', DATE: '%s', IP: '%s', LIMIT: %lld, MESSAGE: '%s', PERIOD: %lld, SUBJECT: '%s', VALUE: %lld }", 
            monitor->counterName, mprGetDate(NULL), ip, monitor->limit, msg, period, subject, counter->value));
        /*  
            WARNING: may yield depending on remedy
         */
        invokeDefenses(monitor, args);
    }
    counter->value = 0;
}


PUBLIC void httpPruneMonitors()
{
    Http        *http;
    HttpAddress *address;
    MprTicks    period;
    MprKey      *kp;

    http = HTTP;
    period = max(http->monitorMaxPeriod, 15 * MPR_TICKS_PER_SEC);
    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        if (address->banUntil && address->banUntil < http->now) {
            httpTrace(0, "monitor.ban.stop", "context", "client:'%s'", kp->key);
            address->banUntil = 0;
        }
        if ((address->updated + period) < http->now && address->banUntil == 0) {
            mprRemoveKey(http->addresses, kp->key);
            /* Safe to keep iterating after removal of key */
        }
    }
    unlock(http->addresses);
}


/*
    WARNING: this routine may yield
 */
static void checkMonitor(HttpMonitor *monitor, MprEvent *event)
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     c, *counter;
    MprKey          *kp;

    http = HTTP;
    http->now = mprGetTicks();

    if (monitor->counterIndex == HTTP_COUNTER_MEMORY) {
        memset(&c, 0, sizeof(HttpCounter));
        c.value = mprGetMem();
        checkCounter(monitor, &c, NULL);

    } else if (monitor->counterIndex == HTTP_COUNTER_ACTIVE_PROCESSES) {
        memset(&c, 0, sizeof(HttpCounter));
        c.value = mprGetListLength(MPR->cmdService->cmds);
        checkCounter(monitor, &c, NULL);

    } else if (monitor->counterIndex == HTTP_COUNTER_ACTIVE_CLIENTS) {
        memset(&c, 0, sizeof(HttpCounter));
        c.value = mprGetHashLength(http->addresses);
        checkCounter(monitor, &c, NULL);

    } else {
        /*
            Check the monitor for each active client address
         */
        lock(http->addresses);
        for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
            counter = &address->counters[monitor->counterIndex];
            unlock(http->addresses);

            /*
                WARNING: this may allow new addresses to be added or stale addresses to be removed.
                Regardless, because GC is paused, iterating is safe.
             */
            checkCounter(monitor, counter, kp->key);

            lock(http->addresses);
        }
        if (mprGetHashLength(http->addresses) == 0) {
            stopMonitors();
        }
        unlock(http->addresses);
        httpPruneMonitors();
    }
}


static int manageMonitor(HttpMonitor *monitor, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(monitor->counterName);
        mprMark(monitor->defenses);
        mprMark(monitor->timer);
    }
    return 0;
}


PUBLIC int httpAddMonitor(cchar *counterName, cchar *expr, uint64 limit, MprTicks period, cchar *defenses)
{
    Http            *http;
    HttpMonitor     *monitor, *mp;
    HttpDefense     *defense;
    MprList         *defenseList;
    cchar           *def;
    char            *tok;
    int             counterIndex, next;

    http = HTTP;
    if (period < HTTP_MONITOR_MIN_PERIOD) {
        return MPR_ERR_BAD_ARGS;
    }
    if ((counterIndex = mprLookupStringItem(http->counters, counterName)) < 0) {
        mprLog("error http monitor", 0, "Cannot find counter %s", counterName);
        return MPR_ERR_CANT_FIND;
    }
    for (ITERATE_ITEMS(http->monitors, mp, next)) {
        if (mp->counterIndex == counterIndex) {
            mprLog("error http monitor", 0, "Monitor already exists for counter %s", counterName);
            return MPR_ERR_ALREADY_EXISTS;
        }
    }
    if ((monitor = mprAllocObj(HttpMonitor, manageMonitor)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if ((defenseList = mprCreateList(-1, MPR_LIST_STABLE)) == 0) {
        return MPR_ERR_MEMORY;
    }
    tok = sclone(defenses);
    while ((def = stok(tok, " \t", &tok)) != 0) {
        if ((defense = mprLookupKey(http->defenses, def)) == 0) {
            mprLog("error http monitor", 0, "Cannot find Defense \"%s\"", def);
            return MPR_ERR_CANT_FIND;
        }
        mprAddItem(defenseList, defense);
    }
    monitor->counterIndex = counterIndex;
    monitor->counterName = mprGetItem(http->counters, monitor->counterIndex);
    monitor->expr = (expr && *expr == '<') ? '<' : '>';
    monitor->limit = limit;
    monitor->period = period;
    monitor->defenses = defenseList;
    monitor->http = http;
    http->monitorMinPeriod = min(http->monitorMinPeriod, period);
    http->monitorMaxPeriod = max(http->monitorMaxPeriod, period);
    mprAddItem(http->monitors, monitor);
    return 0;
}


static void manageAddress(HttpAddress *address, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(address->banMsg);
    }
}


static void startMonitors() 
{
    HttpMonitor     *monitor;
    Http            *http;
    int             next;

    if (mprGetDebugMode()) {
        return;
    }
    http = HTTP;
    lock(http);
    if (!http->monitorsStarted) {
        for (ITERATE_ITEMS(http->monitors, monitor, next)) {
            if (!monitor->timer) {
                monitor->timer = mprCreateTimerEvent(NULL, "monitor", monitor->period, checkMonitor, monitor, 0);
            }
        }
        http->monitorsStarted = 1;
    }
    unlock(http);
}


static void stopMonitors() 
{
    HttpMonitor     *monitor;
    Http            *http;
    int             next;

    http = HTTP;
    lock(http);
    if (http->monitorsStarted) {
        for (ITERATE_ITEMS(http->monitors, monitor, next)) {
            if (monitor->timer) {
                mprStopContinuousEvent(monitor->timer);
                monitor->timer = 0;
            }
        }
        http->monitorsStarted = 0;
    }
    unlock(http);
}


/*
    Register a monitor event
    This code is very carefully coded for maximum speed to minimize locks for keep-alive requests. 
    There are some tolerated race conditions.
 */
PUBLIC int64 httpMonitorEvent(HttpConn *conn, int counterIndex, int64 adj)
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     *counter;
    static int      seqno = 0;
    int             ncounters;

    http = conn->http;
    address = conn->address;

    if (!address) {
        lock(http->addresses);
        address = mprLookupKey(http->addresses, conn->ip);
        if (!address || address->ncounters <= counterIndex) {
            ncounters = ((counterIndex + 0xF) & ~0xF);
            if (address) {
                address = mprRealloc(address, sizeof(HttpAddress) * ncounters * sizeof(HttpCounter));
            } else {
                address = mprAllocBlock(sizeof(HttpAddress) * ncounters * sizeof(HttpCounter), MPR_ALLOC_MANAGER | MPR_ALLOC_ZERO);
                mprSetManager(address, (MprManager) manageAddress);
            }
            if (!address) {
                return 0;
            }
            address->ncounters = ncounters;
            address->seqno = ++seqno;
            mprAddKey(http->addresses, conn->ip, address);
        }
        conn->address = address;
        if (!http->monitorsStarted) {
            startMonitors();
        }
        unlock(http->addresses);
    }
    counter = &address->counters[counterIndex];
    mprAtomicAdd64((int64*) &counter->value, adj);
    /* 
        Tolerated race with "updated" and the return value 
     */
    address->updated = http->now;
    return counter->value;
}


static int manageDefense(HttpDefense *defense, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(defense->name);
        mprMark(defense->remedy);
        mprMark(defense->args);
        mprMark(defense->suppress);
    }
    return 0;
}


static HttpDefense *createDefense(cchar *name, cchar *remedy, MprHash *args)
{
    HttpDefense     *defense;

    if ((defense = mprAllocObj(HttpDefense, manageDefense)) == 0) {
        return 0;
    }
    defense->name = sclone(name);
    defense->remedy = sclone(remedy);
    defense->args = args;
    defense->suppressPeriod = lookupTicks(args, "SUPPRESS", 0);
    return defense;
}


/*
    Remedy can also be set via REMEDY= in the remedyArgs
 */
PUBLIC int httpAddDefense(cchar *name, cchar *remedy, cchar *remedyArgs)
{
    Http        *http;
    MprHash     *args;
    MprList     *list;
    char        *arg, *key, *value;
    int         next;

    assert(name && *name);

    http = HTTP;
    args = mprCreateHash(0, MPR_HASH_STABLE);
    list = stolist(remedyArgs);
    for (ITERATE_ITEMS(list, arg, next)) {
        key = stok(arg, "=", &value);
        mprAddKey(args, key, strim(value, "\"'", 0));
    }
    if (!remedy) {
        remedy = mprLookupKey(args, "REMEDY");
    }
    mprAddKey(http->defenses, name, createDefense(name, remedy, args));
    return 0;
}


PUBLIC int httpAddDefenseFromJson(cchar *name, cchar *remedy, MprJson *jargs)
{
    Http        *http;
    MprHash     *args;
    MprJson     *arg;
    int         next;

    assert(name && *name);

    http = HTTP;
    args = mprCreateHash(0, MPR_HASH_STABLE);
    for (ITERATE_JSON(jargs, arg, next)) {
        mprAddKey(args, arg->name, arg->value);
        if (smatch(arg->name, "remedy")) {
            remedy = arg->value;
        }
    }
    mprAddKey(http->defenses, name, createDefense(name, remedy, args));
    return 0;
}


PUBLIC void httpDumpCounters()
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     *counter;
    MprKey          *kp;
    cchar           *name;
    int             i;

    http = HTTP;
    mprLog(0, 0, "Monitor Counters:\n");
    mprLog(0, 0, "Memory counter     %'zd\n", mprGetMem());
    mprLog(0, 0, "Active processes   %d\n", mprGetListLength(MPR->cmdService->cmds));
    mprLog(0, 0, "Active clients     %d\n", mprGetHashLength(http->addresses));

    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        mprLog(0, 0, "Client             %s\n", kp->key);
        for (i = 0; i < address->ncounters; i++) {
            counter = &address->counters[i];
            name = mprGetItem(http->counters, i);
            if (name == NULL) {
                break;
            }
            mprLog(0, 0, "  Counter          %s = %'lld\n", name, counter->value);
        }
    }
    unlock(http->addresses);
}


/************************************ Remedies ********************************/

PUBLIC int httpBanClient(cchar *ip, MprTicks period, int status, cchar *msg)
{
    Http            *http;
    HttpAddress     *address;
    MprTicks        banUntil;

    http = HTTP;
    if ((address = mprLookupKey(http->addresses, ip)) == 0) {
        mprLog("error http monitor", 1, "Cannot find client %s to ban", ip);
        return MPR_ERR_CANT_FIND;
    }
    if (address->banUntil < http->now) {
        httpTrace(NULL, "monitor.ban.start", "error", "client:'%s',duration:%lld", ip, period / 1000);
    }
    banUntil = http->now + period;
    address->banUntil = max(banUntil, address->banUntil);
    if (msg && *msg) {
        address->banMsg = sclone(msg);
    }
    address->banStatus = status;
    return 0;
}


static MprTicks lookupTicks(MprHash *args, cchar *key, MprTicks defaultValue)
{
    cchar   *s;
    return ((s = mprLookupKey(args, key)) ? httpGetTicks(s) : defaultValue);
}


static void banRemedy(MprHash *args)
{
    MprTicks    period;
    cchar       *ip, *banStatus, *msg;
    int         status;

    if ((ip = mprLookupKey(args, "IP")) != 0) {
        period = lookupTicks(args, "PERIOD", ME_HTTP_BAN_PERIOD);
        msg = mprLookupKey(args, "MESSAGE");
        status = ((banStatus = mprLookupKey(args, "STATUS")) != 0) ? atoi(banStatus) : 0;
        httpBanClient(ip, period, status, msg);
    }
}


static void cmdRemedy(MprHash *args)
{
    MprCmd      *cmd;
    cchar       **argv;
    char        *command, *data;
    int         rc, status, argc, background;

#if DEBUG_IDE && ME_UNIX_LIKE
    unsetenv("DYLD_LIBRARY_PATH");
    unsetenv("DYLD_FRAMEWORK_PATH");
#endif
    if ((cmd = mprCreateCmd(NULL)) == 0) {
        return;
    }
    command = sclone(mprLookupKey(args, "CMD"));
    data = 0;
    if (scontains(command, "|")) {
        data = stok(command, "|", &command);
        data = stemplate(data, args);
    }
    command = strim(command, " \t", MPR_TRIM_BOTH);
    if ((background = (sends(command, "&"))) != 0) {
        command = strim(command, "&", MPR_TRIM_END);
    }
    argc = mprMakeArgv(command, &argv, 0);
    cmd->stdoutBuf = mprCreateBuf(ME_MAX_BUFFER, -1);
    cmd->stderrBuf = mprCreateBuf(ME_MAX_BUFFER, -1);

    httpTrace(0, "monitor.remedy.cmd", "context", "remedy:'%s'", command);
    if (mprStartCmd(cmd, argc, argv, NULL, MPR_CMD_DETACH | MPR_CMD_IN) < 0) {
        httpTrace(0, "monitor.rememdy.cmd.error", "error", "msg:'Cannot start command. %s", command);
        return;
    }
    if (data) {
        if (mprWriteCmdBlock(cmd, MPR_CMD_STDIN, data, -1) < 0) {
            httpTrace(0, "monitor.remedy.cmd.error", "error", "msg:'Cannot write to command. %s'", command);
            return;
        }
    }
    mprFinalizeCmd(cmd);
    if (!background) {
        rc = mprWaitForCmd(cmd, ME_HTTP_REMEDY_TIMEOUT);
        status = mprGetCmdExitStatus(cmd);
        if (rc < 0 || status != 0) {
            httpTrace(0, "monitor.remedy.cmd.error", "error", "msg:'Remedy failed. %s. %s', command: '%s'", 
                mprGetBufStart(cmd->stderrBuf), mprGetBufStart(cmd->stdoutBuf), command);
            return;
        }
        mprDestroyCmd(cmd);
    }
}


static void delayRemedy(MprHash *args)
{
    Http            *http;
    HttpAddress     *address;
    MprTicks        delayUntil;
    cchar           *ip;
    int             delay;

    http = HTTP;
    if ((ip = mprLookupKey(args, "IP")) != 0) {
        if ((address = mprLookupKey(http->addresses, ip)) != 0) {
            delayUntil = http->now + lookupTicks(args, "PERIOD", ME_HTTP_DELAY_PERIOD);
            address->delayUntil = max(delayUntil, address->delayUntil);
            delay = (int) lookupTicks(args, "DELAY", ME_HTTP_DELAY);
            address->delay = max(delay, address->delay);
            httpTrace(0, "monitor.delay.start", "context", "client:'%s',delay:%d", ip, address->delay);
        }
    }
}


static void emailRemedy(MprHash *args)
{
    if (!mprLookupKey(args, "FROM")) {
        mprAddKey(args, "FROM", "admin");
    }
    mprAddKey(args, "CMD", "To: ${TO}\nFrom: ${FROM}\nSubject: ${SUBJECT}\n${MESSAGE}\n\n| sendmail -t");
    cmdRemedy(args);
}


static void httpRemedy(MprHash *args)
{
    HttpConn    *conn;
    cchar       *uri, *msg, *method;
    char        *err;
    int         status;

    uri = mprLookupKey(args, "URI");
    if ((method = mprLookupKey(args, "METHOD")) == 0) {
        method = "POST";
    }
    msg = smatch(method, "POST") ? mprLookupKey(args, "MESSAGE") : 0;
    if ((conn = httpRequest(method, uri, msg, &err)) == 0) {
        httpTrace(0, "monitor.remedy.http.error", "error", "msg:'%s'", err);
        return;
    }
    status = httpGetStatus(conn);
    if (status != HTTP_CODE_OK) {
        httpTrace(0, "monitor.remedy.http.error", "error", "status:%d,uri:'%s'", status, uri);
    }
}

/*
    Write to the error log
 */
static void logRemedy(MprHash *args)
{
    mprLog("error http monitor", 0, "%s", (char*) mprLookupKey(args, "MESSAGE"));
}


static void restartRemedy(MprHash *args)
{
    mprLog("info http monitor", 0, "RestartRemedy: Restarting ...");
    mprRestart();
}


PUBLIC int httpAddRemedy(cchar *name, HttpRemedyProc remedy)
{
    mprAddKey(HTTP->remedies, name, remedy);
    return 0;
}


PUBLIC int httpAddRemedies()
{
    httpAddRemedy("ban", banRemedy);
    httpAddRemedy("cmd", cmdRemedy);
    httpAddRemedy("delay", delayRemedy);
    httpAddRemedy("email", emailRemedy);
    httpAddRemedy("http", httpRemedy);
    httpAddRemedy("log", logRemedy);
    httpAddRemedy("restart", restartRemedy);
    return 0;
} 


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/netConnector.c ************/


/*
    netConnector.c -- General network connector.

    The Network connector handles output data (only) from upstream handlers and filters. It uses vectored writes to
    aggregate output packets into fewer actual I/O requests to the O/S.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/**************************** Forward Declarations ****************************/

static void addPacketForNet(HttpQueue *q, HttpPacket *packet);
static void adjustNetVec(HttpQueue *q, ssize written);
static MprOff buildNetVec(HttpQueue *q);
static void freeNetPackets(HttpQueue *q, ssize written);
static void netClose(HttpQueue *q);
static void netOutgoingService(HttpQueue *q);

/*********************************** Code *************************************/
/*
    Initialize the net connector
 */
PUBLIC int httpOpenNetConnector()
{
    HttpStage     *stage;

    if ((stage = httpCreateConnector("netConnector", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->close = netClose;
    stage->outgoingService = netOutgoingService;
    HTTP->netConnector = stage;
    return 0;
}


static void netClose(HttpQueue *q)
{
    HttpTx      *tx;

    tx = q->conn->tx;
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
}


static void netOutgoingService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    ssize       written;
    int         errCode;

    conn = q->conn;
    tx = conn->tx;
    conn->lastActivity = conn->http->now;

    if (tx->finalizedConnector) {
        return;
    }
    if (tx->flags & HTTP_TX_NO_BODY) {
        httpDiscardQueueData(q, 1);
    }
    if ((tx->bytesWritten + q->count) > conn->limits->transmissionBodySize) {
        httpLimitError(conn, HTTP_CODE_REQUEST_TOO_LARGE | ((tx->bytesWritten) ? HTTP_ABORT : 0),
            "Http transmission aborted. Exceeded transmission max body of %'lld bytes", conn->limits->transmissionBodySize);
        if (tx->bytesWritten) {
            httpFinalizeConnector(conn);
            return;
        }
    }
#if !ME_ROM
    if (tx->flags & HTTP_TX_SENDFILE) {
        /* Relay via the send connector */
        if (tx->file == 0) {
            if (tx->flags & HTTP_TX_HEADERS_CREATED) {
                tx->flags &= ~HTTP_TX_SENDFILE;
            } else {
                tx->connector = conn->http->sendConnector;
                httpSendOpen(q);
            }
        }
        if (tx->file) {
            httpSendOutgoingService(q);
            return;
        }
    }
#endif
    tx->writeBlocked = 0;

    while (q->first || q->ioIndex) {
        if (q->ioIndex == 0 && buildNetVec(q) <= 0) {
            break;
        }
        /*
            Issue a single I/O request to write all the blocks in the I/O vector
         */
        assert(q->ioIndex > 0);
        written = mprWriteSocketVector(conn->sock, q->iovec, q->ioIndex);
        if (written < 0) {
            errCode = mprGetError();
            if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
                /*  Socket full, wait for an I/O event */
                tx->writeBlocked = 1;
                break;
            }
            if (errCode == EPROTO && conn->secure) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR,
                    "Cannot negotiate SSL with server: %s", conn->sock->errorMsg);
            } else if (errCode != EPIPE && errCode != ECONNRESET && errCode != ECONNABORTED && errCode != ENOTCONN) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "netConnector: Cannot write. errno %d", errCode);
            } else {
                httpDisconnect(conn);
            }
            httpFinalizeConnector(conn);
            httpTrace(conn, "connection.io.error", "error", "msg:'Connector write error', errno: %d", errCode);
            break;

        } else if (written > 0) {
            tx->bytesWritten += written;
            freeNetPackets(q, written);
            adjustNetVec(q, written);

        } else {
            break;
        }
    }
    if (q->first && q->first->flags & HTTP_PACKET_END) {
        httpFinalizeConnector(conn);
    }
}


/*
    Build the IO vector. Return the count of bytes to be written. Return -1 for EOF.
 */
static MprOff buildNetVec(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    HttpPacket  *packet, *prev;

    conn = q->conn;
    tx = conn->tx;

    /*
        Examine each packet and accumulate as many packets into the I/O vector as possible. Leave the packets on the queue
        for now, they are removed after the IO is complete for the entire packet.
     */
    for (packet = prev = q->first; packet && !(packet->flags & HTTP_PACKET_END); packet = packet->next) {
        if (packet->flags & HTTP_PACKET_HEADER) {
            if (tx->chunkSize <= 0 && q->count > 0 && tx->length < 0) {
                /* Incase no chunking filter and we've not seen all the data yet */
                conn->keepAliveCount = 0;
            }
            httpWriteHeaders(q, packet);
        }
        if (q->ioIndex >= (ME_MAX_IOVEC - 2)) {
            break;
        }
        if (httpGetPacketLength(packet) > 0 || packet->prefix) {
            addPacketForNet(q, packet);
        } else {
            /* Remove empty packets */
            prev->next = packet->next;
            continue;
        }
        prev = packet;
    }
    return q->ioCount;
}


/*
    Add one entry to the io vector
 */
static void addToNetVector(HttpQueue *q, char *ptr, ssize bytes)
{
    assert(bytes > 0);

    q->iovec[q->ioIndex].start = ptr;
    q->iovec[q->ioIndex].len = bytes;
    q->ioCount += bytes;
    q->ioIndex++;
}


/*
    Add a packet to the io vector. Return the number of bytes added to the vector.
 */
static void addPacketForNet(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;

    conn = q->conn;
    assert(q->count >= 0);
    assert(q->ioIndex < (ME_MAX_IOVEC - 2));

    if (packet->prefix) {
        addToNetVector(q, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix));
    }
    if (httpGetPacketLength(packet) > 0) {
        addToNetVector(q, mprGetBufStart(packet->content), mprGetBufLength(packet->content));
    }
    if (httpTracing(conn) && packet->flags & HTTP_PACKET_DATA) {
        httpTraceBody(conn, 1, packet, -1);
    }
}


static void freeNetPackets(HttpQueue *q, ssize bytes)
{
    HttpPacket  *packet;
    ssize       len;

    assert(q->count >= 0);
    assert(bytes > 0);

    /*
        Loop while data to be accounted for and we have not hit the end of data packet.
        Chunks will have the chunk header in the packet->prefix.
        The final chunk trailer will be in a packet->prefix with no other data content.
        Must leave this routine with the end packet still on the queue and all bytes accounted for.
     */
    while ((packet = q->first) != 0 && !(packet->flags & HTTP_PACKET_END) && bytes > 0) {
        if (packet->prefix) {
            len = mprGetBufLength(packet->prefix);
            len = min(len, bytes);
            mprAdjustBufStart(packet->prefix, len);
            bytes -= len;
            /* Prefixes don't count in the q->count. No need to adjust */
            if (mprGetBufLength(packet->prefix) == 0) {
                packet->prefix = 0;
            }
        }
        if (packet->content) {
            len = mprGetBufLength(packet->content);
            len = min(len, bytes);
            mprAdjustBufStart(packet->content, len);
            bytes -= len;
            q->count -= len;
            assert(q->count >= 0);
        }
        if (httpGetPacketLength(packet) == 0) {
            /* Done with this packet - consume it */
            assert(!(packet->flags & HTTP_PACKET_END));
            httpGetPacket(q);
        } else {
            break;
        }
    }
    assert(bytes == 0);
}


/*
    Clear entries from the IO vector that have actually been transmitted. Support partial writes.
 */
static void adjustNetVec(HttpQueue *q, ssize written)
{
    MprIOVec    *iovec;
    ssize       len;
    int         i, j;

    /*
        Cleanup the IO vector
     */
    if (written == q->ioCount) {
        /*
            Entire vector written. Just reset.
         */
        q->ioIndex = 0;
        q->ioCount = 0;

    } else {
        /*
            Partial write of an vector entry. Need to copy down the unwritten vector entries.
         */
        q->ioCount -= written;
        assert(q->ioCount >= 0);
        iovec = q->iovec;
        for (i = 0; i < q->ioIndex; i++) {
            len = iovec[i].len;
            if (written < len) {
                iovec[i].start += written;
                iovec[i].len -= written;
                break;
            } else {
                written -= len;
            }
        }
        /*
            Compact
         */
        for (j = 0; i < q->ioIndex; ) {
            iovec[j++] = iovec[i++];
        }
        q->ioIndex = j;
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/packet.c ************/


/*
    packet.c -- Queue support routines. Queues are the bi-directional data flow channels for the pipeline.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void managePacket(HttpPacket *packet, int flags);

/************************************ Code ************************************/
/*
    Create a new packet. If size is -1, then also create a default growable buffer -- 
    used for incoming body content. If size > 0, then create a non-growable buffer 
    of the requested size.
 */
PUBLIC HttpPacket *httpCreatePacket(ssize size)
{
    HttpPacket  *packet;

    if ((packet = mprAllocObj(HttpPacket, managePacket)) == 0) {
        return 0;
    }
    if (size != 0) {
        if ((packet->content = mprCreateBuf(size < 0 ? ME_MAX_BUFFER: (ssize) size, -1)) == 0) {
            return 0;
        }
    }
    return packet;
}


static void managePacket(HttpPacket *packet, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(packet->prefix);
        mprMark(packet->content);
        /* Don't mark next packet, list owner will mark */
    }
}


PUBLIC HttpPacket *httpCreateDataPacket(ssize size)
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(size)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_DATA;
    return packet;
}


PUBLIC HttpPacket *httpCreateEntityPacket(MprOff pos, MprOff size, HttpFillProc fill)
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(0)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_DATA;
    packet->epos = pos;
    packet->esize = size;
    packet->fill = fill;
    return packet;
}


PUBLIC HttpPacket *httpCreateEndPacket()
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(0)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_END;
    return packet;
}


PUBLIC HttpPacket *httpCreateHeaderPacket()
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(ME_MAX_BUFFER)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_HEADER;
    return packet;
}


PUBLIC HttpPacket *httpClonePacket(HttpPacket *orig)
{
    HttpPacket  *packet;

    if ((packet = httpCreatePacket(0)) == 0) {
        return 0;
    }
    if (orig->content) {
        packet->content = mprCloneBuf(orig->content);
    }
    if (orig->prefix) {
        packet->prefix = mprCloneBuf(orig->prefix);
    }
    packet->flags = orig->flags;
    packet->type = orig->type;
    packet->last = orig->last;
    packet->esize = orig->esize;
    packet->epos = orig->epos;
    packet->fill = orig->fill;
    return packet;
}


PUBLIC void httpAdjustPacketStart(HttpPacket *packet, MprOff size)
{
    if (packet->esize) {
        packet->epos += size;
        packet->esize -= size;
    } else if (packet->content) {
        mprAdjustBufStart(packet->content, (ssize) size);
    }
}


PUBLIC void httpAdjustPacketEnd(HttpPacket *packet, MprOff size)
{
    if (packet->esize) {
        packet->esize += size;
    } else if (packet->content) {
        mprAdjustBufEnd(packet->content, (ssize) size);
    }
}


PUBLIC HttpPacket *httpGetPacket(HttpQueue *q)
{
    HttpQueue     *prev;
    HttpPacket    *packet;

    while (q->first) {
        if ((packet = q->first) != 0) {
            q->first = packet->next;
            packet->next = 0;
            q->count -= httpGetPacketLength(packet);
            assert(q->count >= 0);
            if (packet == q->last) {
                q->last = 0;
                assert(q->first == 0);
            }
            if (q->first == 0) {
                assert(q->last == 0);
            }
        }
        if (q->count < q->low) {
            prev = httpFindPreviousQueue(q);
            if (prev && prev->flags & HTTP_QUEUE_SUSPENDED) {
                /*
                    This queue was full and now is below the low water mark. Back-enable the previous queue.
                 */
                httpResumeQueue(prev);
            }
        }
        return packet;
    }
    return 0;
}


PUBLIC char *httpGetPacketStart(HttpPacket *packet)
{
    if (!packet && !packet->content) {
        return 0;
    }
    return mprGetBufStart(packet->content);
}


PUBLIC char *httpGetPacketString(HttpPacket *packet)
{
    if (!packet && !packet->content) {
        return 0;
    }
    mprAddNullToBuf(packet->content);
    return mprGetBufStart(packet->content);
}


/*
    Test if the packet is too too large to be accepted by the downstream queue.
 */
PUBLIC bool httpIsPacketTooBig(HttpQueue *q, HttpPacket *packet)
{
    ssize   size;

    size = mprGetBufLength(packet->content);
    return size > q->max || size > q->packetSize;
}


/*
    Join a packet onto the service queue. This joins packet content data.
 */
PUBLIC void httpJoinPacketForService(HttpQueue *q, HttpPacket *packet, bool serviceQ)
{
    if (q->first == 0) {
        /*  Just use the service queue as a holding queue while we aggregate the post data.  */
        httpPutForService(q, packet, HTTP_DELAY_SERVICE);

    } else {
        /* Skip over the header packet */
        if (q->first && q->first->flags & HTTP_PACKET_HEADER) {
            packet = q->first->next;
            q->first = packet;
        } else {
            /* Aggregate all data into one packet and free the packet.  */
            httpJoinPacket(q->first, packet);
        }
        q->count += httpGetPacketLength(packet);
    }
    if (serviceQ && !(q->flags & HTTP_QUEUE_SUSPENDED))  {
        httpScheduleQueue(q);
    }
}


/*
    Join two packets by pulling the content from the second into the first.
    WARNING: this will not update the queue count. Assumes the either both are on the queue or neither. 
 */
PUBLIC int httpJoinPacket(HttpPacket *packet, HttpPacket *p)
{
    ssize   len;

    assert(packet->esize == 0);
    assert(p->esize == 0);
    assert(!(packet->flags & HTTP_PACKET_SOLO));
    assert(!(p->flags & HTTP_PACKET_SOLO));

    len = httpGetPacketLength(p);
    if (mprPutBlockToBuf(packet->content, mprGetBufStart(p->content), len) != len) {
        assert(0);
        return MPR_ERR_MEMORY;
    }
    return 0;
}


/*
    Join queue packets. Packets will not be split so the maximum size is advisory and may be exceeded.
    NOTE: this will not update the queue count.
 */
PUBLIC void httpJoinPackets(HttpQueue *q, ssize size)
{
    HttpPacket  *packet, *p;
    ssize       count, len;

    if (size < 0) {
        size = MAXINT;
    }
    if (q->first && q->first->next) {
        /*
            Get total length of data and create one packet for all the data, up to the size max
         */
        count = 0;
        for (p = q->first; p; p = p->next) {
            if (!(p->flags & HTTP_PACKET_HEADER)) {
                count += httpGetPacketLength(p);
            }
        }
        size = min(count, size);
        if ((packet = httpCreateDataPacket(size)) == 0) {
            return;
        }
        /*
            Insert the new packet as the first data packet
         */
        if (q->first->flags & HTTP_PACKET_HEADER) {
            /* Step over a header packet */
            packet->next = q->first->next;
            q->first->next = packet;
        } else {
            packet->next = q->first;
            q->first = packet;
        }
        /*
            Copy the data and free all other packets
         */
        for (p = packet->next; p && size > 0; p = p->next) {
            if (p->content == 0 || (len = httpGetPacketLength(p)) == 0) {
                break;
            }
            httpJoinPacket(packet, p);
            /* Unlink the packet */
            packet->next = p->next;
            if (q->last == p) {
                q->last = packet;
            }
            size -= len;
        }
    }
}


PUBLIC void httpPutPacket(HttpQueue *q, HttpPacket *packet)
{
    assert(packet);
    assert(q->put);

    q->put(q, packet);
}


/*
    Pass to the next stage in the pipeline
 */
PUBLIC void httpPutPacketToNext(HttpQueue *q, HttpPacket *packet)
{
    assert(packet);
    assert(q->nextQ->put);

    q->nextQ->put(q->nextQ, packet);
}


PUBLIC void httpPutPackets(HttpQueue *q)
{
    HttpPacket    *packet;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        httpPutPacketToNext(q, packet);
    }
}


PUBLIC bool httpNextQueueFull(HttpQueue *q)
{
    HttpQueue   *nextQ;

    nextQ = q->nextQ;
    return (nextQ && nextQ->count > nextQ->max) ? 1 : 0;
}


/*
    Put the packet back at the front of the queue
 */
PUBLIC void httpPutBackPacket(HttpQueue *q, HttpPacket *packet)
{
    assert(packet);
    assert(packet->next == 0);
    assert(q->count >= 0);

    if (packet) {
        packet->next = q->first;
        if (q->first == 0) {
            q->last = packet;
        }
        q->first = packet;
        q->count += httpGetPacketLength(packet);
    }
}


/*
    Put a packet on the service queue.
 */
PUBLIC void httpPutForService(HttpQueue *q, HttpPacket *packet, bool serviceQ)
{
    assert(packet);

    q->count += httpGetPacketLength(packet);
    packet->next = 0;

    if (q->first) {
        q->last->next = packet;
        q->last = packet;
    } else {
        q->first = packet;
        q->last = packet;
    }
    if (serviceQ && !(q->flags & HTTP_QUEUE_SUSPENDED))  {
        httpScheduleQueue(q);
    }
}


/*
    Resize and possibly split a packet so it fits in the downstream queue. Put back the 2nd portion of the split packet 
    on the queue. Ensure that the packet is not larger than "size" if it is greater than zero. If size < 0, then
    use the default packet size. Return the tail packet.
 */
PUBLIC HttpPacket *httpResizePacket(HttpQueue *q, HttpPacket *packet, ssize size)
{
    HttpPacket  *tail;
    ssize       len;

    if (size <= 0) {
        size = MAXINT;
    }
    if (packet->esize > size) {
        if ((tail = httpSplitPacket(packet, size)) == 0) {
            return 0;
        }
    } else {
        /*
            Calculate the size that will fit downstream
         */
        len = packet->content ? httpGetPacketLength(packet) : 0;
        size = min(size, len);
        size = min(size, q->nextQ->packetSize);
        if (size == 0 || size == len) {
            return 0;
        }
        if ((tail = httpSplitPacket(packet, size)) == 0) {
            return 0;
        }
    }
    httpPutBackPacket(q, tail);
    return tail;
}


/*
    Split a packet at a given offset and return the tail packet containing the data after the offset.
    The prefix data remains with the original packet. 
 */
PUBLIC HttpPacket *httpSplitPacket(HttpPacket *orig, ssize offset)
{
    HttpPacket  *tail;
    ssize       count, size;

    /* Must not be in a queue */
    assert(orig->next == 0);

    if (orig->esize) {
        if (offset >= orig->esize) {
            return 0;
        }
        if ((tail = httpCreateEntityPacket(orig->epos + offset, orig->esize - offset, orig->fill)) == 0) {
            return 0;
        }
        orig->esize = offset;

    } else {
        if (offset >= httpGetPacketLength(orig)) {
            return 0;
        }
        if (offset < (httpGetPacketLength(orig) / 2)) {
            /*
                A large packet will often be resized by splitting into chunks that the downstream queues will accept. 
                To optimize, we allocate a new packet content buffer and the tail packet keeps the trimmed 
                original packet buffer.
             */
            if ((tail = httpCreateDataPacket(0)) == 0) {
                return 0;
            }
            tail->content = orig->content;
            if ((orig->content = mprCreateBuf(offset, 0)) == 0) {
                return 0;
            }
            if (mprPutBlockToBuf(orig->content, mprGetBufStart(tail->content), offset) != offset) {
                return 0;
            }
            mprAdjustBufStart(tail->content, offset);

        } else {
            count = httpGetPacketLength(orig) - offset;
            size = max(count, ME_MAX_BUFFER);
            size = HTTP_PACKET_ALIGN(size);
            if ((tail = httpCreateDataPacket(size)) == 0) {
                return 0;
            }
            httpAdjustPacketEnd(orig, -count);
            if (mprPutBlockToBuf(tail->content, mprGetBufEnd(orig->content), count) != count) {
                return 0;
            }
        }
    }
    tail->flags = orig->flags;
    tail->type = orig->type;
    tail->last = orig->last;
    return tail;
}


bool httpIsLastPacket(HttpPacket *packet) 
{
    return packet->last;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/pam.c ************/


/*
    authPam.c - Authorization using PAM (Pluggable Authorization Module)

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_COMPILER_HAS_PAM && ME_HTTP_PAM
 #include    <security/pam_appl.h>

/********************************* Defines ************************************/

typedef struct {
    char    *name;
    char    *password;
} UserInfo;

#if MACOSX
    typedef int Gid;
#else
    typedef gid_t Gid;
#endif

/********************************* Forwards ***********************************/

static int pamChat(int msgCount, const struct pam_message **msg, struct pam_response **resp, void *data);

/*********************************** Code *************************************/
/*
    Use PAM to verify a user.  The password may be NULL if using auto-login.
 */
PUBLIC bool httpPamVerifyUser(HttpConn *conn, cchar *username, cchar *password)
{
    MprBuf              *abilities;
    pam_handle_t        *pamh;
    UserInfo            info;
    struct pam_conv     conv = { pamChat, &info };
    struct group        *gp;
    int                 res, i;

    assert(username);
    assert(!conn->encoded);

    info.name = (char*) username;

    if (password) {
        info.password = (char*) password;
        pamh = NULL;
        if ((res = pam_start("login", info.name, &conv, &pamh)) != PAM_SUCCESS) {
            return 0;
        }
        if ((res = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK)) != PAM_SUCCESS) {
            pam_end(pamh, PAM_SUCCESS);
            mprDebug("http pam", 5, "httpPamVerifyUser failed to verify %s", username);
            return 0;
        }
        pam_end(pamh, PAM_SUCCESS);
    }
    mprDebug("http pam", 5, "httpPamVerifyUser verified %s", username);

    if (!conn->user) {
        conn->user = mprLookupKey(conn->rx->route->auth->userCache, username);
    }
    if (!conn->user) {
        /* 
            Create a temporary user with a abilities set to the groups 
         */
        Gid     groups[32];
        int     ngroups;
        ngroups = sizeof(groups) / sizeof(Gid);
        if ((i = getgrouplist(username, 99999, groups, &ngroups)) >= 0) {
            abilities = mprCreateBuf(0, 0);
            for (i = 0; i < ngroups; i++) {
                if ((gp = getgrgid(groups[i])) != 0) {
                    mprPutToBuf(abilities, "%s ", gp->gr_name);
                }
            }
#if ME_DEBUG
            mprAddNullToBuf(abilities);
            mprDebug("http pam", 5, "Create temp user \"%s\" with abilities: %s", username, mprGetBufStart(abilities));
#endif
            /*
                Create a user and map groups to roles and expand to abilities
             */
            conn->user = httpAddUser(conn->rx->route->auth, username, 0, mprGetBufStart(abilities));
        }
    }
    return 1;
}

/*
    Callback invoked by the pam_authenticate function
 */
static int pamChat(int msgCount, const struct pam_message **msg, struct pam_response **resp, void *data) 
{
    UserInfo                *info;
    struct pam_response     *reply;
    int                     i;

    i = 0;
    reply = 0;
    info = (UserInfo*) data;

    if (resp == 0 || msg == 0 || info == 0) {
        return PAM_CONV_ERR;
    }
    if ((reply = calloc(msgCount, sizeof(struct pam_response))) == 0) {
        return PAM_CONV_ERR;
    }
    for (i = 0; i < msgCount; i++) {
        reply[i].resp_retcode = 0;
        reply[i].resp = 0;

        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_ON:
            reply[i].resp = strdup(info->name);
            break;

        case PAM_PROMPT_ECHO_OFF:
            /* Retrieve the user password and pass onto pam */
            reply[i].resp = strdup(info->password);
            break;

        default:
            free(reply);
            return PAM_CONV_ERR;
        }
    }
    *resp = reply;
    return PAM_SUCCESS;
}
#endif /* ME_COMPILER_HAS_PAM */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/passHandler.c ************/


/*
    passHandler.c -- Pass through handler

    This handler simply relays all content to a network connector. It is used for the ErrorHandler and 
    when there is no handler defined. It is configured as the "passHandler" and "errorHandler".
    It also handles OPTIONS and TRACE methods for all.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



static void handleTrace(HttpConn *conn);

/*********************************** Code *************************************/

static void startPass(HttpQueue *q)
{
    if (q->conn->rx->flags & HTTP_TRACE) {
        handleTrace(q->conn);
    }
}


static void readyPass(HttpQueue *q)
{
    httpFinalizeOutput(q->conn);
}


static void readyError(HttpQueue *q)
{
    if (!q->conn->error) {
        httpError(q->conn, HTTP_CODE_NOT_FOUND, "The requested resource is not available");
    }
    httpFinalizeOutput(q->conn);
}


PUBLIC void httpHandleOptions(HttpConn *conn)
{
    httpSetHeaderString(conn, "Allow", httpGetRouteMethods(conn->rx->route));
    httpFinalizeOutput(conn);
}


static void handleTrace(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q;
    HttpPacket  *traceData, *headers;

    /*
        Create a dummy set of headers to use as the response body. Then reset so the connector will create 
        the headers in the normal fashion. Need to be careful not to have a content length in the headers in the body.
     */
    tx = conn->tx;
    q = conn->writeq;
    headers = q->first;
    tx->flags |= HTTP_TX_NO_LENGTH;
    httpWriteHeaders(q, headers);
    httpDiscardData(conn, HTTP_QUEUE_TX);
    traceData = httpCreateDataPacket(httpGetPacketLength(headers) + 128);
    tx->flags &= ~(HTTP_TX_NO_LENGTH | HTTP_TX_HEADERS_CREATED);
    q->count -= httpGetPacketLength(headers);
    assert(q->count == 0);
    mprFlushBuf(headers->content);
    mprPutStringToBuf(traceData->content, mprGetBufStart(q->first->content));
    httpSetContentType(conn, "message/http");
    httpPutForService(q, traceData, HTTP_DELAY_SERVICE);
    httpFinalize(conn);
}


PUBLIC int httpOpenPassHandler()
{
    HttpStage     *stage;

    if ((stage = httpCreateHandler("passHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->passHandler = stage;
    stage->start = startPass;
    stage->ready = readyPass;

    /*
        PassHandler is an alias as the ErrorHandler too
     */
    if ((stage = httpCreateHandler("errorHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->start = startPass;
    stage->ready = readyError;
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/pipeline.c ************/


/*
    pipeline.c -- HTTP pipeline processing. 
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forward ***********************************/

static bool matchFilter(HttpConn *conn, HttpStage *filter, HttpRoute *route, int dir);
static void openQueues(HttpConn *conn);
static void pairQueues(HttpConn *conn);
static void httpStartHandler(HttpConn *conn);

/*********************************** Code *************************************/
/*
    Called after routing the request (httpRouteRequest)
 */
PUBLIC void httpCreatePipeline(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;

    if (httpServerConn(conn)) {
        assert(rx->route);
        httpCreateRxPipeline(conn, rx->route);
        httpCreateTxPipeline(conn, rx->route);
    }
}


PUBLIC void httpCreateTxPipeline(HttpConn *conn, HttpRoute *route)
{
    Http        *http;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next, hasOutputFilters;

    assert(conn);
    assert(route);

    http = conn->http;
    rx = conn->rx;
    tx = conn->tx;

    tx->outputPipeline = mprCreateList(-1, MPR_LIST_STABLE);
    if (httpServerConn(conn)) {
        if (tx->handler == 0 || tx->finalized) {
            tx->handler = http->passHandler;
        }
        mprAddItem(tx->outputPipeline, tx->handler);
    }
    hasOutputFilters = 0;
    if (route->outputStages) {
        for (next = 0; (filter = mprGetNextItem(route->outputStages, &next)) != 0; ) {
            if (matchFilter(conn, filter, route, HTTP_STAGE_TX) == HTTP_ROUTE_OK) {
                mprAddItem(tx->outputPipeline, filter);
                hasOutputFilters = 1;
            }
        }
    }
    if (tx->connector == 0) {
#if !ME_ROM
        if (tx->handler == http->fileHandler && (rx->flags & HTTP_GET) && !hasOutputFilters && !conn->secure && 
                !httpTracing(conn)) {
            tx->connector = http->sendConnector;
        } else 
#endif
        tx->connector = (route && route->connector) ? route->connector : http->netConnector;
    }
    mprAddItem(tx->outputPipeline, tx->connector);

    /*  Create the outgoing queue heads and open the queues */
    q = tx->queue[HTTP_QUEUE_TX];
    for (next = 0; (stage = mprGetNextItem(tx->outputPipeline, &next)) != 0; ) {
        q = httpCreateQueue(conn, stage, HTTP_QUEUE_TX, q);
    }
    conn->connectorq = tx->queue[HTTP_QUEUE_TX]->prevQ;

    /*
        Double the connector max hi-water mark. This optimization permits connectors to accept packets without 
        unnecesary flow control.
     */
    conn->connectorq->max *= 2;

    pairQueues(conn);

    /*
        Put the header before opening the queues incase an open routine actually services and completes the request
     */
    httpPutForService(conn->writeq, httpCreateHeaderPacket(), HTTP_DELAY_SERVICE);

    /*
        Open the pipeline stages. This calls the open entrypoints on all stages.
     */
    openQueues(conn);

    if (conn->error) {
        if (tx->handler != http->passHandler) {
            tx->handler = http->passHandler;
            httpAssignQueue(conn->writeq, tx->handler, HTTP_QUEUE_TX);
        }
    }
    tx->flags |= HTTP_TX_PIPELINE;

    if (conn->endpoint) {
        httpTrace(conn, "request.pipeline", "context",  
            "route:'%s',handler:'%s',target:'%s',endpoint:'%s:%d',host:'%s',referrer:'%s',filename:'%s'",
            rx->route->name, tx->handler->name, rx->route->targetRule, conn->endpoint->ip, conn->endpoint->port,
            conn->host->name ? conn->host->name : "default", rx->referrer ? rx->referrer : "", 
            tx->filename ? tx->filename : "");
    }
}


PUBLIC void httpCreateRxPipeline(HttpConn *conn, HttpRoute *route)
{
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next;

    assert(conn);
    assert(route);

    rx = conn->rx;
    tx = conn->tx;
    rx->inputPipeline = mprCreateList(-1, MPR_LIST_STABLE);
    if (route) {
        for (next = 0; (filter = mprGetNextItem(route->inputStages, &next)) != 0; ) {
            if (matchFilter(conn, filter, route, HTTP_STAGE_RX) == HTTP_ROUTE_OK) {
                mprAddItem(rx->inputPipeline, filter);
            }
        }
    }
    mprAddItem(rx->inputPipeline, tx->handler ? tx->handler : conn->http->clientHandler);
    /*  Create the incoming queue heads and open the queues.  */
    q = tx->queue[HTTP_QUEUE_RX];
    for (next = 0; (stage = mprGetNextItem(rx->inputPipeline, &next)) != 0; ) {
        q = httpCreateQueue(conn, stage, HTTP_QUEUE_RX, q);
    }
    if (httpClientConn(conn)) {
        pairQueues(conn);
        openQueues(conn);
    }
}


static void pairQueues(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead, *rq, *rqhead;

    tx = conn->tx;
    qhead = tx->queue[HTTP_QUEUE_TX];
    rqhead = tx->queue[HTTP_QUEUE_RX];
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->pair == 0) {
            for (rq = rqhead->nextQ; rq != rqhead; rq = rq->nextQ) {
                if (q->stage == rq->stage) {
                    q->pair = rq;
                    rq->pair = q;
                }
            }
        }
    }
}


static int openQueue(HttpQueue *q, ssize chunkSize)
{
    Http        *http;
    HttpConn    *conn;
    HttpStage   *stage;
    MprModule   *module;

    stage = q->stage;
    conn = q->conn;
    http = q->conn->http;

    if (chunkSize > 0) {
        q->packetSize = min(q->packetSize, chunkSize);
    }
    if (stage->flags & HTTP_STAGE_UNLOADED && stage->module) {
        module = stage->module;
        module = mprCreateModule(module->name, module->path, module->entry, http);
        if (mprLoadModule(module) < 0) {
            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot load module %s", module->name);
            return MPR_ERR_CANT_READ;
        }
        stage->module = module;
    }
    if (stage->module) {
        stage->module->lastActivity = http->now;
    }
    return 0;
}


static void openQueues(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;
    int         i;

    tx = conn->tx;
    for (i = 0; i < HTTP_MAX_QUEUE; i++) {
        qhead = tx->queue[i];
        for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
            if (q->open && !(q->flags & (HTTP_QUEUE_OPEN_TRIED))) {
                if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_OPEN_TRIED)) {
                    openQueue(q, tx->chunkSize);
                    if (q->open) {
                        q->flags |= HTTP_QUEUE_OPEN_TRIED;
                        if (q->stage->open(q) == 0) {
                            q->flags |= HTTP_QUEUE_OPENED;
                        } else {
                            if (!conn->error) {
                                httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot open stage %s", q->stage->name);
                            }

                        }
                    }
                }
            }
        }
    }
}


PUBLIC void httpSetSendConnector(HttpConn *conn, cchar *path)
{
#if !ME_ROM
    HttpTx      *tx;

    tx = conn->tx;
    tx->flags |= HTTP_TX_SENDFILE;
    tx->filename = sclone(path);
#else
    mprLog("error http config", 0, "Send connector not available if ROMFS enabled");
#endif
}


PUBLIC void httpClosePipeline(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;
    int         i;

    tx = conn->tx;
    if (tx) {
        for (i = 0; i < HTTP_MAX_QUEUE; i++) {
            qhead = tx->queue[i];
            for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
                if (q->close && q->flags & HTTP_QUEUE_OPENED) {
                    q->flags &= ~HTTP_QUEUE_OPENED;
                    q->stage->close(q);
                }
            }
        }
    }
}


PUBLIC void httpStartPipeline(HttpConn *conn)
{
    HttpQueue   *qhead, *q, *prevQ, *nextQ;
    HttpTx      *tx;
    HttpRx      *rx;

    tx = conn->tx;
    rx = conn->rx;
    assert(conn->endpoint);

    if (rx->needInputPipeline) {
        qhead = tx->queue[HTTP_QUEUE_RX];
        for (q = qhead->nextQ; q->nextQ != qhead; q = nextQ) {
            nextQ = q->nextQ;
            if (q->start && !(q->flags & HTTP_QUEUE_STARTED)) {
                if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_STARTED)) {
                    q->flags |= HTTP_QUEUE_STARTED;
                    q->stage->start(q);
                }
            }
        }
    }
    qhead = tx->queue[HTTP_QUEUE_TX];
    for (q = qhead->prevQ; q->prevQ != qhead; q = prevQ) {
        prevQ = q->prevQ;
        if (q->start && !(q->flags & HTTP_QUEUE_STARTED)) {
            q->flags |= HTTP_QUEUE_STARTED;
            q->stage->start(q);
        }
    }
    httpStartHandler(conn);

    if (tx->pendingFinalize) {
        tx->finalizedOutput = 0;
        httpFinalizeOutput(conn);
    }
}


PUBLIC void httpReadyHandler(HttpConn *conn)
{
    HttpQueue   *q;

    q = conn->writeq;
    if (q->stage && q->stage->ready && !(q->flags & HTTP_QUEUE_READY)) {
        q->flags |= HTTP_QUEUE_READY;
        q->stage->ready(q);
    }
}


static void httpStartHandler(HttpConn *conn)
{
    HttpQueue   *q;

    assert(!conn->tx->started);

    conn->tx->started = 1;
    q = conn->writeq;
    if (q->stage->start && !(q->flags & HTTP_QUEUE_STARTED)) {
        q->flags |= HTTP_QUEUE_STARTED;
        q->stage->start(q);
    }
}


PUBLIC bool httpQueuesNeedService(HttpConn *conn)
{
    HttpQueue   *q;

    q = conn->serviceq;
    return (q->scheduleNext != q);
}


/*
    Run the queue service routines until there is no more work to be done.
    If flags & HTTP_BLOCK, this routine may block while yielding.  Return true if actual work was done.
 */
PUBLIC bool httpServiceQueues(HttpConn *conn, int flags)
{
    HttpQueue   *q;
    bool        workDone;

    workDone = 0;

    while (conn->state < HTTP_STATE_COMPLETE && (q = httpGetNextQueueForService(conn->serviceq)) != NULL) {
        if (q->servicing) {
            /* Called re-entrantly */
            q->flags |= HTTP_QUEUE_RESERVICE;
        } else {
            assert(q->schedulePrev == q->scheduleNext);
            httpServiceQueue(q);
            workDone = 1;
        }
        if (mprNeedYield() && (flags & HTTP_BLOCK)) {
            mprYield(0);
        }
    }
    /* 
        Always do a yield if requested even if there are no queues to service 
     */
    if (mprNeedYield() && (flags & HTTP_BLOCK)) {
        mprYield(0);
    }
    return workDone;
}


PUBLIC void httpDiscardData(HttpConn *conn, int dir)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;

    tx = conn->tx;
    if (tx == 0) {
        return;
    }
    qhead = tx->queue[dir];
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        httpDiscardQueueData(q, 1);
    }
}


static bool matchFilter(HttpConn *conn, HttpStage *filter, HttpRoute *route, int dir)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (filter->match) {
        return filter->match(conn, route, dir);
    }
    if (filter->extensions && tx->ext) {
        return mprLookupKey(filter->extensions, tx->ext) != 0;
    }
    return 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/queue.c ************/


/*
    queue.c -- Queue support routines. Queues are the bi-directional data flow channels for the pipeline.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void manageQueue(HttpQueue *q, int flags);

/************************************ Code ************************************/

PUBLIC HttpQueue *httpCreateQueueHead(HttpConn *conn, cchar *name)
{
    HttpQueue   *q;

    if ((q = mprAllocObj(HttpQueue, manageQueue)) == 0) {
        return 0;
    }
    httpInitQueue(conn, q, name);
    httpInitSchedulerQueue(q);
    return q;
}


/*
    Create a queue associated with a connection.
    Prev may be set to the previous queue in a pipeline. If so, then the Conn.readq and writeq are updated.
 */
PUBLIC HttpQueue *httpCreateQueue(HttpConn *conn, HttpStage *stage, int dir, HttpQueue *prev)
{
    HttpQueue   *q;

    if ((q = mprAllocObj(HttpQueue, manageQueue)) == 0) {
        return 0;
    }
    q->conn = conn;
    httpInitQueue(conn, q, sfmt("%s-%s", stage->name, dir == HTTP_QUEUE_TX ? "tx" : "rx"));
    httpInitSchedulerQueue(q);
    httpAssignQueue(q, stage, dir);
    if (prev) {
        httpAppendQueue(prev, q);
        if (dir == HTTP_QUEUE_RX) {
            conn->readq = conn->tx->queue[HTTP_QUEUE_RX]->prevQ;
        } else {
            conn->writeq = conn->tx->queue[HTTP_QUEUE_TX]->nextQ;
        }
    }
    return q;
}


static void manageQueue(HttpQueue *q, int flags)
{
    HttpPacket      *packet;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(q->name);
        mprMark(q->nextQ);
        for (packet = q->first; packet; packet = packet->next) {
            mprMark(packet);
        }
        mprMark(q->conn);
        mprMark(q->last);
        mprMark(q->prevQ);
        mprMark(q->stage);
        mprMark(q->scheduleNext);
        mprMark(q->schedulePrev);
        mprMark(q->pair);
        mprMark(q->queueData);
        if (q->nextQ && q->nextQ->stage) {
            /* Not a queue head */
            mprMark(q->nextQ);
        }
    }
}


PUBLIC void httpAssignQueue(HttpQueue *q, HttpStage *stage, int dir)
{
    q->stage = stage;
    q->close = stage->close;
    q->open = stage->open;
    q->start = stage->start;
    if (dir == HTTP_QUEUE_TX) {
        q->put = stage->outgoing;
        q->service = stage->outgoingService;
    } else {
        q->put = stage->incoming;
        q->service = stage->incomingService;
    }
}


PUBLIC void httpInitQueue(HttpConn *conn, HttpQueue *q, cchar *name)
{
    HttpTx      *tx;

    tx = conn->tx;
    q->conn = conn;
    q->nextQ = q;
    q->prevQ = q;
    q->name = sclone(name);
    q->max = conn->limits->bufferSize;
    q->low = q->max / 100 *  5;
    if (tx && tx->chunkSize > 0) {
        q->packetSize = tx->chunkSize;
    } else {
        q->packetSize = q->max;
    }
}


PUBLIC void httpSetQueueLimits(HttpQueue *q, ssize low, ssize max)
{
    q->low = low;
    q->max = max;
}


#if KEEP
/*
    Insert a queue after the previous element
 */
PUBLIC void httpAppendQueueToHead(HttpQueue *head, HttpQueue *q)
{
    q->nextQ = head;
    q->prevQ = head->prevQ;
    head->prevQ->nextQ = q;
    head->prevQ = q;
}
#endif


PUBLIC bool httpIsQueueSuspended(HttpQueue *q)
{
    return (q->flags & HTTP_QUEUE_SUSPENDED) ? 1 : 0;
}


PUBLIC void httpSuspendQueue(HttpQueue *q)
{
    q->flags |= HTTP_QUEUE_SUSPENDED;
}


PUBLIC bool httpIsSuspendQueue(HttpQueue *q)
{
    return q->flags & HTTP_QUEUE_SUSPENDED;
}


/*
    Remove all data in the queue. If removePackets is true, actually remove the packet too.
    This preserves the header and EOT packets.
 */
PUBLIC void httpDiscardQueueData(HttpQueue *q, bool removePackets)
{
    HttpPacket  *packet, *prev, *next;
    ssize       len;

    if (q == 0) {
        return;
    }
    for (prev = 0, packet = q->first; packet; packet = next) {
        next = packet->next;
        if (packet->flags & (HTTP_PACKET_RANGE | HTTP_PACKET_DATA)) {
            if (removePackets) {
                if (prev) {
                    prev->next = next;
                } else {
                    q->first = next;
                }
                if (packet == q->last) {
                    q->last = prev;
                }
                q->count -= httpGetPacketLength(packet);
                assert(q->count >= 0);
                continue;
            } else {
                len = httpGetPacketLength(packet);
                q->conn->tx->length -= len;
                q->count -= len;
                assert(q->count >= 0);
                if (packet->content) {
                    mprFlushBuf(packet->content);
                }
            }
        }
        prev = packet;
    }
}


/*
    Flush queue data by scheduling the queue and servicing all scheduled queues. Return true if there is room for more data.
    If blocking is requested, the call will block until the queue count falls below the queue max.
    WARNING: Be very careful when using blocking == true. Should only be used by end applications and not by middleware.
 */
PUBLIC bool httpFlushQueue(HttpQueue *q, int flags)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;

    /*
        Initiate flushing
     */
    httpScheduleQueue(q);
    httpServiceQueues(conn, flags);

    if (flags & HTTP_BLOCK) {
        /*
            Blocking mode: Fully drain the pipeline. This blocks until the connector has written all the data to the O/S socket.
         */
        while (tx->writeBlocked || conn->connectorq->count > 0 || conn->connectorq->ioCount) {
            if (conn->connError) {
                break;
            }
            assert(!tx->finalizedConnector);
            assert(conn->connectorq->count > 0 || conn->connectorq->ioCount);
            if (!mprWaitForSingleIO((int) conn->sock->fd, MPR_WRITABLE, conn->limits->inactivityTimeout)) {
                break;
            }
            conn->lastActivity = conn->http->now;
            httpResumeQueue(conn->connectorq);
            httpServiceQueues(conn, flags);
        }
    }
    return (q->count < q->max) ? 1 : 0;
}


PUBLIC void httpFlush(HttpConn *conn)
{
    httpFlushQueue(conn->writeq, HTTP_NON_BLOCK);
}


/*
    Flush the write queue. In sync mode, this call may yield. 
 */
PUBLIC void httpFlushAll(HttpConn *conn)
{
    httpFlushQueue(conn->writeq, conn->async ? HTTP_NON_BLOCK : HTTP_BLOCK);
}


PUBLIC void httpResumeQueue(HttpQueue *q)
{
    if (q) {
        q->flags &= ~HTTP_QUEUE_SUSPENDED;
        httpScheduleQueue(q);
    }
}


PUBLIC HttpQueue *httpFindPreviousQueue(HttpQueue *q)
{
    while (q->prevQ && q->prevQ->stage && q->prevQ != q) {
        q = q->prevQ;
        if (q->service) {
            return q;
        }
    }
    return 0;
}


PUBLIC HttpQueue *httpGetNextQueueForService(HttpQueue *q)
{
    HttpQueue     *next;

    if (q->scheduleNext != q) {
        next = q->scheduleNext;
        next->schedulePrev->scheduleNext = next->scheduleNext;
        next->scheduleNext->schedulePrev = next->schedulePrev;
        next->schedulePrev = next->scheduleNext = next;
        return next;
    }
    return 0;
}


/*
    Return the number of bytes the queue will accept. Always positive.
 */
PUBLIC ssize httpGetQueueRoom(HttpQueue *q)
{
    assert(q->max > 0);
    assert(q->count >= 0);

    if (q->count >= q->max) {
        return 0;
    }
    return q->max - q->count;
}


PUBLIC void httpInitSchedulerQueue(HttpQueue *q)
{
    q->scheduleNext = q;
    q->schedulePrev = q;
}


/*
    Append a queue after the previous element
 */
PUBLIC void httpAppendQueue(HttpQueue *prev, HttpQueue *q)
{
    q->nextQ = prev->nextQ;
    q->prevQ = prev;
    prev->nextQ->prevQ = q;
    prev->nextQ = q;
}


PUBLIC bool httpIsQueueEmpty(HttpQueue *q)
{
    return q->first == 0;
}


PUBLIC void httpRemoveQueue(HttpQueue *q)
{
    q->prevQ->nextQ = q->nextQ;
    q->nextQ->prevQ = q->prevQ;
    q->prevQ = q->nextQ = q;
}


PUBLIC void httpScheduleQueue(HttpQueue *q)
{
    HttpQueue     *head;

    assert(q->conn);
    head = q->conn->serviceq;

    if (q->scheduleNext == q && !(q->flags & HTTP_QUEUE_SUSPENDED)) {
        q->scheduleNext = head;
        q->schedulePrev = head->schedulePrev;
        head->schedulePrev->scheduleNext = q;
        head->schedulePrev = q;
    }
}


PUBLIC void httpServiceQueue(HttpQueue *q)
{
    q->conn->currentq = q;

    if (q->servicing) {
        q->flags |= HTTP_QUEUE_RESERVICE;
    } else {
        /*
            Since we are servicing this "q" now, we can remove from the schedule queue if it is already queued.
         */
        if (q->conn->serviceq->scheduleNext == q) {
            httpGetNextQueueForService(q->conn->serviceq);
        }
        if (!(q->flags & HTTP_QUEUE_SUSPENDED)) {
            q->servicing = 1;
            q->service(q);
            if (q->flags & HTTP_QUEUE_RESERVICE) {
                q->flags &= ~HTTP_QUEUE_RESERVICE;
                httpScheduleQueue(q);
            }
            q->flags |= HTTP_QUEUE_SERVICED;
            q->servicing = 0;
        }
    }
}


/*
    Return true if the next queue will accept this packet. If not, then disable the queue's service procedure.
    This may split the packet if it exceeds the downstreams maximum packet size.
 */
PUBLIC bool httpWillNextQueueAcceptPacket(HttpQueue *q, HttpPacket *packet)
{
    HttpQueue   *nextQ;
    ssize       size;

    nextQ = q->nextQ;
    size = httpGetPacketLength(packet);
    if (size <= nextQ->packetSize && (size + nextQ->count) <= nextQ->max) {
        return 1;
    }
    httpResizePacket(q, packet, 0);
    size = httpGetPacketLength(packet);
    assert(size <= nextQ->packetSize);
    /* 
        Packet size is now acceptable. Accept the packet if the queue is mostly empty (< low) or if the 
        packet will fit entirely under the max or if the queue.
        NOTE: queue maximums are advisory. We choose to potentially overflow the max here to optimize the case where
        the queue may have say one byte and a max size packet would overflow by 1.
     */
    if (nextQ->count < nextQ->low || (size + nextQ->count) <= nextQ->max) {
        return 1;
    }
    /*
        The downstream queue cannot accept this packet, so disable queue and mark the downstream queue as full and service 
     */
    httpSuspendQueue(q);
    if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(nextQ);
    }
    return 0;
}


#if KEEP
PUBLIC bool httpWillQueueAcceptPacket(HttpQueue *q, HttpPacket *packet, bool split)
{
    ssize       size;

    size = httpGetPacketLength(packet);
    if (size <= q->packetSize && (size + q->count) <= q->max) {
        return 1;
    }
    if (split) {
        httpResizePacket(q, packet, 0);
        size = httpGetPacketLength(packet);
        assert(size <= q->packetSize);
        if ((size + q->count) <= q->max) {
            return 1;
        }
    }
    /*
        The downstream queue is full, so disable the queue and mark the downstream queue as full and service 
     */
    if (!(q->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(q);
    }
    return 0;
}
#endif


/*
    Return true if the next queue will accept a certain amount of data. If not, then disable the queue's service procedure.
    Will not split the packet.
 */
PUBLIC bool httpWillNextQueueAcceptSize(HttpQueue *q, ssize size)
{
    HttpQueue   *nextQ;

    nextQ = q->nextQ;
    if (size <= nextQ->packetSize && (size + nextQ->count) <= nextQ->max) {
        return 1;
    }
    httpSuspendQueue(q);
    if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(nextQ);
    }
    return 0;
}


#if ME_DEBUG
PUBLIC bool httpVerifyQueue(HttpQueue *q)
{
    HttpPacket  *packet;
    ssize       count;

    count = 0;
    for (packet = q->first; packet; packet = packet->next) {
        if (packet->next == 0) {
            assert(packet == q->last);
        }
        count += httpGetPacketLength(packet);
    }
    assert(count == q->count);
    return count <= q->count;
}
#endif

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/rangeFilter.c ************/


/*
    rangeFilter.c - Ranged request filter.
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Defines ***********************************/

#define HTTP_RANGE_BUFSIZE 128              /* Packet size to hold range boundary */

/********************************** Forwards **********************************/

static bool applyRange(HttpQueue *q, HttpPacket *packet);
static void createRangeBoundary(HttpConn *conn);
static HttpPacket *createRangePacket(HttpConn *conn, HttpRange *range);
static HttpPacket *createFinalRangePacket(HttpConn *conn);
static void outgoingRangeService(HttpQueue *q);
static bool fixRangeLength(HttpConn *conn);
static int matchRange(HttpConn *conn, HttpRoute *route, int dir);
static void startRange(HttpQueue *q);

/*********************************** Code *************************************/

PUBLIC int httpOpenRangeFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("rangeFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->rangeFilter = filter;
    filter->match = matchRange; 
    filter->start = startRange; 
    filter->outgoingService = outgoingRangeService; 
    return 0;
}


/*
    This is called twice: once for TX and once for RX
 */
static int matchRange(HttpConn *conn, HttpRoute *route, int dir)
{
    assert(conn->rx);

    httpSetHeader(conn, "Accept-Ranges", "bytes");
    if ((dir & HTTP_STAGE_TX) && conn->tx->outputRanges) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


static void startRange(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;
    /*
        The httpContentNotModified routine can set outputRanges to zero if returning not-modified.
     */
    if (tx->outputRanges == 0 || tx->status != HTTP_CODE_OK || !fixRangeLength(conn)) {
        httpRemoveQueue(q);
        tx->outputRanges = 0;
    } else {
        tx->status = HTTP_CODE_PARTIAL;
        if (tx->outputRanges->next) {
            createRangeBoundary(conn);
        }
    }
}


static void outgoingRangeService(HttpQueue *q)
{
    HttpPacket  *packet;
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (packet->flags & HTTP_PACKET_DATA) {
            if (!applyRange(q, packet)) {
                return;
            }
        } else {
            /*
                Send headers and end packet downstream
             */
            if (packet->flags & HTTP_PACKET_END && tx->rangeBoundary) {
                httpPutPacketToNext(q, createFinalRangePacket(conn));
            }
            if (!httpWillNextQueueAcceptPacket(q, packet)) {
                httpPutBackPacket(q, packet);
                return;
            }
            httpPutPacketToNext(q, packet);
        }
    }
}


static bool applyRange(HttpQueue *q, HttpPacket *packet)
{
    HttpRange   *range;
    HttpConn    *conn;
    HttpTx      *tx;
    MprOff      endPacket, length, gap, span;
    ssize       count;

    conn = q->conn;
    tx = conn->tx;
    range = tx->currentRange;

    if (mprNeedYield()) {
        httpScheduleQueue(q);
        httpPutBackPacket(q, packet);
        return 0;
    }
    /*
        Process the data packet over multiple ranges ranges until all the data is processed or discarded.
        A packet may contain data or it may be empty with an associated entityLength. If empty, range packets
        are filled with entity data as required.
     */
    while (range && packet) {
        length = httpGetPacketEntityLength(packet);
        if (length <= 0) {
            break;
        }
        endPacket = tx->rangePos + length;
        if (endPacket < range->start) {
            /* Packet is before the next range, so discard the entire packet and seek forwards */
            tx->rangePos += length;
            break;

        } else if (tx->rangePos < range->start) {
            /*  Packet starts before range so skip some data, but some packet data is in range */
            gap = (range->start - tx->rangePos);
            tx->rangePos += gap;
            if (gap < length) {
                httpAdjustPacketStart(packet, (ssize) gap);
            }
            /* Keep going and examine next range */

        } else {
            /* In range */
            assert(range->start <= tx->rangePos && tx->rangePos < range->end);
            span = min(length, (range->end - tx->rangePos));
            count = (ssize) min(span, q->nextQ->packetSize);
            assert(count > 0);
            if (!httpWillNextQueueAcceptSize(q, count)) {
                httpPutBackPacket(q, packet);
                return 0;
            }
            if (length > count) {
                /* Split packet if packet extends past range */
                httpPutBackPacket(q, httpSplitPacket(packet, count));
            }
            if (packet->fill && (*packet->fill)(q, packet, tx->rangePos, count) < 0) {
                return 0;
            }
            if (tx->rangeBoundary) {
                httpPutPacketToNext(q, createRangePacket(conn, range));
            }
            httpPutPacketToNext(q, packet);
            packet = 0;
            tx->rangePos += count;
        }
        if (tx->rangePos >= range->end) {
            tx->currentRange = range = range->next;
        }
    }
    return 1;
}


/*
    Create a range boundary packet
 */
static HttpPacket *createRangePacket(HttpConn *conn, HttpRange *range)
{
    HttpPacket  *packet;
    HttpTx      *tx;
    char        *length;

    tx = conn->tx;

    length = (tx->entityLength >= 0) ? itos(tx->entityLength) : "*";
    packet = httpCreatePacket(HTTP_RANGE_BUFSIZE);
    packet->flags |= HTTP_PACKET_RANGE;
    mprPutToBuf(packet->content, 
        "\r\n--%s\r\n"
        "Content-Range: bytes %lld-%lld/%s\r\n\r\n",
        tx->rangeBoundary, range->start, range->end - 1, length);
    return packet;
}


/*
    Create a final range packet that follows all the data
 */
static HttpPacket *createFinalRangePacket(HttpConn *conn)
{
    HttpPacket  *packet;
    HttpTx      *tx;

    tx = conn->tx;

    packet = httpCreatePacket(HTTP_RANGE_BUFSIZE);
    packet->flags |= HTTP_PACKET_RANGE;
    mprPutToBuf(packet->content, "\r\n--%s--\r\n", tx->rangeBoundary);
    return packet;
}


/*
    Create a range boundary. This is required if more than one range is requested.
 */
static void createRangeBoundary(HttpConn *conn)
{
    HttpTx      *tx;
    int         when;

    tx = conn->tx;
    assert(tx->rangeBoundary == 0);
    when = (int) conn->http->now;
    tx->rangeBoundary = sfmt("%08X%08X", PTOI(tx) + PTOI(conn) * when, when);
}


/*
    Ensure all the range limits are within the entity size limits. Fixup negative ranges.
 */
static bool fixRangeLength(HttpConn *conn)
{
    HttpTx      *tx;
    HttpRange   *range;
    MprOff      length;

    tx = conn->tx;
    length = tx->entityLength ? tx->entityLength : tx->length;
    if (length <= 0) {
        return 0;
    }
    for (range = tx->outputRanges; range; range = range->next) {
        /*
                Range: 0-49             first 50 bytes
                Range: 50-99,200-249    Two 50 byte ranges from 50 and 200
                Range: -50              Last 50 bytes
                Range: 1-               Skip first byte then emit the rest
         */
        if (length) {
            if (range->end > length) {
                range->end = length;
            }
            if (range->start > length) {
                range->start = length;
            }
        }
        if (range->start < 0) {
            if (length <= 0) {
                /*
                    Cannot compute an offset from the end as we don't know the entity length and it is not 
                    always possible or wise to buffer all the output.
                 */
                httpError(conn, HTTP_CODE_RANGE_NOT_SATISFIABLE, "Cannot compute end range with unknown content length"); 
                return 0;
            }
            /* select last -range-end bytes */
            range->start = length - range->end + 1;
            range->end = length;
        }
        if (range->end < 0) {
            if (length <= 0) {
                return 0;
            }
            range->end = length - range->end - 1;
        }
        range->len = (int) (range->end - range->start);
    }
    return 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/route.c ************/


/*
    route.c -- Http request routing 

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



#if ME_COM_PCRE
 #include    "pcre.h"
#endif

/********************************** Forwards **********************************/

#undef  GRADUATE_LIST
#define GRADUATE_LIST(route, field) \
    if (route->field == 0) { \
        route->field = mprCreateList(-1, 0); \
    } else if (route->parent && route->field == route->parent->field) { \
        route->field = mprCloneList(route->parent->field); \
    }

#undef  GRADUATE_HASH
#define GRADUATE_HASH(route, field) \
    if (!route->field || (route->parent && route->field == route->parent->field)) { \
        route->field = mprCloneHash(route->parent->field); \
    }

/********************************** Forwards **********************************/

static void addUniqueItem(MprList *list, HttpRouteOp *op);
static int checkRoute(HttpConn *conn, HttpRoute *route);
static HttpLang *createLangDef(cchar *path, cchar *suffix, int flags);
static HttpRouteOp *createRouteOp(cchar *name, int flags);
static void definePathVars(HttpRoute *route);
static void defineHostVars(HttpRoute *route);
static char *expandTokens(HttpConn *conn, cchar *path);
static char *expandPatternTokens(cchar *str, cchar *replacement, int *matches, int matchCount);
static char *expandRequestTokens(HttpConn *conn, char *targetKey);
static void finalizePattern(HttpRoute *route);
static char *finalizeReplacement(HttpRoute *route, cchar *str);
static char *finalizeTemplate(HttpRoute *route);
static bool opPresent(MprList *list, HttpRouteOp *op);
static void manageRoute(HttpRoute *route, int flags);
static void manageLang(HttpLang *lang, int flags);
static void manageRouteOp(HttpRouteOp *op, int flags);
static int matchRequestUri(HttpConn *conn, HttpRoute *route);
static int matchRoute(HttpConn *conn, HttpRoute *route);
static int selectHandler(HttpConn *conn, HttpRoute *route);
static int testCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *condition);
static char *trimQuotes(char *str);
static int updateRequest(HttpConn *conn, HttpRoute *route, HttpRouteOp *update);

/************************************ Code ************************************/
/*
    Host may be null
 */
PUBLIC HttpRoute *httpCreateRoute(HttpHost *host)
{
    Http        *http;
    HttpRoute   *route;

    http = HTTP;
    if ((route = mprAllocObj(HttpRoute, manageRoute)) == 0) {
        return 0;
    }
    route->auth = httpCreateAuth();
    route->defaultLanguage = sclone("en");
    route->home = route->documents = mprGetCurrentPath();
    route->flags = HTTP_ROUTE_STEALTH;
#if ME_DEBUG
    route->flags |= HTTP_ROUTE_SHOW_ERRORS;
    route->keepSource = 1;
#endif
    route->update = 1;
    route->host = host;
    route->http = HTTP;
    route->lifespan = ME_MAX_CACHE_DURATION;
    route->pattern = MPR->emptyString;
    route->targetRule = sclone("run");
    route->autoDelete = 1;
    route->workers = -1;
    route->prefix = MPR->emptyString;
    route->serverPrefix = MPR->emptyString;
    route->trace = http->trace;

    route->headers = mprCreateList(-1, MPR_LIST_STABLE);
    route->handlers = mprCreateList(-1, MPR_LIST_STABLE);
    route->indexes = mprCreateList(-1, MPR_LIST_STABLE);
    route->inputStages = mprCreateList(-1, MPR_LIST_STABLE);
    route->outputStages = mprCreateList(-1, MPR_LIST_STABLE);

    route->extensions = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    route->errorDocuments = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STABLE);
    route->methods = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    route->vars = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);

    httpAddRouteMethods(route, NULL);
    httpAddRouteFilter(route, http->rangeFilter->name, NULL, HTTP_STAGE_TX);
    httpAddRouteFilter(route, http->chunkFilter->name, NULL, HTTP_STAGE_RX | HTTP_STAGE_TX);

#if KEEP
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "Content-Security-Policy", "default-src 'self'");
#endif
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-XSS-Protection", "1; mode=block");
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Frame-Options", "SAMEORIGIN");
    httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Content-Type-Options", "nosniff");

    if (MPR->httpService) {
        route->limits = mprMemdup(http->serverLimits ? http->serverLimits : http->clientLimits, sizeof(HttpLimits));
    }
    route->mimeTypes = MPR->mimeTypes;
    route->mutex = mprCreateLock();

    if ((route->mimeTypes = mprCreateMimeTypes("mime.types")) == 0) {
        route->mimeTypes = MPR->mimeTypes;
    }
    definePathVars(route);
    httpSetDefaultDirs(route);
    return route;
}


/*
    Create a new location block. Inherit from the parent. We use a copy-on-write scheme if these are modified later.
 */
PUBLIC HttpRoute *httpCreateInheritedRoute(HttpRoute *parent)
{
    HttpRoute  *route;

    if (!parent && (parent = httpGetDefaultRoute(0)) == 0) {
        return 0;
    }
    if ((route = mprAllocObj(HttpRoute, manageRoute)) == 0) {
        return 0;
    }
    route->auth = httpCreateInheritedAuth(parent->auth);
    route->autoDelete = parent->autoDelete;
    route->caching = parent->caching;
    route->client = parent->client;
    route->combine = parent->combine;
    route->conditions = parent->conditions;
    route->config = parent->config;
    route->configLoaded = parent->configLoaded;
    route->connector = parent->connector;
    route->cookie = parent->cookie;
    route->corsAge = parent->corsAge;
    route->corsCredentials = parent->corsCredentials;
    route->corsHeaders = parent->corsHeaders;
    route->corsMethods = parent->corsMethods;
    route->corsOrigin = parent->corsOrigin;
    route->data = parent->data;
    route->database = parent->database;
    route->defaultLanguage = parent->defaultLanguage;
    route->documents = parent->documents;
    route->envPrefix = parent->envPrefix;
    route->eroute = parent->eroute;
    route->errorDocuments = parent->errorDocuments;
    route->extensions = parent->extensions;
    route->flags = parent->flags & ~(HTTP_ROUTE_FREE_PATTERN);
    route->handler = parent->handler;
    route->handlers = parent->handlers;
    route->headers = parent->headers;
    route->home = parent->home;
    route->host = parent->host;
    route->http = HTTP;
    route->indexes = parent->indexes;
    route->inputStages = parent->inputStages;
    route->keepSource = parent->keepSource;
    route->languages = parent->languages;
    route->lifespan = parent->lifespan;
    route->limits = parent->limits;
    route->loaded = parent->loaded;
    route->map = parent->map;
    route->methods = parent->methods;
    route->mimeTypes = parent->mimeTypes;
    route->mode = parent->mode;
    route->optimizedPattern = parent->optimizedPattern;
    route->outputStages = parent->outputStages;
    route->params = parent->params;
    route->parent = parent;
    route->pattern = parent->pattern;
    route->patternCompiled = parent->patternCompiled;
    route->prefix = parent->prefix;
    route->prefixLen = parent->prefixLen;
    route->requestHeaders = parent->requestHeaders;
    route->responseFormat = parent->responseFormat;
    route->responseStatus = parent->responseStatus;
    route->script = parent->script;
    route->scriptPath = parent->scriptPath;
    route->serverPrefix = parent->serverPrefix;
    route->sourceName = parent->sourceName;
    route->ssl = parent->ssl;
    route->target = parent->target;
    route->targetRule = parent->targetRule;
    route->tokens = parent->tokens;
    route->trace = parent->trace;
    route->update = parent->update;
    route->updates = parent->updates;
    route->vars = parent->vars;
    route->workers = parent->workers;
    return route;
}


static void manageRoute(HttpRoute *route, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(route->auth);
        mprMark(route->caching);
        mprMark(route->client);
        mprMark(route->conditions);
        mprMark(route->config);
        mprMark(route->connector);
        mprMark(route->context);
        mprMark(route->cookie);
        mprMark(route->corsHeaders);
        mprMark(route->corsMethods);
        mprMark(route->corsOrigin);
        mprMark(route->data);
        mprMark(route->database);
        mprMark(route->defaultLanguage);
        mprMark(route->documents);
        mprMark(route->envPrefix);
        mprMark(route->eroute);
        mprMark(route->errorDocuments);
        mprMark(route->extensions);
        mprMark(route->handler);
        mprMark(route->handlers);
        mprMark(route->headers);
        mprMark(route->home);
        mprMark(route->host);
        mprMark(route->http);
        mprMark(route->indexes);
        mprMark(route->inputStages);
        mprMark(route->languages);
        mprMark(route->limits);
        mprMark(route->map);
        mprMark(route->methods);
        mprMark(route->mimeTypes);
        mprMark(route->mode);
        mprMark(route->mutex);
        mprMark(route->name);
        mprMark(route->optimizedPattern);
        mprMark(route->outputStages);
        mprMark(route->params);
        mprMark(route->parent);
        mprMark(route->pattern);
        mprMark(route->prefix);
        mprMark(route->requestHeaders);
        mprMark(route->responseFormat);
        mprMark(route->script);
        mprMark(route->scriptPath);
        mprMark(route->serverPrefix);
        mprMark(route->sourceName);
        mprMark(route->ssl);
        mprMark(route->startSegment);
        mprMark(route->startWith);
        mprMark(route->target);
        mprMark(route->targetRule);
        mprMark(route->tokens);
        mprMark(route->trace);
        mprMark(route->tplate);
        mprMark(route->updates);
        mprMark(route->vars);
        mprMark(route->webSocketsProtocol);

    } else if (flags & MPR_MANAGE_FREE) {
        if (route->patternCompiled && (route->flags & HTTP_ROUTE_FREE_PATTERN)) {
            free(route->patternCompiled);
        }
    }
}


PUBLIC HttpRoute *httpCreateDefaultRoute(HttpHost *host)
{
    HttpRoute   *route;

    assert(host);
    if ((route = httpCreateRoute(host)) == 0) {
        return 0;
    }
    httpSetRouteName(route, "default");
    httpFinalizeRoute(route);
    return route;
}


/*
    Create and configure a basic route. This is used for client side and Ejscript routes. Host may be null.
 */
PUBLIC HttpRoute *httpCreateConfiguredRoute(HttpHost *host, int serverSide)
{
    HttpRoute   *route;
    Http        *http;

    /*
        Create default incoming and outgoing pipelines. Order matters.
     */
    route = httpCreateRoute(host);
    http = route->http;
#if ME_HTTP_WEB_SOCKETS
    httpAddRouteFilter(route, http->webSocketFilter->name, NULL, HTTP_STAGE_RX | HTTP_STAGE_TX);
#endif
    if (serverSide) {
        httpAddRouteFilter(route, http->uploadFilter->name, NULL, HTTP_STAGE_RX);
    }
    return route;
}


PUBLIC HttpRoute *httpCreateAliasRoute(HttpRoute *parent, cchar *pattern, cchar *path, int status)
{
    HttpRoute   *route;

    assert(parent);
    assert(pattern && *pattern);

    if ((route = httpCreateInheritedRoute(parent)) == 0) {
        return 0;
    }
    httpSetRoutePattern(route, pattern, 0);
    if (path) {
        httpSetRouteDocuments(route, path);
    }
    route->responseStatus = status;
    return route;
}


/*
    This routine binds a new route to a URI. It creates a handler, route and binds a callback to that route. 
 */
PUBLIC HttpRoute *httpCreateActionRoute(HttpRoute *parent, cchar *pattern, HttpAction action)
{
    HttpRoute   *route;
    cchar       *name;

    if (!pattern || !action) {
        return 0;
    }
    if ((route = httpCreateInheritedRoute(parent)) != 0) {
        route->handler = route->http->actionHandler;
        httpSetRoutePattern(route, pattern, 0);
        name = strim(pattern, "^$", 0);
        httpDefineAction(name, action);
        httpSetRouteName(route, name);
        httpFinalizeRoute(route);
    }
    return route;
}


PUBLIC int httpStartRoute(HttpRoute *route)
{
#if !ME_ROM
    if (!(route->flags & HTTP_ROUTE_STARTED)) {
        route->flags |= HTTP_ROUTE_STARTED;
        if (route->trace != route->trace->parent) {
            httpOpenTraceLogFile(route->trace);
        }
    }
#endif
    return 0;
}


PUBLIC void httpStopRoute(HttpRoute *route)
{
}


/*
    Find the matching route and handler for a request. If any errors occur, the pass handler is used to 
    pass errors via the net/sendfile connectors onto the client. This process may rewrite the request 
    URI and may redirect the request.
 */
PUBLIC void httpRouteRequest(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    int         next, rewrites, match;

    rx = conn->rx;
    tx = conn->tx;
    route = 0;
    rewrites = 0;

    if (conn->error) {
        tx->handler = conn->http->passHandler;
        route = rx->route = conn->host->defaultRoute;

    } else {
        for (next = rewrites = 0; rewrites < ME_MAX_REWRITE; ) {
            if (next >= conn->host->routes->length) {
                break;
            }
            route = conn->host->routes->items[next++];
            if (route->startSegment && strncmp(rx->pathInfo, route->startSegment, route->startSegmentLen) != 0) {
                /* Failed to match the first URI segment, skip to the next group */
                if (next < route->nextGroup) {
                    next = route->nextGroup;
                }

            } else if (route->startWith && strncmp(rx->pathInfo, route->startWith, route->startWithLen) != 0) {
                /* Failed to match starting literal segment of the route pattern, advance to test the next route */
                continue;

            } else if ((match = matchRoute(conn, route)) == HTTP_ROUTE_REROUTE) {
                next = 0;
                route = 0;
                rewrites++;

            } else if (match == HTTP_ROUTE_OK) {
                break;
            }
        }
    }
    if (route == 0 || tx->handler == 0) {
        rx->route = conn->host->defaultRoute;
        httpError(conn, HTTP_CODE_BAD_METHOD, "Cannot find suitable route for request method");
        return;
    }
    rx->route = route;
    conn->limits = route->limits;
    conn->trace = route->trace;

    if (rewrites >= ME_MAX_REWRITE) {
        httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Too many request rewrites");
    }
    if (tx->finalized) {
        /* Pass handler can transmit the error */
        tx->handler = conn->http->passHandler;
    }
    if (tx->handler->module) {
        tx->handler->module->lastActivity = conn->lastActivity;
    }
}


static int matchRoute(HttpConn *conn, HttpRoute *route)
{
    HttpRx      *rx;
    char        *savePathInfo, *pathInfo;
    int         rc;

    assert(conn);
    assert(route);

    rx = conn->rx;
    savePathInfo = 0;

    //  TODO - remove first test
    assert(route->prefix);
    if (route->prefix && *route->prefix) {
        if (!sstarts(rx->pathInfo, route->prefix)) {
            return HTTP_ROUTE_REJECT;
        }
        savePathInfo = rx->pathInfo;
        pathInfo = &rx->pathInfo[route->prefixLen];
        if (*pathInfo == '\0') {
            pathInfo = "/";
        }
        rx->pathInfo = sclone(pathInfo);
        rx->scriptName = route->prefix;
    }
    if ((rc = matchRequestUri(conn, route)) == HTTP_ROUTE_OK) {
        rc = checkRoute(conn, route);
    }
    if (rc == HTTP_ROUTE_REJECT && savePathInfo) {
        /* Keep the modified pathInfo if OK or REWRITE */
        rx->pathInfo = savePathInfo;
        rx->scriptName = 0;
    }
    return rc;
}


static int matchRequestUri(HttpConn *conn, HttpRoute *route)
{
    HttpRx      *rx;

    assert(conn);
    assert(route);
    rx = conn->rx;

    if (route->patternCompiled) {
        rx->matchCount = pcre_exec(route->patternCompiled, NULL, rx->pathInfo, (int) slen(rx->pathInfo), 0, 0, 
            rx->matches, sizeof(rx->matches) / sizeof(int));
        if (route->flags & HTTP_ROUTE_NOT) {
            if (rx->matchCount > 0) {
                return HTTP_ROUTE_REJECT;
            }
            rx->matchCount = 1;
            rx->matches[0] = 0;
            rx->matches[1] = (int) slen(rx->pathInfo);

        } else if (rx->matchCount <= 0) {
            return HTTP_ROUTE_REJECT;
        }
    } else if (route->pattern && *route->pattern) {
        /* Pattern compilation failed */
        return HTTP_ROUTE_REJECT;
    }
    if (!mprLookupKey(route->methods, rx->method)) {
        if (!mprLookupKey(route->methods, "*")) {
            if (!(rx->flags & HTTP_HEAD && mprLookupKey(route->methods, "GET"))) {
                return HTTP_ROUTE_REJECT;
            }
        }
    }
    rx->route = route;
    return HTTP_ROUTE_OK;
}


static int checkRoute(HttpConn *conn, HttpRoute *route)
{
    HttpRouteOp     *op, *condition, *update;
    HttpRouteProc   *proc;
    HttpRx          *rx;
    HttpTx          *tx;
    cchar           *token, *value, *header, *field;
    int             next, rc, matched[ME_MAX_ROUTE_MATCHES * 2], count, result;

    assert(conn);
    assert(route);
    rx = conn->rx;
    tx = conn->tx;

    rx->target = route->target ? expandTokens(conn, route->target) : sclone(&conn->rx->pathInfo[1]);

    if (route->requestHeaders) {
        for (next = 0; (op = mprGetNextItem(route->requestHeaders, &next)) != 0; ) {
            if ((header = httpGetHeader(conn, op->name)) != 0) {
                count = pcre_exec(op->mdata, NULL, header, (int) slen(header), 0, 0, 
                    matched, sizeof(matched) / sizeof(int));
                result = count > 0;
                if (op->flags & HTTP_ROUTE_NOT) {
                    result = !result;
                }
                if (!result) {
                    return HTTP_ROUTE_REJECT;
                }
            }
        }
    }
    if (route->params) {
        for (next = 0; (op = mprGetNextItem(route->params, &next)) != 0; ) {
            if ((field = httpGetParam(conn, op->name, "")) != 0) {
                count = pcre_exec(op->mdata, NULL, field, (int) slen(field), 0, 0, 
                    matched, sizeof(matched) / sizeof(int));
                result = count > 0;
                if (op->flags & HTTP_ROUTE_NOT) {
                    result = !result;
                }
                if (!result) {
                    return HTTP_ROUTE_REJECT;
                }
            }
        }
    }
    if (route->conditions) {
        for (next = 0; (condition = mprGetNextItem(route->conditions, &next)) != 0; ) {
            rc = testCondition(conn, route, condition);
            if (rc == HTTP_ROUTE_REROUTE) {
                return rc;
            }
            if (condition->flags & HTTP_ROUTE_NOT) {
                rc = !rc;
            }
            if (rc == HTTP_ROUTE_REJECT) {
                return rc;
            }
        }
    }
    if (route->updates) {
        for (next = 0; (update = mprGetNextItem(route->updates, &next)) != 0; ) {
            if ((rc = updateRequest(conn, route, update)) == HTTP_ROUTE_REROUTE) {
                return rc;
            }
        }
    }
    if (route->prefix[0]) {
        httpSetParam(conn, "prefix", route->prefix);
    }
    if ((rc = selectHandler(conn, route)) != HTTP_ROUTE_OK) {
        return rc;
    }
    if (route->tokens) {
        for (next = 0; (token = mprGetNextItem(route->tokens, &next)) != 0; ) {
            int index = rx->matches[next * 2];
            if (index >= 0 && rx->pathInfo[index]) {
                value = snclone(&rx->pathInfo[index], rx->matches[(next * 2) + 1] - index);
                httpSetParam(conn, token, value);
            }
        }
    }
    if ((proc = mprLookupKey(conn->http->routeTargets, route->targetRule)) == 0) {
        httpError(conn, -1, "Cannot find route target rule \"%s\"", route->targetRule);
        return HTTP_ROUTE_REJECT;
    }
    if ((rc = (*proc)(conn, route, 0)) != HTTP_ROUTE_OK) {
        return rc;
    }
    if (tx->finalized) {
        tx->handler = conn->http->passHandler;
    } else if (tx->handler->rewrite) {
        rc = tx->handler->rewrite(conn);
    }
    return rc;
}


static int selectHandler(HttpConn *conn, HttpRoute *route)
{
    HttpRx      *rx;
    HttpTx      *tx;
    int         next, rc;

    assert(conn);
    assert(route);

    rx = conn->rx;
    tx = conn->tx;
    if (route->handler) {
        tx->handler = route->handler;
        return HTTP_ROUTE_OK;
    }
    for (next = 0; (tx->handler = mprGetNextStableItem(route->handlers, &next)) != 0; ) {
        rc = tx->handler->match(conn, route, 0);
        if (rc == HTTP_ROUTE_OK || rc == HTTP_ROUTE_REROUTE) {
            return rc;
        }
    }
    if (!tx->handler) {
        /*
            Now match by extensions
         */
        if (!tx->ext || (tx->handler = mprLookupKey(route->extensions, tx->ext)) == 0) {
            tx->handler = mprLookupKey(route->extensions, "");
        }
    }
    if (rx->flags & HTTP_TRACE) {
        /*
            Trace method always processed for all requests by the passHandler
         */
        tx->handler = conn->http->passHandler;
    }
    if (tx->finalized) {
        tx->handler = conn->http->passHandler;
    }
    return tx->handler ? HTTP_ROUTE_OK : HTTP_ROUTE_REJECT;
}


PUBLIC void httpSetHandler(HttpConn *conn, HttpStage *handler)
{
    conn->tx->handler = handler;
}


static cchar *mapContent(HttpConn *conn, cchar *filename)
{
    HttpRoute   *route;
    HttpRx      *rx;
    HttpTx      *tx;
    MprList     *extensions;
    MprPath     *info;
    bool        acceptGzip, zipped;
    cchar       *ext, *path;
    int         next;

    tx = conn->tx;
    rx = conn->rx;
    route = rx->route;
    info = &tx->fileInfo;

    if (route->map && !(tx->flags & HTTP_TX_NO_MAP)) {
        if ((extensions = mprLookupKey(route->map, tx->ext)) != 0) {
            acceptGzip = scontains(rx->acceptEncoding, "gzip") != 0;
            for (ITERATE_ITEMS(extensions, ext, next)) {
                zipped = sends(ext, "gz");
                if (zipped && !acceptGzip) {
                    continue;
                }
                path = mprReplacePathExt(filename, ext);
                if (mprGetPathInfo(path, info) == 0) {
                    httpTrace(conn, "request.map", "context", 
                        "originalFilename:'%s',filename:'%s'", filename, path);
                    filename = path;
                    if (zipped) {
                        httpSetHeader(conn, "Content-Encoding", "gzip");
                    }
                    break;
                }
            }
        }
    }
    return filename;
}


PUBLIC void httpMapFile(HttpConn *conn)
{
    HttpTx      *tx;
    HttpLang    *lang;
    cchar       *filename;

    tx = conn->tx;
    if (tx->filename) {
        return;
    }
    filename = conn->rx->target;
    lang = conn->rx->lang;
    if (lang && lang->path) {
        filename = mprJoinPath(lang->path, filename);
    }
    filename = mprJoinPath(conn->rx->route->documents, filename);
    filename = mapContent(conn, filename);
#if ME_ROM
    filename = mprGetRelPath(filename, NULL);
#endif
    httpSetFilename(conn, filename, 0);
}


/************************************ API *************************************/

PUBLIC int httpAddRouteCondition(HttpRoute *route, cchar *name, cchar *details, int flags)
{
    HttpRouteOp *op;
    cchar       *errMsg;
    char        *pattern, *value;
    int         column;

    assert(route);

    GRADUATE_LIST(route, conditions);
    if ((op = createRouteOp(name, flags)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (scaselessmatch(name, "auth") || scaselessmatch(name, "unauthorized")) {
        /* Nothing to do. Route->auth has it all */

    } else if (scaselessmatch(name, "missing")) {
        op->details = finalizeReplacement(route, "${request:filename}");

    } else if (scaselessmatch(name, "directory")) {
        op->details = finalizeReplacement(route, details);

    } else if (scaselessmatch(name, "exists")) {
        op->details = finalizeReplacement(route, details);

    } else if (scaselessmatch(name, "match")) {
        /* 
            Condition match string pattern
            String can contain matching ${tokens} from the route->pattern and can contain request ${tokens}
         */
        if (!httpTokenize(route, details, "%S %S", &value, &pattern)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        if ((op->mdata = pcre_compile2(pattern, 0, 0, &errMsg, &column, NULL)) == 0) {
            mprLog("error http route", 0, "Cannot compile condition match pattern. Error %s at column %d", errMsg, column);
            return MPR_ERR_BAD_SYNTAX;
        }
        op->details = finalizeReplacement(route, value);
        op->flags |= HTTP_ROUTE_FREE;

    } else if (scaselessmatch(name, "secure")) {
        if (!details || *details == '\0') {
            mprLog("error http config", 0, "Secure route condition is missing a redirect target in route \"%s\"", route->name);
        }
        op->details = finalizeReplacement(route, details);
    }
    addUniqueItem(route->conditions, op);
    return 0;
}


PUBLIC int httpAddRouteFilter(HttpRoute *route, cchar *name, cchar *extensions, int direction)
{
    HttpStage   *stage;
    HttpStage   *filter;
    char        *extlist, *word, *tok;
    int         pos, next;

    assert(route);

    for (ITERATE_ITEMS(route->outputStages, stage, next)) {
        if (smatch(stage->name, name)) {
            mprLog("warn http route", 0, "Stage \"%s\" is already configured for the route \"%s\". Ignoring.", 
                name, route->name);
            return 0;
        }
    }
    stage = httpLookupStage(name);
    if (stage == 0) {
        mprLog("error http route", 0, "Cannot find filter %s", name);
        return MPR_ERR_CANT_FIND;
    }
    /*
        Clone an existing stage because each filter stores its own set of extensions to match against
     */
    filter = httpCloneStage(stage);

    if (extensions && *extensions) {
        filter->extensions = mprCreateHash(0, MPR_HASH_CASELESS | MPR_HASH_STABLE);
        extlist = sclone(extensions);
        word = stok(extlist, " \t\r\n", &tok);
        while (word) {
            if (*word == '*' && word[1] == '.') {
                word += 2;
            } else if (*word == '.') {
                word++;
            } else if (*word == '\"' && word[1] == '\"') {
                word = "";
            }
            mprAddKey(filter->extensions, word, filter);
            word = stok(NULL, " \t\r\n", &tok);
        }
    }
    if (direction & HTTP_STAGE_RX && filter->incoming) {
        GRADUATE_LIST(route, inputStages);
        mprAddItem(route->inputStages, filter);
    }
    if (direction & HTTP_STAGE_TX && filter->outgoing) {
        GRADUATE_LIST(route, outputStages);
        if (smatch(name, "cacheFilter") && 
                (pos = mprGetListLength(route->outputStages) - 1) >= 0 &&
                smatch(((HttpStage*) mprGetLastItem(route->outputStages))->name, "chunkFilter")) {
            mprInsertItemAtPos(route->outputStages, pos, filter);
        } else {
            mprAddItem(route->outputStages, filter);
        }
    }
    return 0;
}


PUBLIC int httpAddRouteHandler(HttpRoute *route, cchar *name, cchar *extensions)
{
    HttpStage   *handler, *prior;
    char        *extlist, *word, *tok;

    assert(route);

    if ((handler = httpLookupStage(name)) == 0) {
        mprLog("error http route", 0, "Cannot find stage %s", name);
        return MPR_ERR_CANT_FIND;
    }
    if (route->handler) {
        mprLog("error http route", 0, "Cannot add handler \"%s\" to route \"%s\" once SetHandler used.", 
            handler->name, route->name);
    }
    if (!extensions && !handler->match) {
        mprLog("info http route", 2, "Adding handler \"%s\" without extensions to match", handler->name);
    }
    if (extensions) {
        /*
            Add to the handler extension hash. Skip over "*." and "."
         */ 
        GRADUATE_HASH(route, extensions);
        extlist = sclone(extensions);
        if ((word = stok(extlist, " \t\r\n", &tok)) == 0) {
            mprAddKey(route->extensions, "", handler);
        } else {
            while (word) {
                if (*word == '*' && word[1] == '\0') {
                    word++;
                } else if (*word == '*' && word[1] == '.') {
                    word += 2;
                } else if (*word == '.') {
                    word++;
                } else if (*word == '\"' && word[1] == '\"') {
                    word = "";
                }
                prior = mprLookupKey(route->extensions, word);
                if (prior && prior != handler) {
                    mprLog("warn http route", 0, "Route \"%s\" has multiple handlers defined for extension \"%s\". "
                            "Handlers: \"%s\", \"%s\".", route->name, word, handler->name, 
                            ((HttpStage*) mprLookupKey(route->extensions, word))->name);
                } else {
                    mprAddKey(route->extensions, word, handler);
                }
                word = stok(NULL, " \t\r\n", &tok);
            }
        }
    }
    if (handler->match && mprLookupItem(route->handlers, handler) < 0) {
        GRADUATE_LIST(route, handlers);
        if (smatch(name, "cacheHandler")) {
            mprInsertItemAtPos(route->handlers, 0, handler);
        } else {
            mprAddItem(route->handlers, handler);
        }
    }
    return 0;
}


PUBLIC void httpAddRouteMapping(HttpRoute *route, cchar *extensions, cchar *mappings)
{
    MprList     *mapList;
    cchar       *map;
    char        *etok, *ext, *mtok;
    ssize       len;

    if (extensions == 0) {
        return;
    }
    if (*extensions == '[') {
        extensions = strim(extensions, "[]", 0);
    }
    if (!route->map) {
        route->map = mprCreateHash(ME_MAX_ROUTE_MAP_HASH, MPR_HASH_STABLE);
    }
    for (ext = stok(sclone(extensions), ", \t", &etok); ext; ext = stok(NULL, ", \t", &etok)) {
        if (*ext == '.') {
            ext++;
        }
        if (*ext == '"') {
            ext++;
        }
        len = slen(ext);
        if (ext[len - 1] == '"') {
            ext[len - 1] = '\0';
        }
        mapList = mprCreateList(0, MPR_LIST_STABLE);
        for (map = stok(sclone(mappings), ", \t", &mtok); map; map = stok(NULL, ", \t", &mtok)) {
            mprAddItem(mapList, sreplace(map, "${1}", ext));
        }
        mprAddKey(route->map, ext, mapList);
    }
}


/*
    Param field valuePattern
 */
PUBLIC void httpAddRouteParam(HttpRoute *route, cchar *field, cchar *value, int flags)
{
    HttpRouteOp     *op;
    cchar           *errMsg;
    int             column;

    assert(route);
    assert(field && *field);
    assert(value && *value);

    GRADUATE_LIST(route, params);
    if ((op = createRouteOp(field, flags | HTTP_ROUTE_FREE)) == 0) {
        return;
    }
    if ((op->mdata = pcre_compile2(value, 0, 0, &errMsg, &column, NULL)) == 0) {
        mprLog("error http route", 0, "Cannot compile field pattern. Error %s at column %d", errMsg, column);
    } else {
        mprAddItem(route->params, op);
    }
}


/*
    RequestHeader [!] header pattern
 */
PUBLIC void httpAddRouteRequestHeaderCheck(HttpRoute *route, cchar *header, cchar *pattern, int flags)
{
    HttpRouteOp     *op;
    cchar           *errMsg;
    int             column;

    assert(route);
    assert(header && *header);
    assert(pattern && *pattern);

    GRADUATE_LIST(route, requestHeaders);
    if ((op = createRouteOp(header, flags | HTTP_ROUTE_FREE)) == 0) {
        return;
    }
    if ((op->mdata = pcre_compile2(pattern, 0, 0, &errMsg, &column, NULL)) == 0) {
        mprLog("error http route", 0, "Cannot compile header pattern. Error %s at column %d", errMsg, column);
    } else {
        mprAddItem(route->requestHeaders, op);
    }
}


/*
    Header [add|append|remove|set] header [value]
 */
PUBLIC void httpAddRouteResponseHeader(HttpRoute *route, int cmd, cchar *header, cchar *value)
{
    MprKeyValue     *pair;
    int             next;

    assert(route);
    assert(header && *header);

    GRADUATE_LIST(route, headers);
    if (cmd == HTTP_ROUTE_REMOVE_HEADER) {
        /*
            Remove existing route headers, but keep the remove record so that user headers will be removed too
         */
        for (ITERATE_ITEMS(route->headers, pair, next)) {
            if (smatch(pair->key, header)) {
                mprRemoveItem(route->headers, pair);
                next--;
            }
        }
    }
    mprAddItem(route->headers, mprCreateKeyPair(header, value, cmd));
}


/*
    Add a route update record. These run to modify a request.
        Update rule var value
        rule == "cmd|param"
        details == "var value"
    Value can contain pattern and request tokens.
 */
PUBLIC int httpAddRouteUpdate(HttpRoute *route, cchar *rule, cchar *details, int flags)
{
    HttpRouteOp *op;
    char        *value;

    assert(route);
    assert(rule && *rule);

    GRADUATE_LIST(route, updates);
    if ((op = createRouteOp(rule, flags)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (scaselessmatch(rule, "cmd")) {
        op->details = sclone(details);

    } else if (scaselessmatch(rule, "lang")) {
        /* Nothing to do */;

    } else if (scaselessmatch(rule, "param")) {
        if (!httpTokenize(route, details, "%S %S", &op->var, &value)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        op->value = finalizeReplacement(route, value);

    } else {
        return MPR_ERR_BAD_SYNTAX;
    }
    addUniqueItem(route->updates, op);
    return 0;
}


PUBLIC void httpClearRouteStages(HttpRoute *route, int direction)
{
    assert(route);

    if (direction & HTTP_STAGE_RX) {
        route->inputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (direction & HTTP_STAGE_TX) {
        route->outputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
}


PUBLIC void httpDefineRouteTarget(cchar *key, HttpRouteProc *proc)
{
    assert(key && *key);
    assert(proc);

    mprAddKey(HTTP->routeTargets, key, proc);
}


PUBLIC void httpDefineRouteCondition(cchar *key, HttpRouteProc *proc)
{
    assert(key && *key);
    assert(proc);

    mprAddKey(HTTP->routeConditions, key, proc);
}


PUBLIC void httpDefineRouteUpdate(cchar *key, HttpRouteProc *proc)
{
    assert(key && *key);
    assert(proc);

    mprAddKey(HTTP->routeUpdates, key, proc);
}


PUBLIC void *httpGetRouteData(HttpRoute *route, cchar *key)
{
    assert(route);
    assert(key && *key);

    if (!route->data) {
        return 0;
    }
    return mprLookupKey(route->data, key);
}


PUBLIC cchar *httpGetRouteDocuments(HttpRoute *route)
{
    assert(route);
    return route->documents;
}


PUBLIC cchar *httpGetRouteHome(HttpRoute *route)
{
    assert(route);
    return route->home;
}


PUBLIC cchar *httpGetRouteMethods(HttpRoute *route)
{
    assert(route);
    assert(route->methods);
    return mprHashKeysToString(route->methods, ",");
}


PUBLIC void httpResetRoutePipeline(HttpRoute *route)
{
    assert(route);

    if (!route->parent || route->caching != route->parent->caching) {
        route->caching = 0;
    }
    if (!route->parent || route->errorDocuments != route->parent->errorDocuments) {
        route->errorDocuments = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STABLE);
    }
    if (!route->parent || route->extensions != route->parent->extensions) {
        route->extensions = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    }
    if (!route->parent || route->handlers != route->parent->handlers) {
        route->handlers = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->inputStages != route->parent->inputStages) {
        route->inputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->indexes != route->parent->indexes) {
        route->indexes = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->outputStages != route->parent->outputStages) {
        route->outputStages = mprCreateList(-1, MPR_LIST_STABLE);
    }
    if (!route->parent || route->methods != route->parent->methods) {
        route->methods = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        httpAddRouteMethods(route, NULL);
    }
    if (!route->parent || route->requestHeaders != route->parent->requestHeaders) {
        route->requestHeaders = 0;
    }
    if (!route->parent || route->params != route->parent->params) {
        route->params = 0;
    }
    if (!route->parent || route->updates != route->parent->updates) {
        route->updates = 0;
    }
    if (!route->parent || route->conditions != route->parent->conditions) {
        route->conditions = 0;
    }
    if (!route->parent || route->map != route->parent->map) {
        route->map = 0;
    }
    if (!route->parent || route->languages != route->parent->languages) {
        route->languages = 0;
    }
    if (!route->parent || route->headers != route->parent->headers) {
        route->headers = 0;
#if FUTURE
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "Content-Security-Policy", "default-src 'self'");
#endif
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-XSS-Protection", "1; mode=block");
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Frame-Options", "SAMEORIGIN");
        httpAddRouteResponseHeader(route, HTTP_ROUTE_ADD_HEADER, "X-Content-Type-Options", "nosniff");
    }
}


PUBLIC void httpResetHandlers(HttpRoute *route)
{
    assert(route);
    route->handlers = mprCreateList(-1, MPR_LIST_STABLE);
}


PUBLIC void httpSetRouteAuth(HttpRoute *route, HttpAuth *auth)
{
    assert(route);
    route->auth = auth;
}


PUBLIC void httpSetRouteAutoDelete(HttpRoute *route, bool enable)
{
    assert(route);
    route->autoDelete = enable;
}


PUBLIC int httpSetRouteConnector(HttpRoute *route, cchar *name)
{
    HttpStage     *stage;

    assert(route);

    stage = httpLookupStage(name);
    if (stage == 0) {
        mprLog("error http route", 0, "Cannot find connector %s", name);
        return MPR_ERR_CANT_FIND;
    }
    route->connector = stage;
    return 0;
}


PUBLIC void httpSetRouteData(HttpRoute *route, cchar *key, void *data)
{
    assert(route);
    assert(key && *key);
    assert(data);

    if (route->data == 0) {
        route->data = mprCreateHash(-1, 0);
    } else {
        GRADUATE_HASH(route, data);
    }
    mprAddKey(route->data, key, data);
}


PUBLIC void httpSetRouteDocuments(HttpRoute *route, cchar *path)
{
    assert(route);
    assert(path && *path);

    route->documents = httpMakePath(route, route->home, path);
    httpSetRouteVar(route, "DOCUMENTS", route->documents);
}


PUBLIC void httpSetRouteFlags(HttpRoute *route, int flags)
{
    assert(route);
    route->flags = flags;
}


PUBLIC void httpSetRouteEnvEscape(HttpRoute *route, bool on)
{
    route->flags &= ~(HTTP_ROUTE_ENV_ESCAPE);
    if (on) {
        route->flags |= HTTP_ROUTE_ENV_ESCAPE;
    }
}


PUBLIC void httpSetRouteEnvPrefix(HttpRoute *route, cchar *prefix)
{
    route->envPrefix = sclone(prefix);
}


PUBLIC int httpSetRouteHandler(HttpRoute *route, cchar *name)
{
    HttpStage     *handler;

    assert(route);
    assert(name && *name);

    if ((handler = httpLookupStage(name)) == 0) {
        mprLog("error http route", 0, "Cannot find handler %s", name);
        return MPR_ERR_CANT_FIND;
    }
    route->handler = handler;
    return 0;
}


PUBLIC void httpSetRouteHome(HttpRoute *route, cchar *path)
{
    assert(route);
    assert(path && *path);

    route->home = httpMakePath(route, ".", path);
    httpSetRouteVar(route, "HOME", route->home);
}


/*
    WARNING: internal API only. 
 */
PUBLIC void httpSetRouteHost(HttpRoute *route, HttpHost *host)
{
    assert(route);
    assert(host);

    route->host = host;
    defineHostVars(route);
}


PUBLIC void httpSetRouteIgnoreEncodingErrors(HttpRoute *route, bool on)
{
    route->ignoreEncodingErrors = on;
}


PUBLIC void httpAddRouteIndex(HttpRoute *route, cchar *index)
{
    cchar   *item;
    int     next;

    assert(route);
    assert(index && *index);

    GRADUATE_LIST(route, indexes);
    for (ITERATE_ITEMS(route->indexes, item, next)) {
        if (smatch(index, item)) {
            return;
        }
    }
    mprAddItem(route->indexes, sclone(index));
}


PUBLIC void httpAddRouteMethods(HttpRoute *route, cchar *methods)
{
    char    *method, *tok;

    assert(route);

    if (methods == NULL || *methods == '\0') {
        methods = ME_HTTP_DEFAULT_METHODS;
    } else if (scaselessmatch(methods, "ALL")) {
       methods = "*";
    } else if (*methods == '[') {
        methods = strim(methods, "[]", 0);
    }
    if (!route->methods || (route->parent && route->methods == route->parent->methods)) {
        GRADUATE_HASH(route, methods);
    }
    tok = sclone(methods);
    while ((method = stok(tok, ", \t\n\r", &tok)) != 0) {
        mprAddKey(route->methods, method, LTOP(1));
    }
}


PUBLIC void httpRemoveRouteMethods(HttpRoute *route, cchar *methods)
{
    char    *method, *tok;

    assert(route);
    tok = sclone(methods);
    while ((method = stok(tok, ", \t\n\r", &tok)) != 0) {
        mprRemoveKey(route->methods, method);
    }
}


PUBLIC void httpResetRouteIndexes(HttpRoute *route)
{
    route->indexes = mprCreateList(-1, MPR_LIST_STABLE);
}


PUBLIC void httpSetRouteMethods(HttpRoute *route, cchar *methods)
{
    route->methods = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    httpAddRouteMethods(route, methods);
}


PUBLIC void httpSetRouteCookie(HttpRoute *route, cchar *cookie)
{
    assert(route);
    assert(cookie && *cookie);
    route->cookie = cookie;
}


PUBLIC void httpSetRouteName(HttpRoute *route, cchar *name)
{
    assert(route);
    assert(name && *name);

    route->name = sclone(name);
}


PUBLIC void httpSetRoutePattern(HttpRoute *route, cchar *pattern, int flags)
{
    assert(route);
    assert(pattern);

    route->flags |= (flags & HTTP_ROUTE_NOT);
    route->pattern = sclone(pattern);
    finalizePattern(route);
}


/*
    Set the prefix to empty if no prefix
 */
PUBLIC void httpSetRoutePrefix(HttpRoute *route, cchar *prefix)
{
    assert(route);

    if (prefix && *prefix) {
        if (smatch(prefix, "/")) {
            route->prefix = MPR->emptyString;
            route->prefixLen = 0;
        } else {
            route->prefix = sclone(prefix);
            route->prefixLen = slen(prefix);
            httpSetRouteVar(route, "PREFIX", prefix);
        }
    } else {
        route->prefix = MPR->emptyString;
        route->prefixLen = 0;
        httpSetRouteVar(route, "PREFIX", "");
    }
    if (route->pattern) {
        finalizePattern(route);
    }
    assert(route->prefix);
}


PUBLIC void httpSetRoutePreserveFrames(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_PRESERVE_FRAMES;
    if (on) {
        route->flags |= HTTP_ROUTE_PRESERVE_FRAMES;
    }
}


PUBLIC void httpSetRouteServerPrefix(HttpRoute *route, cchar *prefix)
{
    assert(route);
    assert(!smatch(prefix, "/"));

    if (prefix && *prefix) {
        if (smatch(prefix, "/")) {
            route->serverPrefix = MPR->emptyString;
        } else {
            route->serverPrefix = sclone(prefix);
        }
    } else {
        route->serverPrefix = MPR->emptyString;
    }
    assert(route->serverPrefix);
}


PUBLIC void httpSetRouteSessionVisibility(HttpRoute *route, bool visible)
{
    route->flags &= ~HTTP_ROUTE_VISIBLE_SESSION;
    if (visible) {
        route->flags |= HTTP_ROUTE_VISIBLE_SESSION;
    }
}


PUBLIC void httpSetRouteShowErrors(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_SHOW_ERRORS;
    if (on) {
        route->flags |= HTTP_ROUTE_SHOW_ERRORS;
    }
}


PUBLIC void httpSetRouteSource(HttpRoute *route, cchar *source)
{
    assert(route);
    assert(source);

    /* Source can be empty */
    route->sourceName = sclone(source);
}


PUBLIC void httpSetRouteScript(HttpRoute *route, cchar *script, cchar *scriptPath)
{
    assert(route);

    if (script) {
        assert(*script);
        route->script = sclone(script);
    }
    if (scriptPath) {
        assert(*scriptPath);
        route->scriptPath = sclone(scriptPath);
    }
}


PUBLIC void httpSetRouteStealth(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_STEALTH;
    if (on) {
        route->flags |= HTTP_ROUTE_STEALTH;
    }
}


/*
    Target names are extensible and hashed in http->routeTargets. 

        Target close
        Target redirect status [URI]
        Target run ${DOCUMENTS}/${request:uri}.gz
        Target run ${controller}-${action} 
        Target write [-r] status "Hello World\r\n"
 */
PUBLIC int httpSetRouteTarget(HttpRoute *route, cchar *rule, cchar *details)
{
    char    *redirect, *msg;

    assert(route);
    assert(rule && *rule);

    route->targetRule = sclone(rule);
    route->target = sclone(details);

    if (scaselessmatch(rule, "close")) {
        route->target = sclone(details);

    } else if (scaselessmatch(rule, "redirect")) {
        if (!httpTokenize(route, details, "%N ?S", &route->responseStatus, &redirect)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        route->target = finalizeReplacement(route, redirect);
        return 0;

    } else if (scaselessmatch(rule, "run")) {
        route->target = finalizeReplacement(route, details);

    } else if (scaselessmatch(rule, "write")) {
        /*
            Write [-r] status Message
         */
        if (sncmp(details, "-r", 2) == 0) {
            route->flags |= HTTP_ROUTE_RAW;
            details = &details[2];
        }
        if (!httpTokenize(route, details, "%N %S", &route->responseStatus, &msg)) {
            return MPR_ERR_BAD_SYNTAX;
        }
        route->target = finalizeReplacement(route, msg);

    } else {
        return MPR_ERR_BAD_SYNTAX;
    }
    return 0;
}


PUBLIC void httpSetRouteTemplate(HttpRoute *route, cchar *tplate)
{
    assert(route);
    assert(tplate && *tplate);

    route->tplate = sclone(tplate);
}


PUBLIC void httpSetRouteUploadDir(HttpRoute *route, cchar *dir)
{
    httpSetDir(route, "upload", dir);
}


PUBLIC void httpSetRouteWorkers(HttpRoute *route, int workers)
{
    assert(route);
    route->workers = workers;
}


PUBLIC void httpAddRouteErrorDocument(HttpRoute *route, int status, cchar *url)
{
    char    *code;

    assert(route);
    GRADUATE_HASH(route, errorDocuments);
    code = itos(status);
    mprAddKey(route->errorDocuments, code, sclone(url));
}


PUBLIC cchar *httpLookupRouteErrorDocument(HttpRoute *route, int code)
{
    char   *num;

    assert(route);
    if (route->errorDocuments == 0) {
        return 0;
    }
    num = itos(code);
    return (cchar*) mprLookupKey(route->errorDocuments, num);
}


/*
    Finalize the pattern. 
        - Change "\{n[:m]}" to "{n[:m]}"
        - Change "\~" to "~"
        - Change "(~ PAT ~)" to "(?: PAT )?"
        - Extract the tokens and change tokens: "{word}" to "([^/]*)"
 */
static void finalizePattern(HttpRoute *route)
{
    MprBuf      *pattern;
    cchar       *errMsg;
    char        *startPattern, *cp, *ep, *token, *field;
    ssize       len;
    int         column;

    assert(route);
    route->tokens = mprCreateList(-1, MPR_LIST_STABLE);
    pattern = mprCreateBuf(-1, -1);
    startPattern = route->pattern[0] == '^' ? &route->pattern[1] : route->pattern;

    if (route->name == 0) {
        route->name = sclone(startPattern);
    }
    if (route->tplate == 0) {
        /* Do this while the prefix is still in the route pattern */
        route->tplate = finalizeTemplate(route);
    }
    /*
        Create an simple literal startWith string to optimize route rejection.
     */
    len = strcspn(startPattern, "^$*+?.(|{[\\");
    if (len) {
        route->startWith = snclone(startPattern, len);
        route->startWithLen = len;
        if ((cp = strchr(&route->startWith[1], '/')) != 0) {
            route->startSegment = snclone(route->startWith, cp - route->startWith);
        } else {
            route->startSegment = route->startWith;
        }
        route->startSegmentLen = slen(route->startSegment);
    } else {
        /* Pattern has special characters */
        route->startWith = 0;
        route->startWithLen = 0;
        route->startSegmentLen = 0;
        route->startSegment = 0;
    }

    /*
        Remove the route prefix from the start of the compiled pattern.
     */
    if (route->prefix && *route->prefix && sstarts(startPattern, route->prefix)) {
        assert(route->prefixLen <= route->startWithLen);
        startPattern = sfmt("^%s", &startPattern[route->prefixLen]);
    } else {
        startPattern = sjoin("^", startPattern, NULL);
    }
    for (cp = startPattern; *cp; cp++) {
        /* Alias for optional, non-capturing pattern:  "(?: PAT )?" */
        if (*cp == '(' && cp[1] == '~') {
            mprPutStringToBuf(pattern, "(?:");
            cp++;

        } else if (*cp == '(') {
            mprPutCharToBuf(pattern, *cp);
        } else if (*cp == '~' && cp[1] == ')') {
            mprPutStringToBuf(pattern, ")?");
            cp++;

        } else if (*cp == ')') {
            mprPutCharToBuf(pattern, *cp);

        } else if (*cp == '{') {
            if (cp > startPattern&& cp[-1] == '\\') {
                mprAdjustBufEnd(pattern, -1);
                mprPutCharToBuf(pattern, *cp);
            } else {
                if ((ep = schr(cp, '}')) != 0) {
                    /* Trim {} off the token and replace in pattern with "([^/]*)"  */
                    token = snclone(&cp[1], ep - cp - 1);
                    if ((field = schr(token, '=')) != 0) {
                        *field++ = '\0';
                        field = sfmt("(%s)", field);
                    } else {
                        field = "([^/]*)";
                    }
                    mprPutStringToBuf(pattern, field);
                    mprAddItem(route->tokens, token);
                    /* Params ends up looking like "$1:$2:$3:$4" */
                    cp = ep;
                }
            }
        } else if (*cp == '\\' && *cp == '~') {
            mprPutCharToBuf(pattern, *++cp);

        } else {
            mprPutCharToBuf(pattern, *cp);
        }
    }
    mprAddNullToBuf(pattern);
    route->optimizedPattern = sclone(mprGetBufStart(pattern));
    if (mprGetListLength(route->tokens) == 0) {
        route->tokens = 0;
    }
    if (route->patternCompiled && (route->flags & HTTP_ROUTE_FREE_PATTERN)) {
        free(route->patternCompiled);
    }
    if ((route->patternCompiled = pcre_compile2(route->optimizedPattern, 0, 0, &errMsg, &column, NULL)) == 0) {
        mprLog("error http route", 0, "Cannot compile route. Error %s at column %d", errMsg, column);
    }
    route->flags |= HTTP_ROUTE_FREE_PATTERN;
}


static char *finalizeReplacement(HttpRoute *route, cchar *str)
{
    MprBuf      *buf;
    cchar       *item;
    cchar       *tok, *cp, *ep, *token;
    int         next, braced;

    assert(route);

    /*
        Prepare a replacement string. Change $token to $N
     */
    buf = mprCreateBuf(-1, -1);
    if (str && *str) {
        for (cp = str; *cp; cp++) {
            if ((tok = schr(cp, '$')) != 0 && (tok == str || tok[-1] != '\\')) {
                if (tok > cp) {
                    mprPutBlockToBuf(buf, cp, tok - cp);
                }
                if ((braced = (*++tok == '{')) != 0) {
                    tok++;
                }
                if (*tok == '&' || *tok == '\'' || *tok == '`' || *tok == '$') {
                    mprPutCharToBuf(buf, '$');
                    mprPutCharToBuf(buf, *tok);
                    ep = tok + 1;
                } else {
                    if (braced) {
                        for (ep = tok; *ep && *ep != '}'; ep++) ;
                    } else {
                        for (ep = tok; *ep && isdigit((uchar) *ep); ep++) ;
                    }
                    token = snclone(tok, ep - tok);
                    if (schr(token, ':') || schr(token, '.')) {
                        /* Double quote to get through two levels of expansion in writeTarget */
                        mprPutStringToBuf(buf, "$${");
                        mprPutStringToBuf(buf, token);
                        mprPutCharToBuf(buf, '}');
                    } else {
                        for (next = 0; (item = mprGetNextItem(route->tokens, &next)) != 0; ) {
                            if (scmp(item, token) == 0) {
                                break;
                            }
                        }
                        /*  Insert "$" in front of "{token}" */
                        if (item) {
                            mprPutCharToBuf(buf, '$');
                            mprPutIntToBuf(buf, next);
                        } else if (snumber(token)) {
                            mprPutCharToBuf(buf, '$');
                            mprPutStringToBuf(buf, token);
                        } else {
                            mprLog("error http route", 0, "Cannot find token \"%s\" in template \"%s\"", 
                                token, route->pattern);
                        }
                    }
                }
                if (braced) {
                    ep++;
                }
                cp = ep - 1;

            } else {
                if (*cp == '\\') {
                    if (cp[1] == 'r') {
                        mprPutCharToBuf(buf, '\r');
                        cp++;
                    } else if (cp[1] == 'n') {
                        mprPutCharToBuf(buf, '\n');
                        cp++;
                    } else {
                        mprPutCharToBuf(buf, *cp);
                    }
                } else {
                    mprPutCharToBuf(buf, *cp);
                }
            }
        }
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


/*
    Convert a route pattern into a usable template to construct URI links
    NOTE: this is heuristic and not perfect. Users can define the template via the httpSetTemplate API or in appweb via the
    EspURITemplate configuration directive.
 */
static char *finalizeTemplate(HttpRoute *route)
{
    MprBuf  *buf;
    char    *sp, *tplate;

    if ((buf = mprCreateBuf(0, 0)) == 0) {
        return 0;
    }
    /*
        Note: the route->pattern includes the prefix
     */
    for (sp = route->pattern; *sp; sp++) {
        switch (*sp) {
        default:
            mprPutCharToBuf(buf, *sp);
            break;
        case '$':
            if (sp[1]) {
                mprPutCharToBuf(buf, *sp);
            }
            break;
        case '^':
            if (sp > route->pattern) {
                mprPutCharToBuf(buf, *sp);
            }
            break;
        case '+':
        case '?':
        case '|':
        case '[':
        case ']':
        case '*':
        case '.':
            break;
        case '(':
            if (sp[1] == '~') {
                sp++;
            }
            break;
        case '~':
            if (sp[1] == ')') {
                sp++;
            } else {
                mprPutCharToBuf(buf, *sp);
            }
            break;
        case ')':
            break;
        case '\\':
            if (sp[1] == '\\') {
                mprPutCharToBuf(buf, *sp++);
            } else {
                mprPutCharToBuf(buf, *++sp);
            }
            break;
        case '{':
            mprPutCharToBuf(buf, '$');
            while (sp[1] && *sp != '}') {
                if (*sp == '=') {
                    while (sp[1] && *sp != '}') sp++;
                } else {
                    mprPutCharToBuf(buf, *sp++);
                }
            }
            mprPutCharToBuf(buf, '}');
            break;
        }
    }
    if (mprLookAtLastCharInBuf(buf) == '/') {
        mprAdjustBufEnd(buf, -1);
    }
    mprAddNullToBuf(buf);
    if (mprGetBufLength(buf) > 0) {
        tplate = sclone(mprGetBufStart(buf));
    } else {
        tplate = sclone("/");
    }
    return tplate;
}


PUBLIC void httpFinalizeRoute(HttpRoute *route)
{
    /*
        Add the route to the owning host. When using an Appweb configuration file, the order of route finalization 
        will be from the inside out. This ensures that nested routes are defined BEFORE outer/enclosing routes.
        This is important as requests process routes in-order.
     */
    assert(route);
    if (mprGetListLength(route->indexes) == 0) {
        mprAddItem(route->indexes,  sclone("index.html"));
    }
    httpAddRoute(route->host, route);
}


/*
    Expect a template with embedded tokens of the form: "/${controller}/${action}/${other}"
    Understands the following aliases:
        ~   For ${PREFIX}
        ^   For ${PREFIX}${SERVER_PREFIX}
    The options is a hash of token values.
 */
PUBLIC char *httpTemplate(HttpConn *conn, cchar *template, MprHash *options)
{
    MprBuf      *buf;
    HttpRx      *rx;
    HttpRoute   *route;
    cchar       *cp, *ep, *value;
    char        key[ME_MAX_BUFFER];

    rx = conn->rx;
    route = rx->route;
    if (template == 0 || *template == '\0') {
        return MPR->emptyString;
    }
    buf = mprCreateBuf(-1, -1);
    for (cp = template; *cp; cp++) {
        if (cp == template && *cp == '~') {
            mprPutStringToBuf(buf, route->prefix);

        } else if (cp == template && *cp == ME_SERVER_PREFIX_CHAR) {
            mprPutStringToBuf(buf, route->prefix);
            mprPutStringToBuf(buf, route->serverPrefix);

        } else if (*cp == '$' && cp[1] == '{' && (cp == template || cp[-1] != '\\')) {
            cp += 2;
            if ((ep = strchr(cp, '}')) != 0) {
                sncopy(key, sizeof(key), cp, ep - cp);
                if (options && (value = httpGetOption(options, key, 0)) != 0) {
                    mprPutStringToBuf(buf, value);

                } else if ((value = mprLookupJson(rx->params, key)) != 0) {
                    mprPutStringToBuf(buf, value);
                }
                if (value == 0) {
                    /* Just emit the token name if the token cannot be found */
                    mprPutStringToBuf(buf, key);
                }
                cp = ep;
            }
        } else {
            mprPutCharToBuf(buf, *cp);
        }
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


PUBLIC void httpSetRouteVar(HttpRoute *route, cchar *key, cchar *value)
{
    assert(route);
    assert(key);

    GRADUATE_HASH(route, vars);
    if (schr(value, '$')) {
        value = stemplate(value, route->vars);
    }
    mprAddKey(route->vars, key, sclone(value));
}


PUBLIC cchar *httpGetRouteVar(HttpRoute *route, cchar *key)
{
    return mprLookupKey(route->vars, key);
}


PUBLIC cchar *httpExpandRouteVars(HttpRoute *route, cchar *str)
{
    return stemplate(str, route->vars);
}


/*
    Make a path name. This replaces $references, converts to an absolute path name, cleans the path and maps delimiters.
    Paths are resolved relative to the given directory or route home if "dir" is null.
 */
PUBLIC char *httpMakePath(HttpRoute *route, cchar *dir, cchar *path)
{
    assert(route);
    assert(path);

    if ((path = stemplate(path, route->vars)) == 0) {
        return 0;
    }
    if (mprIsPathRel(path)) {
        path = mprJoinPath(dir ? dir : route->home, path);
    }
    return mprGetAbsPath(path);
}


PUBLIC void httpSetRouteXsrf(HttpRoute *route, bool enable)
{
    route->flags &= ~HTTP_ROUTE_XSRF;
    if (enable) {
        route->flags |= HTTP_ROUTE_XSRF;
    }
}

/********************************* Language ***********************************/
/*
    Language can be an empty string
 */
PUBLIC int httpAddRouteLanguageSuffix(HttpRoute *route, cchar *language, cchar *suffix, int flags)
{
    HttpLang    *lp;

    assert(route);
    assert(language);
    assert(suffix && *suffix);

    if (route->languages == 0) {
        route->languages = mprCreateHash(-1, MPR_HASH_STABLE);
    } else {
        GRADUATE_HASH(route, languages);
    }
    if ((lp = mprLookupKey(route->languages, language)) != 0) {
        lp->suffix = sclone(suffix);
        lp->flags = flags;
    } else {
        mprAddKey(route->languages, language, createLangDef(0, suffix, flags));
    }
    return httpAddRouteUpdate(route, "lang", 0, 0);
}


PUBLIC int httpAddRouteLanguageDir(HttpRoute *route, cchar *language, cchar *path)
{
    HttpLang    *lp;

    assert(route);
    assert(language && *language);
    assert(path && *path);

    if (route->languages == 0) {
        route->languages = mprCreateHash(-1, MPR_HASH_STABLE);
    } else {
        GRADUATE_HASH(route, languages);
    }
    if ((lp = mprLookupKey(route->languages, language)) != 0) {
        lp->path = sclone(path);
    } else {
        mprAddKey(route->languages, language, createLangDef(path, 0, 0));
    }
    return httpAddRouteUpdate(route, "lang", 0, 0);
}


PUBLIC void httpSetRouteDefaultLanguage(HttpRoute *route, cchar *language)
{
    assert(route);
    assert(language && *language);

    route->defaultLanguage = sclone(language);
}


/********************************* Conditions *********************************/

static int testCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *condition)
{
    HttpRouteProc   *proc;

    assert(conn);
    assert(route);
    assert(condition);

    if ((proc = mprLookupKey(conn->http->routeConditions, condition->name)) == 0) {
        httpError(conn, -1, "Cannot find route condition rule %s", condition->name);
        return 0;
    }
    return (*proc)(conn, route, condition);
}


/*
    Allow/Deny authorization
 */
static int allowDenyCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpRx      *rx;
    HttpAuth    *auth;
    int         allow, deny;

    assert(conn);
    assert(route);

    rx = conn->rx;
    auth = rx->route->auth;
    if (auth == 0) {
        return HTTP_ROUTE_OK;
    }
    allow = 0;
    deny = 0;
    if (auth->flags & HTTP_ALLOW_DENY) {
        if (auth->allow && mprLookupKey(auth->allow, conn->ip)) {
            allow++;
        } else {
            allow++;
        }
        if (auth->deny && mprLookupKey(auth->deny, conn->ip)) {
            deny++;
        }
        if (!allow || deny) {
            httpError(conn, HTTP_CODE_FORBIDDEN, "Access denied for this server %s", conn->ip);
            return HTTP_ROUTE_OK;
        }
    } else {
        if (auth->deny && mprLookupKey(auth->deny, conn->ip)) {
            deny++;
        }
        if (auth->allow && !mprLookupKey(auth->allow, conn->ip)) {
            deny = 0;
            allow++;
        } else {
            allow++;
        }
        if (deny || !allow) {
            httpError(conn, HTTP_CODE_FORBIDDEN, "Access denied for this server %s", conn->ip);
            return HTTP_ROUTE_OK;
        }
    }
    return HTTP_ROUTE_OK;
}


/*
    This condition is used to implement all user authentication for routes
 */
static int authCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpAuth    *auth;
    cchar       *username, *password;

    assert(conn);
    assert(route);

    auth = route->auth;
    if (!auth || !auth->type) {
        /* Authentication not required */
        return HTTP_ROUTE_OK;
    }
    if (!httpLoggedIn(conn)) {
        httpGetCredentials(conn, &username, &password);
        if (!httpLogin(conn, username, password)) {
            if (!conn->tx->finalized) {
                if (auth && auth->type) {
                    (auth->type->askLogin)(conn);
                } else {
                    httpError(conn, HTTP_CODE_UNAUTHORIZED, "Access Denied, login required");
                }
                /* Request has been denied and a response generated. So OK to accept this route. */
            }
            return HTTP_ROUTE_OK;
        }
    }
    if (!httpCanUser(conn, NULL)) {
        httpTrace(conn, "auth.check", "error", "msg:'Access denied, user is not authorized for access'");
        if (!conn->tx->finalized) {
            httpError(conn, HTTP_CODE_FORBIDDEN, "Access denied. User is not authorized for access.");
            /* Request has been denied and a response generated. So OK to accept this route. */
        }
    }
    /* OK to accept route. This does not mean the request was authenticated - an error may have been already generated */
    return HTTP_ROUTE_OK;
}


/*
    This condition is used for "Condition unauthorized"
 */
static int unauthorizedCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpAuth    *auth;
    cchar       *username, *password;

    auth = route->auth;
    if (!auth || !auth->type) {
        return HTTP_ROUTE_REJECT;
    }
    if (httpLoggedIn(conn)) {
        return HTTP_ROUTE_REJECT;
    }
    httpGetCredentials(conn, &username, &password);
    if (httpLogin(conn, username, password)) {
        return HTTP_ROUTE_REJECT;
    }
    return HTTP_ROUTE_OK;
}


static int directoryCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpTx      *tx;
    MprPath     info;
    char        *path;

    assert(conn);
    assert(route);
    assert(op);

    /* 
        Must have tx->filename set when expanding op->details, so map target now 
     */
    tx = conn->tx;
    httpMapFile(conn);
    path = mprJoinPath(route->documents, expandTokens(conn, op->details));
    tx->ext = tx->filename = 0;
    mprGetPathInfo(path, &info);
    if (info.isDir) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


/*
    Test if a file exists
 */
static int existsCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpTx  *tx;
    char    *path;

    assert(conn);
    assert(route);
    assert(op);

    /* 
        Must have tx->filename set when expanding op->details, so map target now 
     */
    tx = conn->tx;
    httpMapFile(conn);
    path = mprJoinPath(route->documents, expandTokens(conn, op->details));
    tx->ext = tx->filename = 0;
    if (mprPathExists(path, R_OK)) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


static int matchCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    char    *str;
    int     matched[ME_MAX_ROUTE_MATCHES * 2], count;

    assert(conn);
    assert(route);
    assert(op);

    str = expandTokens(conn, op->details);
    count = pcre_exec(op->mdata, NULL, str, (int) slen(str), 0, 0, matched, sizeof(matched) / sizeof(int));
    if (count > 0) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


/*
    Test if the connection is secure
    Set op->details to a non-zero "age" to emit a Strict-Transport-Security header
    A negative age signifies to "includeSubDomains"
 */
static int secureCondition(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    int64       age;

    assert(conn);
    if (op->flags & HTTP_ROUTE_STRICT_TLS) {
        /* Negative age means subDomains == true */
        age = stoi(op->details);
        if (age < 0) {
            httpAddHeader(conn, "Strict-Transport-Security", "max-age=%lld; includeSubDomains", -age / MPR_TICKS_PER_SEC);
        } else if (age > 0) {
            httpAddHeader(conn, "Strict-Transport-Security", "max-age=%lld", age / MPR_TICKS_PER_SEC);
        }
    }
    if (op->flags & HTTP_ROUTE_REDIRECT) {
        if (!conn->secure) {
            assert(op->details && *op->details);
            httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, expandTokens(conn, op->details));
        }
        return HTTP_ROUTE_OK;
    }
    if (!conn->secure) {
        return HTTP_ROUTE_REJECT;
    }
    return HTTP_ROUTE_OK;
}

/********************************* Updates ******************************/

static int updateRequest(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpRouteProc   *proc;

    assert(conn);
    assert(route);
    assert(op);

    if ((proc = mprLookupKey(conn->http->routeUpdates, op->name)) == 0) {
        httpError(conn, -1, "Cannot find route update rule %s", op->name);
        return HTTP_ROUTE_OK;
    }
    return (*proc)(conn, route, op);
}


static int cmdUpdate(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    MprCmd  *cmd;
    char    *command, *out, *err;
    int     status;

    assert(conn);
    assert(route);
    assert(op);

    command = expandTokens(conn, op->details);
    cmd = mprCreateCmd(conn->dispatcher);
    httpTrace(conn, "request.run", "context", "command:'%s'", command);
    if ((status = mprRunCmd(cmd, command, NULL, NULL, &out, &err, -1, 0)) != 0) {
        /* Don't call httpError, just set errorMsg which can be retrieved via: ${request:error} */
        conn->errorMsg = sfmt("Command failed: %s\nStatus: %d\n%s\n%s", command, status, out, err);
        httpTrace(conn, "request.run.error", "error", "command:'%s',error:'%s'", command, conn->errorMsg);
        /* Continue */
    }
    return HTTP_ROUTE_OK;
}


static int paramUpdate(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    assert(conn);
    assert(route);
    assert(op);

    httpSetParam(conn, op->var, expandTokens(conn, op->value));
    return HTTP_ROUTE_OK;
}


static int langUpdate(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    HttpUri     *prior;
    HttpRx      *rx;
    HttpLang    *lang;
    char        *ext, *pathInfo, *uri;

    assert(conn);
    assert(route);

    rx = conn->rx;
    prior = rx->parsedUri;
    assert(route->languages);

    if ((lang = httpGetLanguage(conn, route->languages, 0)) != 0) {
        rx->lang = lang;
        if (lang->suffix) {
            pathInfo = 0;
            if (lang->flags & HTTP_LANG_AFTER) {
                pathInfo = sjoin(rx->pathInfo, ".", lang->suffix, NULL);
            } else if (lang->flags & HTTP_LANG_BEFORE) {
                ext = httpGetExt(conn);
                if (ext && *ext) {
                    pathInfo = sjoin(mprJoinPathExt(mprTrimPathExt(rx->pathInfo), lang->suffix), ".", ext, NULL);
                } else {
                    pathInfo = mprJoinPathExt(mprTrimPathExt(rx->pathInfo), lang->suffix);
                }
            }
            if (pathInfo) {
                uri = httpFormatUri(prior->scheme, prior->host, prior->port, pathInfo, prior->reference, prior->query, 0);
                httpSetUri(conn, uri);
            }
        }
    }
    return HTTP_ROUTE_OK;
}


/*********************************** Targets **********************************/

static int closeTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    assert(conn);
    assert(route);

    httpError(conn, HTTP_CODE_RESET | HTTP_ABORT, "Route target \"close\" is closing request");
    return HTTP_ROUTE_OK;
}


static int redirectTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    cchar       *target;

    assert(conn);
    assert(route);
    assert(route->target);

    target = expandTokens(conn, route->target);
    httpRedirect(conn, route->responseStatus ? route->responseStatus : HTTP_CODE_MOVED_TEMPORARILY, target);
    return HTTP_ROUTE_OK;
}


static int runTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    /*
        Need to re-compute output string as updates may have run to define params which affect the route->target tokens
     */
    conn->rx->target = route->target ? expandTokens(conn, route->target) : sclone(&conn->rx->pathInfo[1]);
    return HTTP_ROUTE_OK;
}


static int writeTarget(HttpConn *conn, HttpRoute *route, HttpRouteOp *op)
{
    char    *str;

    assert(conn);
    assert(route);

    /*
        Need to re-compute output string as updates may have run to define params which affect the route->target tokens
     */
    str = route->target ? expandTokens(conn, route->target) : sclone(&conn->rx->pathInfo[1]);
    if (!(route->flags & HTTP_ROUTE_RAW)) {
        str = mprEscapeHtml(str);
    }
    httpSetStatus(conn, route->responseStatus);
    httpFormatResponse(conn, "%s", str);
    httpFinalize(conn);
    return HTTP_ROUTE_OK;
}


/************************************************** Route Convenience ****************************************************/

PUBLIC HttpRoute *httpDefineRoute(HttpRoute *parent, cchar *name, cchar *methods, cchar *pattern, cchar *target, cchar *source)
{
    HttpRoute   *route;

    if ((route = httpCreateInheritedRoute(parent)) == 0) {
        return 0;
    }
    if (name) {
        httpSetRouteName(route, name);
    }
    httpSetRoutePattern(route, pattern, 0);
    if (methods) {
        httpSetRouteMethods(route, methods);
    }
    if (source) {
        httpSetRouteSource(route, source);
    }
    httpSetRouteTarget(route, "run", target);
    httpFinalizeRoute(route);
    return route;
}


PUBLIC HttpRoute *httpAddRestfulRoute(HttpRoute *parent, cchar *uprefix, cchar *action, cchar *methods, cchar *pattern, 
        cchar *target, cchar *resource)
{
    cchar       *name, *nameResource, *source, *routePrefix;

    routePrefix = parent->prefix;
    if (!uprefix) {
        uprefix = parent->serverPrefix;
    }
    nameResource = smatch(resource, "{controller}") ? "*" : resource;
    name = sfmt("%s%s/%s/%s", routePrefix, uprefix, nameResource, action);
    if (*resource == '{') {
        pattern = sfmt("^%s%s/%s%s", routePrefix, uprefix, resource, pattern);
    } else {
        pattern = sfmt("^%s%s/{controller=%s}%s", routePrefix, uprefix, resource, pattern);
    }
    if (*resource == '{') {
        target = sfmt("$%s-%s", resource, target);
        source = sfmt("$%s.c", resource);
    } else {
        target = sfmt("%s-%s", resource, target);
        source = sfmt("%s.c", resource);
    }
    return httpDefineRoute(parent, name, methods, pattern, target, source);
}


PUBLIC void httpAddResourceGroup(HttpRoute *parent, cchar *uprefix, cchar *resource)
{
    /* Delete is a POST method alternative to remove */
    httpAddRestfulRoute(parent, uprefix, "delete",    "POST",    "/{id=[0-9]+}/delete$",        "delete",          resource);
    httpAddRestfulRoute(parent, uprefix, "create",    "POST",    "(/)*$",                       "create",          resource);
    httpAddRestfulRoute(parent, uprefix, "edit",      "GET",     "/{id=[0-9]+}/edit$",          "edit",            resource);
    httpAddRestfulRoute(parent, uprefix, "get",       "GET",     "/{id=[0-9]+}$",               "get",             resource);
    httpAddRestfulRoute(parent, uprefix, "init",      "GET",     "/init$",                      "init",            resource);
    httpAddRestfulRoute(parent, uprefix, "list",      "GET",     "/list$",                      "list",            resource);
    httpAddRestfulRoute(parent, uprefix, "remove",    "DELETE",  "/{id=[0-9]+}$",               "remove",          resource);
    httpAddRestfulRoute(parent, uprefix, "update",    "POST",    "/{id=[0-9]+}$",               "update",          resource);
    httpAddRestfulRoute(parent, uprefix, "action",    "GET,POST","/{id=[0-9]+}/{action}(/)*$",  "${action}",       resource);
    httpAddRestfulRoute(parent, uprefix, "default",   "GET,POST","/{action}(/)*$",              "cmd-${action}",   resource);
}


PUBLIC void httpAddResource(HttpRoute *parent, cchar *uprefix, cchar *resource)
{
    /* Delete is a POST method alternative to remove */
    httpAddRestfulRoute(parent, uprefix, "delete",    "POST",    "/delete$",       "delete",     resource);
    httpAddRestfulRoute(parent, uprefix, "create",    "POST",    "(/)*$",          "create",     resource);
    httpAddRestfulRoute(parent, uprefix, "edit",      "GET",     "/edit$",         "edit",       resource);
    httpAddRestfulRoute(parent, uprefix, "get",       "GET",     "(/)*$",          "get",        resource);
    httpAddRestfulRoute(parent, uprefix, "init",      "GET",     "/init$",         "init",       resource);
    httpAddRestfulRoute(parent, uprefix, "update",    "POST",    "(/)*$",          "update",     resource);
    httpAddRestfulRoute(parent, uprefix, "remove",    "DELETE",  "(/)*$",          "remove",     resource);
    httpAddRestfulRoute(parent, uprefix, "default",   "GET,POST","/{action}(/)*$", "${action}",  resource);
}


/*
    Add routes for a permanent resource. Cannot create or remove.
 */
PUBLIC void httpAddPermResource(HttpRoute *parent, cchar *uprefix, cchar *resource)
{
    httpAddRestfulRoute(parent, uprefix, "get",       "GET",     "(/)*$",          "get",        resource);
    httpAddRestfulRoute(parent, uprefix, "update",    "POST",    "(/)*$",          "update",     resource);
    httpAddRestfulRoute(parent, uprefix, "default",   "GET,POST","/{action}(/)*$", "${action}",  resource);
}


PUBLIC void httpAddClientRoute(HttpRoute *parent, cchar *uprefix, cchar *name)
{
    HttpRoute   *route;
    cchar       *path, *pattern;

    if (!uprefix) {
        uprefix = "";
    }
    if (parent->prefix) {
        uprefix = sjoin(parent->prefix, uprefix, NULL);
        name = sjoin(parent->prefix, name, NULL);
    }
    pattern = sfmt("^%s(/.*)", uprefix);
    path = sjoin(mprGetRelPath(stemplate("${CLIENT_DIR}", parent->vars), parent->documents), "$1", NULL);
    route = httpDefineRoute(parent, name, "GET", pattern, path, parent->sourceName);
    httpAddRouteHandler(route, "fileHandler", "");
}


PUBLIC void httpAddHomeRoute(HttpRoute *parent)
{
    cchar   *source, *name, *path, *pattern;

    source = parent->sourceName;
    name = sjoin(parent->prefix, "/home", NULL);
    path = stemplate("${CLIENT_DIR}/index.esp", parent->vars);
    pattern = sfmt("^%s(/)$", parent->prefix);
    httpDefineRoute(parent, name, "GET,POST", pattern, path, source);
}


PUBLIC HttpRoute *httpAddWebSocketsRoute(HttpRoute *parent, cchar *uprefix, cchar *name)
{
    HttpRoute   *route;
    cchar       *pattern;

    if (!uprefix) {
        uprefix = parent->serverPrefix;
    }
    if (parent->prefix) {
        name = sjoin(parent->prefix, name, NULL);
    }
    pattern = sfmt("^%s%s/{controller}/stream", parent->prefix, uprefix);
    route = httpDefineRoute(parent, name, "GET", pattern, "$1-cmd-stream", "${controller}.c");
    httpAddRouteFilter(route, "webSocketFilter", "", HTTP_STAGE_RX | HTTP_STAGE_TX);

    httpGraduateLimits(route, 0);
    /*
        Set some reasonable defaults. 5 minutes for inactivity and no request timeout limit
     */
    route->limits->inactivityTimeout = ME_MAX_INACTIVITY_DURATION * 10;
    route->limits->requestTimeout = MPR_MAX_TIMEOUT;
    return route;
}

/*************************************************** Support Routines ****************************************************/
/*
    Route operations are used per-route for headers and fields
 */
static HttpRouteOp *createRouteOp(cchar *name, int flags)
{
    HttpRouteOp   *op;

    assert(name && *name);

    if ((op = mprAllocObj(HttpRouteOp, manageRouteOp)) == 0) {
        return 0;
    }
    op->name = sclone(name);
    op->flags = flags;
    return op;
}


static void manageRouteOp(HttpRouteOp *op, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(op->name);
        mprMark(op->details);
        mprMark(op->var);
        mprMark(op->value);

    } else if (flags & MPR_MANAGE_FREE) {
        if (op->flags & HTTP_ROUTE_FREE) {
            free(op->mdata);
        }
    }
}


static bool opPresent(MprList *list, HttpRouteOp *op)
{
    HttpRouteOp   *last;

    if ((last = mprGetLastItem(list)) == 0) {
        return 0;
    }
    if (smatch(last->name, op->name) && 
        smatch(last->details, op->details) && 
        smatch(last->var, op->var) && 
        smatch(last->value, op->value) && 
        last->mdata == op->mdata && 
        last->flags == op->flags) {
        return 1;
    }
    return 0;
}


static void addUniqueItem(MprList *list, HttpRouteOp *op)
{
    int     index;

    assert(list);
    assert(op);

    if (!opPresent(list, op)) {
        index = smatch(op->name, "secure") ? 0 : list->length;
        mprInsertItemAtPos(list, index, op);
    }
}


static HttpLang *createLangDef(cchar *path, cchar *suffix, int flags)
{
    HttpLang    *lang;

    if ((lang = mprAllocObj(HttpLang, manageLang)) == 0) {
        return 0;
    }
    if (path) {
        lang->path = sclone(path);
    }
    if (suffix) {
        lang->suffix = sclone(suffix);
    }
    lang->flags = flags;
    return lang;
}


static void manageLang(HttpLang *lang, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(lang->path);
        mprMark(lang->suffix);
    }
}


static void definePathVars(HttpRoute *route)
{
    assert(route);

    mprAddKey(route->vars, "PRODUCT", sclone(ME_NAME));
    mprAddKey(route->vars, "OS", sclone(ME_OS));
    mprAddKey(route->vars, "VERSION", sclone(ME_VERSION));
    mprAddKey(route->vars, "PLATFORM", sclone(ME_PLATFORM));
    mprAddKey(route->vars, "BIN_DIR", mprGetAppDir());
    if (route->host) {
        defineHostVars(route);
    }
}


static void defineHostVars(HttpRoute *route) 
{
    assert(route);
    mprAddKey(route->vars, "DOCUMENTS", route->documents);
    mprAddKey(route->vars, "HOME", route->home);
    mprAddKey(route->vars, "HOST", route->host->name);
#if DEPRECATED || 1
    mprAddKey(route->vars, "SERVER_NAME", route->host->name);
#endif
}


static char *expandTokens(HttpConn *conn, cchar *str)
{
    HttpRx      *rx;

    assert(conn);
    assert(str);

    rx = conn->rx;
    return expandRequestTokens(conn, expandPatternTokens(rx->pathInfo, str, rx->matches, rx->matchCount));
}


/*
    WARNING: str is modified. Result is allocated string.
 */
static char *expandRequestTokens(HttpConn *conn, char *str)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    MprBuf      *buf;
    HttpLang    *lang;
    char        *tok, *cp, *key, *value, *field, *header, *defaultValue, *state, *v, *p;

    assert(conn);
    assert(str);

    rx = conn->rx;
    route = rx->route;
    tx = conn->tx;
    buf = mprCreateBuf(-1, -1);
    tok = 0;

    for (cp = str; cp && *cp; ) {
        if ((tok = strstr(cp, "${")) == 0) {
            break;
        }
        if (tok > cp) {
            mprPutBlockToBuf(buf, cp, tok - cp);
        }
        if ((key = stok(&tok[2], ".:}", &value)) == 0) {
            continue;
        }
        stok(value, "}", &cp);

        if (smatch(key, "header")) {
            header = stok(value, "=", &defaultValue);
            if ((value = (char*) httpGetHeader(conn, header)) == 0) {
                value = defaultValue ? defaultValue : "";
            }
            mprPutStringToBuf(buf, value);

        } else if (smatch(key, "param")) {
            field = stok(value, "=", &defaultValue);
            if (defaultValue == 0) {
                defaultValue = "";
            }
            mprPutStringToBuf(buf, httpGetParam(conn, field, defaultValue));

        } else if (smatch(key, "request")) {
            value = stok(value, "=", &defaultValue);
            //  OPT with switch on first char

            if (smatch(value, "authenticated")) {
                mprPutStringToBuf(buf, rx->authenticated ? "true" : "false");

            } else if (smatch(value, "clientAddress")) {
                mprPutStringToBuf(buf, conn->ip);

            } else if (smatch(value, "clientPort")) {
                mprPutIntToBuf(buf, conn->port);

            } else if (smatch(value, "error")) {
                mprPutStringToBuf(buf, conn->errorMsg);

            } else if (smatch(value, "ext")) {
                mprPutStringToBuf(buf, rx->parsedUri->ext);

            } else if (smatch(value, "extraPath")) {
                mprPutStringToBuf(buf, rx->extraPath);

            } else if (smatch(value, "filename")) {
                mprPutStringToBuf(buf, tx->filename);

            } else if (scaselessmatch(value, "language")) {
                if (!defaultValue) {
                    defaultValue = route->defaultLanguage;
                }
                if ((lang = httpGetLanguage(conn, route->languages, defaultValue)) != 0) {
                    mprPutStringToBuf(buf, lang->suffix);
                } else {
                    mprPutStringToBuf(buf, defaultValue);
                }

            } else if (scaselessmatch(value, "languageDir")) {
                lang = httpGetLanguage(conn, route->languages, 0);
                if (!defaultValue) {
                    defaultValue = ".";
                }
                mprPutStringToBuf(buf, lang ? lang->path : defaultValue);

            } else if (smatch(value, "host")) {
                /* Includes port if present */
                mprPutStringToBuf(buf, rx->parsedUri->host);

            } else if (smatch(value, "method")) {
                mprPutStringToBuf(buf, rx->method);

            } else if (smatch(value, "originalUri")) {
                mprPutStringToBuf(buf, rx->originalUri);

            } else if (smatch(value, "pathInfo")) {
                mprPutStringToBuf(buf, rx->pathInfo);

            } else if (smatch(value, "prefix")) {
                mprPutStringToBuf(buf, route->prefix);

            } else if (smatch(value, "query")) {
                mprPutStringToBuf(buf, rx->parsedUri->query);

            } else if (smatch(value, "reference")) {
                mprPutStringToBuf(buf, rx->parsedUri->reference);

            } else if (smatch(value, "scheme")) {
                if (rx->parsedUri->scheme) {
                    mprPutStringToBuf(buf, rx->parsedUri->scheme);
                }  else {
                    mprPutStringToBuf(buf, (conn->secure) ? "https" : "http");
                }

            } else if (smatch(value, "scriptName")) {
                mprPutStringToBuf(buf, rx->scriptName);

            } else if (smatch(value, "serverAddress")) {
                /* Pure IP address, no port. See "serverPort" */
                mprPutStringToBuf(buf, conn->sock->acceptIp);

            } else if (smatch(value, "serverPort")) {
                mprPutIntToBuf(buf, conn->sock->acceptPort);

            } else if (smatch(value, "uri")) {
                mprPutStringToBuf(buf, rx->uri);
            }
        } else if (smatch(key, "ssl")) {
            value = stok(value, "=", &defaultValue);
            if (smatch(value, "state")) {
                mprPutStringToBuf(buf, mprGetSocketState(conn->sock));
            } else {
                state = mprGetSocketState(conn->sock);
                if ((p = scontains(state, value)) != 0) {
                    stok(p, "=", &v);
                    mprPutStringToBuf(buf, stok(v, ", ", NULL));
                }
            }
        }
    }
    assert(cp);
    if (tok) {
        if (tok > cp) {
            mprPutBlockToBuf(buf, tok, tok - cp);
        }
    } else {
        mprPutStringToBuf(buf, cp);
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


PUBLIC char *httpExpandUri(HttpConn *conn, cchar *str)
{
    return expandRequestTokens(conn, stemplate(str, conn->rx->route->vars));
}


/*
    Replace text using pcre regular expression match indexes
 */
static char *expandPatternTokens(cchar *str, cchar *replacement, int *matches, int matchCount)
{
    MprBuf  *result;
    cchar   *end, *cp, *lastReplace;
    int     submatch;

    assert(str);
    assert(replacement);
    assert(matches);

    result = mprCreateBuf(-1, -1);
    lastReplace = replacement;
    end = &replacement[slen(replacement)];

    for (cp = replacement; cp < end; ) {
        if (*cp == '$') {
            if (lastReplace < cp) {
                mprPutSubStringToBuf(result, lastReplace, (int) (cp - lastReplace));
            }
            switch (*++cp) {
            case '$':
                mprPutCharToBuf(result, '$');
                break;
            case '&':
                /* Replace the matched string */
                if (matchCount > 0) {
                    mprPutSubStringToBuf(result, &str[matches[0]], matches[1] - matches[0]);
                }
                break;
            case '`':
                /* Insert the portion that preceeds the matched string */
                if (matchCount > 0) {
                    mprPutSubStringToBuf(result, str, matches[0]);
                }
                break;
            case '\'':
                /* Insert the portion that follows the matched string */
                if (matchCount > 0) {
                    mprPutSubStringToBuf(result, &str[matches[1]], slen(str) - matches[1]);
                }
                break;
            default:
                /* Insert the nth submatch */
                if (isdigit((uchar) *cp)) {
                    submatch = (int) atoi(cp);
                    while (isdigit((uchar) *++cp))
                        ;
                    cp--;
                    if (submatch < matchCount) {
                        submatch *= 2;
                        mprPutSubStringToBuf(result, &str[matches[submatch]], matches[submatch + 1] - matches[submatch]);
                    }
                } else {
                    mprDebug("http route", 5, "Bad replacement $ specification in page");
                    return 0;
                }
            }
            lastReplace = cp + 1;
        }
        cp++;
    }
    if (lastReplace < cp && lastReplace < end) {
        mprPutSubStringToBuf(result, lastReplace, (int) (cp - lastReplace));
    }
    mprAddNullToBuf(result);
    return sclone(mprGetBufStart(result));
}


PUBLIC void httpDefineRouteBuiltins()
{
    /*
        These are the conditions that can be selected. Use httpAddRouteCondition to add to a route.
        The allow and auth conditions are internal and are configured via various Auth APIs.
     */
    httpDefineRouteCondition("allowDeny", allowDenyCondition);
    httpDefineRouteCondition("auth", authCondition);
    httpDefineRouteCondition("directory", directoryCondition);
    httpDefineRouteCondition("exists", existsCondition);
    httpDefineRouteCondition("match", matchCondition);
    httpDefineRouteCondition("secure", secureCondition);
    httpDefineRouteCondition("unauthorized", unauthorizedCondition);

    httpDefineRouteUpdate("param", paramUpdate);
    httpDefineRouteUpdate("cmd", cmdUpdate);
    httpDefineRouteUpdate("lang", langUpdate);

    httpDefineRouteTarget("close", closeTarget);
    httpDefineRouteTarget("redirect", redirectTarget);
    httpDefineRouteTarget("run", runTarget);
    httpDefineRouteTarget("write", writeTarget);
}


/*
    Tokenizes a line using %formats. Mandatory tokens can be specified with %. Optional tokens are specified with ?. 
    Supported tokens:
        %B - Boolean. Parses: on/off, true/false, yes/no.
        %N - Number. Parses numbers in base 10.
        %S - String. Removes quotes.
        %T - Template String. Removes quotes and expand ${PathVars}.
        %P - Path string. Removes quotes and expands ${PathVars}. Resolved relative to host->dir (Home).
        %W - Parse words into a list
        %! - Optional negate. Set value to HTTP_ROUTE_NOT present, otherwise zero.
    Values wrapped in quotes will have the outermost quotes trimmed.
 */
PUBLIC bool httpTokenize(HttpRoute *route, cchar *line, cchar *fmt, ...)
{
    va_list     args;
    bool        rc;

    assert(route);
    assert(line);
    assert(fmt);

    va_start(args, fmt);
    rc =  httpTokenizev(route, line, fmt, args);
    va_end(args);
    return rc;
}


PUBLIC bool httpTokenizev(HttpRoute *route, cchar *line, cchar *fmt, va_list args)
{
    MprList     *list;
    cchar       *f;
    char        *tok, *etok, *value, *word, *end;
    int         quote;

    assert(route);
    assert(fmt);

    if (line == 0) {
        line = "";
    }
    tok = sclone(line);
    end = &tok[slen(line)];

    for (f = fmt; *f && tok < end; f++) {
        for (; isspace((uchar) *tok); tok++) ;
        if (*tok == '\0' || *tok == '#') {
            break;
        }
        if (isspace((uchar) *f)) {
            continue;
        }
        if (*f == '%' || *f == '?') {
            f++;
            quote = 0;
            if (*f != '*' && (*tok == '"' || *tok == '\'')) {
                quote = *tok++;
            }
            if (*f == '!') {
                etok = &tok[1];
            } else {
                if (quote) {
                    for (etok = tok; *etok && !(*etok == quote && etok[-1] != '\\'); etok++) ; 
                    *etok++ = '\0';
                } else if (*f == '*') {
                    for (etok = tok; *etok; etok++) {
                        if (*etok == '#') {
                            *etok = '\0';
                        }
                    }
                } else {
                    for (etok = tok; *etok && !isspace((uchar) *etok); etok++) ;
                }
                *etok++ = '\0';
            }
            if (*f == '*') {
                f++;
                tok = trimQuotes(tok);
                * va_arg(args, char**) = tok;
                tok = etok;
                break;
            }

            switch (*f) {
            case '!':
                if (*tok == '!') {
                    *va_arg(args, int*) = HTTP_ROUTE_NOT;
                } else {
                    *va_arg(args, int*) = 0;
                    continue;
                }
                break;
            case 'B':
                *va_arg(args, bool*) = httpGetBoolToken(tok);;
                break;
            case 'N':
                *va_arg(args, int*) = (int) stoi(tok);
                break;
            case 'P':
                *va_arg(args, char**) = httpMakePath(route, route->home, strim(tok, "\"", MPR_TRIM_BOTH));
                break;
            case 'S':
                *va_arg(args, char**) = strim(tok, "\"", MPR_TRIM_BOTH);
                break;
            case 'T':
                value = strim(tok, "\"", MPR_TRIM_BOTH);
                *va_arg(args, char**) = stemplate(value, route->vars);
                break;
            case 'W':
                list = va_arg(args, MprList*);
                word = stok(tok, " \t\r\n", &tok);
                while (word) {
                    mprAddItem(list, word);
                    word = stok(0, " \t\r\n", &tok);
                }
                break;
            default:
                mprDebug("http route", 5, "Unknown token pattern %%\"%c\"", *f);
                break;
            }
            tok = etok;
        }
    }
    if (tok < end) {
        /*
            Extra unparsed text
         */
        for (; tok < end && isspace((uchar) *tok); tok++) ;
        if (*tok && *tok != '#') {
            mprDebug("http route", 5, "Extra unparsed text: \"%s\"", tok);
            return 0;
        }
    }
    if (*f) {
        /*
            Extra unparsed format tokens
         */
        for (; *f; f++) {
            if (*f == '%') {
                break;
            } else if (*f == '?') {
                switch (*++f) {
                case '!':
                case 'N':
                    *va_arg(args, int*) = 0;
                    break;
                case 'B':
                    *va_arg(args, bool*) = 0;
                    break;
                case 'D':
                case 'P':
                case 'S':
                case 'T':
                case '*':
                    *va_arg(args, char**) = 0;
                    break;
                case 'W':
                    break;
                default:
                    mprDebug("http route", 5, "Unknown token pattern %%\"%c\"", *f);
                    break;
                }
            }
        }
        if (*f) {
            mprDebug("http route", 5, "Missing directive parameters");
            return 0;
        }
    }
    va_end(args);
    return 1;
}


PUBLIC bool httpGetBoolToken(cchar *tok)
{
    return scaselessmatch(tok, "on") || scaselessmatch(tok, "true") || scaselessmatch(tok, "yes") || smatch(tok, "1");
}


static char *trimQuotes(char *str)
{
    ssize   len;

    assert(str);
    len = slen(str);
    if (*str == '\"' && str[len - 1] == '\"' && len > 2 && str[1] != '\"') {
        return snclone(&str[1], len - 2);
    }
    return sclone(str);
}


PUBLIC cchar *httpGetDir(HttpRoute *route, cchar *name)
{
    cchar   *key;

    key = sjoin(supper(name), "_DIR", NULL);
    return httpGetRouteVar(route, key);
}


PUBLIC void httpSetDir(HttpRoute *route, cchar *name, cchar *value)
{
    if (value == 0) {
        value = name;
    }
    value = mprJoinPath(route->home, value);
    httpSetRouteVar(route, sjoin(supper(name), "_DIR", NULL), httpMakePath(route, 0, value));
}


PUBLIC void httpSetDefaultDirs(HttpRoute *route)
{
    httpSetDir(route, "cache", 0);
    httpSetDir(route, "client", 0);
    httpSetDir(route, "paks", 0);
#if FUTURE
    httpSetDir(route, "public", 0);
#endif
}


PUBLIC MprHash *httpGetOptions(cchar *options)
{
    if (options == 0) {
        return mprCreateHash(-1, MPR_HASH_STABLE);
    }
    if (*options == '@') {
        /* Allow embedded URIs as options */
        options = sfmt("{ data-click: '%s'}", options);
    }
    assert(*options == '{');
    if (*options != '{') {
        options = sfmt("{%s}", options);
    }
    return mprDeserialize(options);
}


PUBLIC void *httpGetOption(MprHash *options, cchar *field, cchar *defaultValue)
{
    MprKey      *kp;
    cchar       *value;

    if (options == 0) {
        value = defaultValue;
    } else if ((kp = mprLookupKeyEntry(options, field)) == 0) {
        value = defaultValue;
    } else {
        value = kp->data;
    }
    return (void*) value;
}


PUBLIC MprHash *httpGetOptionHash(MprHash *options, cchar *field)
{
    MprKey      *kp;

    if (options == 0) {
        return 0;
    }
    if ((kp = mprLookupKeyEntry(options, field)) == 0) {
        return 0;
    }
    return (MprHash*) kp->data;
}


/* 
    Prepend an option
 */
PUBLIC void httpInsertOption(MprHash *options, cchar *field, cchar *value)
{
    MprKey      *kp;

    if (options == 0) {
        assert(options);
        return;
    }
    if ((kp = mprLookupKeyEntry(options, field)) != 0) {
        kp = mprAddKey(options, field, sjoin(value, " ", kp->data, NULL));
    } else {
        kp = mprAddKey(options, field, value);
    }
}


PUBLIC void httpAddOption(MprHash *options, cchar *field, cchar *value)
{
    MprKey      *kp;

    if (options == 0) {
        assert(options);
        return;
    }
    if ((kp = mprLookupKeyEntry(options, field)) != 0) {
        kp = mprAddKey(options, field, sjoin(kp->data, " ", value, NULL));
    } else {
        kp = mprAddKey(options, field, value);
    }
}


PUBLIC void httpRemoveOption(MprHash *options, cchar *field)
{
    if (options == 0) {
        assert(options);
        return;
    }
    mprRemoveKey(options, field);
}


PUBLIC bool httpOption(MprHash *hash, cchar *field, cchar *value, int useDefault)
{
    return smatch(value, httpGetOption(hash, field, useDefault ? value : 0));
}


PUBLIC void httpSetOption(MprHash *options, cchar *field, cchar *value)
{
    if (value == 0) {
        return;
    }
    if (options == 0) {
        assert(options);
        return;
    }
    mprAddKey(options, field, value);
}


PUBLIC void httpHideRoute(HttpRoute *route, bool on)
{
    route->flags &= ~HTTP_ROUTE_HIDDEN;
    if (on) {
        route->flags |= HTTP_ROUTE_HIDDEN;
    }
}


PUBLIC HttpLimits *httpGraduateLimits(HttpRoute *route, HttpLimits *limits)
{
    if (route->parent && route->limits == route->parent->limits) {
        if (limits == 0) {
            if (route->parent->limits) {
                limits = route->parent->limits;
            } else {
                limits = HTTP->serverLimits;
            }
        }
        route->limits = mprMemdup(limits, sizeof(HttpLimits));
    }
    return route->limits;
}


PUBLIC uint64 httpGetNumber(cchar *value)
{
    uint64  number;

    if (smatch(value, "unlimited")) {
        return MAXINT64;
    }
    if (smatch(value, "infinite") || smatch(value, "never")) {
        return MPR_MAX_TIMEOUT / MPR_TICKS_PER_SEC;
    }
    value = strim(slower(value), " \t", MPR_TRIM_BOTH);
    if (sends(value, "sec") || sends(value, "secs") || sends(value, "seconds") || sends(value, "seconds")) {
        number = stoi(value);
    } else if (sends(value, "min") || sends(value, "mins") || sends(value, "minute") || sends(value, "minutes")) {
        number = stoi(value) * 60;
    } else if (sends(value, "hr") || sends(value, "hrs") || sends(value, "hour") || sends(value, "hours")) {
        number = stoi(value) * 60 * 60;
    } else if (sends(value, "day") || sends(value, "days")) {
        number = stoi(value) * 60 * 60 * 24;
    } else if (sends(value, "kb") || sends(value, "k")) {
        number = stoi(value) * 1024;
    } else if (sends(value, "mb") || sends(value, "m")) {
        number = stoi(value) * 1024 * 1024;
    } else if (sends(value, "gb") || sends(value, "g")) {
        number = stoi(value) * 1024 * 1024 * 1024;
    } else if (sends(value, "byte") || sends(value, "bytes")) {
        number = stoi(value);
    } else {
        number = stoi(value);
    }
    return number;
}


PUBLIC MprTicks httpGetTicks(cchar *value)
{
    uint64  num;

    num = httpGetNumber(value);
    if (num >= (MAXINT64 / MPR_TICKS_PER_SEC)) {
        num = MAXINT64 / MPR_TICKS_PER_SEC;
    }
    return num * MPR_TICKS_PER_SEC;
}


#undef  GRADUATE_HASH
#undef  GRADUATE_LIST

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/rx.c ************/


/*
    rx.c -- Http receiver. Parses http requests and client responses.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/***************************** Forward Declarations ***************************/

static void addMatchEtag(HttpConn *conn, char *etag);
static void delayAwake(HttpConn *conn, MprEvent *event);
static char *getToken(HttpConn *conn, cchar *delim);
static bool getOutput(HttpConn *conn);
static void manageRange(HttpRange *range, int flags);
static void manageRx(HttpRx *rx, int flags);
static bool parseHeaders(HttpConn *conn, HttpPacket *packet);
static bool parseIncoming(HttpConn *conn);
static bool parseRange(HttpConn *conn, char *value);
static bool parseRequestLine(HttpConn *conn, HttpPacket *packet);
static bool parseResponseLine(HttpConn *conn, HttpPacket *packet);
static bool processCompletion(HttpConn *conn);
static bool processFinalized(HttpConn *conn);
static bool processContent(HttpConn *conn);
static void parseMethod(HttpConn *conn);
static bool processParsed(HttpConn *conn);
static bool processReady(HttpConn *conn);
static bool processRunning(HttpConn *conn);
static int setParsedUri(HttpConn *conn);
static int sendContinue(HttpConn *conn);

/*********************************** Code *************************************/

PUBLIC HttpRx *httpCreateRx(HttpConn *conn)
{
    HttpRx      *rx;

    if ((rx = mprAllocObj(HttpRx, manageRx)) == 0) {
        return 0;
    }
    rx->conn = conn;
    rx->length = -1;
    rx->ifMatch = 1;
    rx->ifModified = 1;
    rx->pathInfo = sclone("/");
    rx->scriptName = mprEmptyString();
    rx->needInputPipeline = httpClientConn(conn);
    rx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    rx->chunkState = HTTP_CHUNK_UNCHUNKED;
    rx->seqno = ++conn->totalRequests;
    return rx;
}


static void manageRx(HttpRx *rx, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(rx->accept);
        mprMark(rx->acceptCharset);
        mprMark(rx->acceptEncoding);
        mprMark(rx->acceptLanguage);
        mprMark(rx->authDetails);
        mprMark(rx->conn);
        mprMark(rx->connection);
        mprMark(rx->contentLength);
        mprMark(rx->cookie);
        mprMark(rx->etags);
        mprMark(rx->extraPath);
        mprMark(rx->files);
        mprMark(rx->headerPacket);
        mprMark(rx->headers);
        mprMark(rx->hostHeader);
        mprMark(rx->inputPipeline);
        mprMark(rx->inputRange);
        mprMark(rx->lang);
        mprMark(rx->method);
        mprMark(rx->mimeType);
        mprMark(rx->origin);
        mprMark(rx->originalMethod);
        mprMark(rx->originalUri);
        mprMark(rx->paramString);
        mprMark(rx->params);
        mprMark(rx->parsedUri);
        mprMark(rx->passwordDigest);
        mprMark(rx->pathInfo);
        mprMark(rx->pragma);
        mprMark(rx->redirect);
        mprMark(rx->referrer);
        mprMark(rx->requestData);
        mprMark(rx->route);
        mprMark(rx->scriptName);
        mprMark(rx->securityToken);
        mprMark(rx->session);
        mprMark(rx->statusMessage);
        mprMark(rx->svars);
        mprMark(rx->target);
        mprMark(rx->upgrade);
        mprMark(rx->uri);
        mprMark(rx->userAgent);
        mprMark(rx->webSocket);
    }
}


PUBLIC void httpDestroyRx(HttpRx *rx)
{
    if (rx->conn) {
        rx->conn->rx = 0;
        rx->conn = 0;
    }
}


/*
    HTTP Protocol state machine for server-side requests and client responses.
    Process an incoming request and drive the state machine. This will process only one request.
    All socket I/O is non-blocking, and this routine must not block. Note: packet may be null.
    Return true if the request is completed successfully.

    MUST only ever be called from httpIOEvent otherwise recursion plays havoc.
 */
PUBLIC void httpProtocol(HttpConn *conn)
{
    bool    canProceed;

    assert(conn);

    conn->lastActivity = conn->http->now;

    do {
        switch (conn->state) {
        case HTTP_STATE_BEGIN:
        case HTTP_STATE_CONNECTED:
            canProceed = parseIncoming(conn);
            break;

        case HTTP_STATE_PARSED:
            canProceed = processParsed(conn);
            break;

        case HTTP_STATE_CONTENT:
            canProceed = processContent(conn);
            break;

        case HTTP_STATE_READY:
            canProceed = processReady(conn);
            break;

        case HTTP_STATE_RUNNING:
            canProceed = processRunning(conn);
            break;

        case HTTP_STATE_FINALIZED:
            canProceed = processFinalized(conn);
            break;

        case HTTP_STATE_COMPLETE:
            canProceed = processCompletion(conn);
            break;

        default:
            canProceed = 0;
            break;
        }
        /*
            This may block briefly if GC is due
         */
        httpServiceQueues(conn, HTTP_BLOCK);

        /*
            This is the primary top-level GC yield for the http engine
         */
        if (mprNeedYield()) {
            mprYield(0);
        }
    } while (canProceed);
}


/*
    Parse the incoming http message. Return true to keep going with this or subsequent request, zero means
    insufficient data to proceed.
 */
static bool parseIncoming(HttpConn *conn)
{
    HttpRx      *rx;
    HttpAddress *address;
    HttpPacket  *packet;
    HttpLimits  *limits;
    ssize       len;
    int64       value;
    char        *start, *end;

    if ((packet = conn->input) == 0) {
        return 0;
    }
    if (mprShouldDenyNewRequests()) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "The server is terminating");
        return 0;
    }
    assert(conn->rx);
    assert(conn->tx);
    rx = conn->rx;
    limits = conn->limits;

    if (httpServerConn(conn) && !conn->activeRequest) {
        /*
            ErrorDocuments may come through here twice so test activeRequest to keep counters valid.
         */
        conn->activeRequest = 1;
        if ((value = httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, 1)) >= limits->requestsPerClientMax) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_SERVICE_UNAVAILABLE,
                "Too many concurrent requests for client: %s %d/%d", conn->ip, (int) value,
                limits->requestsPerClientMax);
            return 0;
        }
        httpMonitorEvent(conn, HTTP_COUNTER_REQUESTS, 1);
    }

    if ((len = httpGetPacketLength(packet)) == 0) {
        return 0;
    }
    start = mprGetBufStart(packet->content);
    while (*start == '\r' || *start == '\n') {
        mprGetCharFromBuf(packet->content);
        start = mprGetBufStart(packet->content);
    }
    /*
        Don't start processing until all the headers have been received (delimited by two blank lines)
     */
    if ((end = sncontains(start, "\r\n\r\n", len)) == 0 && (end = sncontains(start, "\n\n", len)) == 0) {
        if (len >= limits->headerSize) {
            httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                "Header too big. Length %zd vs limit %zd", len, limits->headerSize);
        }
        return 0;
    }
    rx->headerPacketLength = len = end - start;

    if (len >= limits->headerSize) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE, "Header too big. Length %zd vs limit %zd", len,
            limits->headerSize);
        return 0;
    }
    if (httpServerConn(conn)) {
        /* This will set conn->error if it does not validate - keep going to generate a response */
        if (!parseRequestLine(conn, packet)) {
            return 0;
        }
    } else if (!parseResponseLine(conn, packet)) {
        return 0;
    }
    if (!parseHeaders(conn, packet)) {
        return 0;
    }
    if (httpServerConn(conn)) {
        httpMatchHost(conn);
        if (setParsedUri(conn) < 0) {
            return 0;
        }
    } else if (rx->status != HTTP_CODE_CONTINUE) {
        /*
            Ignore Expect status responses. NOTE: Clients have already created their Tx pipeline.
         */
        httpCreateRxPipeline(conn, conn->http->clientRoute);
    }
    if (rx->flags & HTTP_EXPECT_CONTINUE) {
        sendContinue(conn);
        rx->flags &= ~HTTP_EXPECT_CONTINUE;
    }
    httpSetState(conn, HTTP_STATE_PARSED);

    if ((address = conn->address) != 0) {
        if (address->delay) {
            if (address->delayUntil > conn->http->now) {
                /*
                    Defensive counter measure - go slow
                 */
                mprCreateEvent(conn->dispatcher, "delayConn", conn->delay, delayAwake, conn, 0);
                return 0;
            } else {
                address->delay = 0;
                httpTrace(conn, "monitor.delay.stop", "context", "client:'%s'", conn->ip);
            }
        }
    }
    return 1;
}


/*
    Defensive countermesasure - resume output after a delay
 */
static void delayAwake(HttpConn *conn, MprEvent *event)
{
    conn->delay = 0;
    mprCreateEvent(conn->dispatcher, "resumeConn", 0, httpIOEvent, conn, 0);
}


static bool mapMethod(HttpConn *conn)
{
    HttpRx      *rx;
    cchar       *method;

    rx = conn->rx;
    if (rx->flags & HTTP_POST && (method = httpGetParam(conn, "-http-method-", 0)) != 0) {
        if (!scaselessmatch(method, rx->method)) {
            httpTrace(conn, "request.method", "context", "originalMethod:'%s',method:'%s'", rx->method, method);
            httpSetMethod(conn, method);
            return 1;
        }
    }
    return 0;
}


static void parseMethod(HttpConn *conn)
{
    HttpRx      *rx;
    cchar       *method;
    int         methodFlags;

    rx = conn->rx;
    method = rx->method;
    methodFlags = 0;

    switch (method[0]) {
    case 'D':
        if (strcmp(method, "DELETE") == 0) {
            methodFlags = HTTP_DELETE;
        }
        break;

    case 'G':
        if (strcmp(method, "GET") == 0) {
            methodFlags = HTTP_GET;
        }
        break;

    case 'H':
        if (strcmp(method, "HEAD") == 0) {
            methodFlags = HTTP_HEAD;
        }
        break;

    case 'O':
        if (strcmp(method, "OPTIONS") == 0) {
            methodFlags = HTTP_OPTIONS;
        }
        break;

    case 'P':
        if (strcmp(method, "POST") == 0) {
            methodFlags = HTTP_POST;
            rx->needInputPipeline = 1;

        } else if (strcmp(method, "PUT") == 0) {
            methodFlags = HTTP_PUT;
            rx->needInputPipeline = 1;
        }
        break;

    case 'T':
        if (strcmp(method, "TRACE") == 0) {
            methodFlags = HTTP_TRACE;
        }
        break;
    }
    rx->flags |= methodFlags;
}


/*
    Parse the first line of a http request. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Requests look like: METHOD URL HTTP/1.X.
 */
static bool parseRequestLine(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpLimits  *limits;
    char        *method, *uri, *protocol, *start;
    MprBuf      *content;
    ssize       len;

    rx = conn->rx;
    limits = conn->limits;

    /*
        These are initially set when the connection is accepted via httpAddConn. Revise to mark a new request.
     */
    conn->startMark = mprGetHiResTicks();
    conn->started = conn->http->now;

    content = packet->content;
    start = content->start;
    method = getToken(conn, 0);
    rx->originalMethod = rx->method = supper(method);
    parseMethod(conn);

    uri = getToken(conn, 0);
    len = slen(uri);
    if (*uri == '\0') {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
        return 0;
    } else if (len >= limits->uriSize) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad request. URI too long. Length %zd vs limit %zd", len, limits->uriSize);
        return 0;
    }
    protocol = getToken(conn, "\r\n");
    conn->protocol = supper(protocol);
    if (strcmp(conn->protocol, "HTTP/1.0") == 0) {
        if (rx->flags & (HTTP_POST|HTTP_PUT)) {
            rx->remainingContent = MAXINT;
            rx->needInputPipeline = 1;
        }
        conn->http10 = 1;
        conn->mustClose = 1;
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        conn->protocol = sclone("HTTP/1.1");
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return 0;
    }
    rx->uri = sclone(uri);
    if (!rx->originalUri) {
        rx->originalUri = rx->uri;
    }
    conn->http->totalRequests++;
    httpSetState(conn, HTTP_STATE_FIRST);

    if (httpTracing(conn)) {
        httpTrace(conn, "rx.first.server", "request", "method:'%s',uri:'%s',protocol:'%s'", 
            rx->method, rx->uri, conn->protocol);
        uri[-1] = ' ';
        protocol[-1] = ' ';
        content->start[-2] = '\r';
        content->start[-1] = '\n';
        httpTraceContent(conn, "rx.headers.server", "context", start, rx->headerPacketLength, NULL);
    }
    return 1;
}


/*
    Parse the first line of a http response. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Response status lines look like: HTTP/1.X CODE Message
 */
static bool parseResponseLine(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprBuf      *content;
    cchar       *endp;
    char        *protocol, *status;
    ssize       len;

    rx = conn->rx;
    tx = conn->tx;

    protocol = conn->protocol = supper(getToken(conn, 0));
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        conn->http10 = 1;
        if (!scaselessmatch(tx->method, "HEAD")) {
            rx->remainingContent = MAXINT;
        }
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return 0;
    }
    status = getToken(conn, 0);
    if (*status == '\0') {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Bad response status code");
        return 0;
    }
    rx->status = atoi(status);
    rx->statusMessage = sclone(getToken(conn, "\r\n"));

    len = slen(rx->statusMessage);
    if (len >= conn->limits->uriSize) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad response. Status message too long. Length %zd vs limit %zd", len, conn->limits->uriSize);
        return 0;
    }
    if (httpTracing(conn)) {
        httpTrace(conn, "rx.first.client", "request", "status:%d,protocol:'%s'", rx->status, protocol);
        content = packet->content;
        endp = strstr((char*) content->start, "\r\n\r\n");
        len = (endp) ? (int) (endp - content->start + 4) : 0;
        httpTraceContent(conn, "rx.headers.client", "context", content->start, len, NULL);
    }
    return 1;
}


/*
    Parse the request headers. Return true if the header parsed.
 */
static bool parseHeaders(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpLimits  *limits;
    MprBuf      *content;
    char        *cp, *key, *value, *tok, *hvalue;
    cchar       *oldValue;
    int         count, keepAliveHeader;

    rx = conn->rx;
    tx = conn->tx;
    rx->headerPacket = packet;
    content = packet->content;
    limits = conn->limits;
    keepAliveHeader = 0;

    for (count = 0; content->start[0] != '\r' && !conn->error; count++) {
        if (count >= limits->headerMax) {
            httpLimitError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Too many headers");
            return 0;
        }
        if ((key = getToken(conn, ":")) == 0 || *key == '\0') {
            httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header format");
            return 0;
        }
        value = getToken(conn, "\r\n");
        while (isspace((uchar) *value)) {
            value++;
        }
        if (strspn(key, "%<>/\\") > 0) {
            httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header key value");
            return 0;
        }
        if ((oldValue = mprLookupKey(rx->headers, key)) != 0) {
            hvalue = sfmt("%s, %s", oldValue, value);
        } else {
            hvalue = sclone(value);
        }
        mprAddKey(rx->headers, key, hvalue);

        switch (tolower((uchar) key[0])) {
        case 'a':
            if (strcasecmp(key, "authorization") == 0) {
                value = sclone(value);
                conn->authType = slower(stok(value, " \t", &tok));
                rx->authDetails = sclone(tok);

            } else if (strcasecmp(key, "accept-charset") == 0) {
                rx->acceptCharset = sclone(value);

            } else if (strcasecmp(key, "accept") == 0) {
                rx->accept = sclone(value);

            } else if (strcasecmp(key, "accept-encoding") == 0) {
                rx->acceptEncoding = sclone(value);

            } else if (strcasecmp(key, "accept-language") == 0) {
                rx->acceptLanguage = sclone(value);
            }
            break;

        case 'c':
            if (strcasecmp(key, "connection") == 0) {
                rx->connection = sclone(value);
                if (scaselesscmp(value, "KEEP-ALIVE") == 0) {
                    keepAliveHeader = 1;

                } else if (scaselesscmp(value, "CLOSE") == 0) {
                    conn->keepAliveCount = 0;
                    conn->mustClose = 1;
                }

            } else if (strcasecmp(key, "content-length") == 0) {
                if (rx->length >= 0) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Mulitple content length headers");
                    break;
                }
                rx->length = stoi(value);
                if (rx->length < 0) {
                    httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad content length");
                    return 0;
                }
                rx->contentLength = sclone(value);
                assert(rx->length >= 0);
                if (httpServerConn(conn) || !scaselessmatch(tx->method, "HEAD")) {
                    rx->remainingContent = rx->length;
                    rx->needInputPipeline = 1;
                }

            } else if (strcasecmp(key, "content-range") == 0) {
                /*
                    The Content-Range header is used in the response. The Range header is used in the request.
                    This headers specifies the range of any posted body data
                    Format is:  Content-Range: bytes n1-n2/length
                    Where n1 is first byte pos and n2 is last byte pos
                 */
                char    *sp;
                MprOff  start, end, size;

                start = end = size = -1;
                sp = value;
                while (*sp && !isdigit((uchar) *sp)) {
                    sp++;
                }
                if (*sp) {
                    start = stoi(sp);
                    if ((sp = strchr(sp, '-')) != 0) {
                        end = stoi(++sp);
                        if ((sp = strchr(sp, '/')) != 0) {
                            /*
                                Note this is not the content length transmitted, but the original size of the input of which
                                the client is transmitting only a portion.
                             */
                            size = stoi(++sp);
                        }
                    }
                }
                if (start < 0 || end < 0 || size < 0 || end < start) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad content range");
                    break;
                }
                rx->inputRange = httpCreateRange(conn, start, end);

            } else if (strcasecmp(key, "content-type") == 0) {
                rx->mimeType = sclone(value);
                if (rx->flags & (HTTP_POST | HTTP_PUT)) {
                    if (httpServerConn(conn)) {
                        rx->form = scontains(rx->mimeType, "application/x-www-form-urlencoded") != 0;
                        rx->upload = scontains(rx->mimeType, "multipart/form-data") != 0;
                    }
                } else {
                    rx->form = rx->upload = 0;
                }
            } else if (strcasecmp(key, "cookie") == 0) {
                if (rx->cookie && *rx->cookie) {
                    rx->cookie = sjoin(rx->cookie, "; ", value, NULL);
                } else {
                    rx->cookie = sclone(value);
                }
            }
            break;

        case 'e':
            if (strcasecmp(key, "expect") == 0) {
                /*
                    Handle 100-continue for HTTP/1.1 clients only. This is the only expectation that is currently supported.
                 */
                if (!conn->http10) {
                    if (strcasecmp(value, "100-continue") != 0) {
                        httpBadRequestError(conn, HTTP_CODE_EXPECTATION_FAILED, "Expect header value is not supported");
                    } else {
                        rx->flags |= HTTP_EXPECT_CONTINUE;
                    }
                }
            }
            break;

        case 'h':
            if (strcasecmp(key, "host") == 0) {
                rx->hostHeader = sclone(value);
            }
            break;

        case 'i':
            if ((strcasecmp(key, "if-modified-since") == 0) || (strcasecmp(key, "if-unmodified-since") == 0)) {
                MprTime     newDate = 0;
                char        *cp;
                bool        ifModified = (tolower((uchar) key[3]) == 'm');

                if ((cp = strchr(value, ';')) != 0) {
                    *cp = '\0';
                }
                if (mprParseTime(&newDate, value, MPR_UTC_TIMEZONE, NULL) < 0) {
                    assert(0);
                    break;
                }
                if (newDate) {
                    rx->since = newDate;
                    rx->ifModified = ifModified;
                    rx->flags |= HTTP_IF_MODIFIED;
                }

            } else if ((strcasecmp(key, "if-match") == 0) || (strcasecmp(key, "if-none-match") == 0)) {
                char    *word, *tok;
                bool    ifMatch = (tolower((uchar) key[3]) == 'm');

                if ((tok = strchr(value, ';')) != 0) {
                    *tok = '\0';
                }
                rx->ifMatch = ifMatch;
                rx->flags |= HTTP_IF_MODIFIED;
                value = sclone(value);
                word = stok(value, " ,", &tok);
                while (word) {
                    addMatchEtag(conn, word);
                    word = stok(0, " ,", &tok);
                }

            } else if (strcasecmp(key, "if-range") == 0) {
                char    *word, *tok;
                if ((tok = strchr(value, ';')) != 0) {
                    *tok = '\0';
                }
                rx->ifMatch = 1;
                rx->flags |= HTTP_IF_MODIFIED;
                value = sclone(value);
                word = stok(value, " ,", &tok);
                while (word) {
                    addMatchEtag(conn, word);
                    word = stok(0, " ,", &tok);
                }
            }
            break;

        case 'k':
            /* Keep-Alive: timeout=N, max=1 */
            if (strcasecmp(key, "keep-alive") == 0) {
                if ((tok = scontains(value, "max=")) != 0) {
                    conn->keepAliveCount = atoi(&tok[4]);
                    if (conn->keepAliveCount < 0) {
                        conn->keepAliveCount = 0;
                    }
                    if (conn->keepAliveCount > ME_MAX_KEEP_ALIVE) {
                        conn->keepAliveCount = ME_MAX_KEEP_ALIVE;
                    }
                    /*
                        IMPORTANT: Deliberately close client connections one request early. This encourages a client-led
                        termination and may help relieve excessive server-side TIME_WAIT conditions.
                     */
                    if (httpClientConn(conn) && conn->keepAliveCount == 1) {
                        conn->keepAliveCount = 0;
                    }
                }
            }
            break;

        case 'l':
            if (strcasecmp(key, "location") == 0) {
                rx->redirect = sclone(value);
            }
            break;

        case 'o':
            if (strcasecmp(key, "origin") == 0) {
                rx->origin = sclone(value);
            }
            break;

        case 'p':
            if (strcasecmp(key, "pragma") == 0) {
                rx->pragma = sclone(value);
            }
            break;

        case 'r':
            if (strcasecmp(key, "range") == 0) {
                /*
                    The Content-Range header is used in the response. The Range header is used in the request.
                 */
                if (!parseRange(conn, value)) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad range");
                }
            } else if (strcasecmp(key, "referer") == 0) {
                /* NOTE: yes the header is misspelt in the spec */
                rx->referrer = sclone(value);
            }
            break;

        case 't':
            if (strcasecmp(key, "transfer-encoding") == 0) {
                if (scaselesscmp(value, "chunked") == 0 && !conn->http10) {
                    /*
                        remainingContent will be revised by the chunk filter as chunks are processed and will
                        be set to zero when the last chunk has been received.
                     */
                    rx->flags |= HTTP_CHUNKED;
                    rx->chunkState = HTTP_CHUNK_START;
                    rx->remainingContent = MAXINT;
                    rx->needInputPipeline = 1;
                }
            }
            break;

        case 'x':
            if (strcasecmp(key, "x-http-method-override") == 0) {
                httpSetMethod(conn, value);

            } else if (strcasecmp(key, "x-own-params") == 0) {
                /*
                    Optimize and don't convert query and body content into params.
                    This is for those who want very large forms and to do their own custom handling.
                 */
                rx->ownParams = 1;
#if ME_DEBUG
            } else if (strcasecmp(key, "x-chunk-size") == 0) {
                tx->chunkSize = atoi(value);
                if (tx->chunkSize <= 0) {
                    tx->chunkSize = 0;
                } else if (tx->chunkSize > conn->limits->chunkSize) {
                    tx->chunkSize = conn->limits->chunkSize;
                }
#endif
            }
            break;

        case 'u':
            if (scaselesscmp(key, "upgrade") == 0) {
                rx->upgrade = sclone(value);
            } else if (strcasecmp(key, "user-agent") == 0) {
                rx->userAgent = sclone(value);
            }
            break;

        case 'w':
            if (strcasecmp(key, "www-authenticate") == 0) {
                cp = value;
                while (*value && !isspace((uchar) *value)) {
                    value++;
                }
                *value++ = '\0';
                conn->authType = slower(cp);
                rx->authDetails = sclone(value);
            }
            break;
        }
    }
    if (rx->form && rx->length >= conn->limits->receiveFormSize) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
            "Request form of %'lld bytes is too big. Limit %'lld", rx->length, conn->limits->receiveFormSize);
    }
    if (conn->error) {
        /* Cannot continue with keep-alive as the headers have not been correctly parsed */
        conn->keepAliveCount = 0;
        conn->connError = 1;
    }
    if (conn->http10 && !keepAliveHeader) {
        conn->keepAliveCount = 0;
    }
    if (httpClientConn(conn) && conn->mustClose && rx->length < 0) {
        /*
            Google does responses with a body and without a Content-Lenght like this:
                Connection: close
                Location: URI
         */
        rx->remainingContent = rx->redirect ? 0 : MAXINT;
    }
    if (!(rx->flags & HTTP_CHUNKED)) {
        /*
            Step over "\r\n" after headers.
            Don't do this if chunked so chunking can parse a single chunk delimiter of "\r\nSIZE ...\r\n"
         */
        mprAdjustBufStart(content, 2);
    }
    /*
        Split the headers and retain the data in conn->input. Revise lastRead to the number of data bytes available.
     */
    conn->input = httpSplitPacket(packet, 0);
    conn->lastRead = httpGetPacketLength(conn->input);
    return 1;
}


/*
    Called once the HTTP request/response headers have been parsed
 */
static bool processParsed(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;

    rx = conn->rx;
    tx = conn->tx;

    if (httpServerConn(conn)) {
        httpAddQueryParams(conn);
        rx->streaming = httpGetStreaming(conn->host, rx->mimeType, rx->uri);
        if (rx->streaming) {
            httpRouteRequest(conn);
        }
        /*
            Delay testing receiveBodySize till after routing for streaming requests. This way, recieveBodySize
            can be defined per route.
         */
        if (!rx->upload && rx->length >= conn->limits->receiveBodySize) {
            httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
                "Request content length %'lld bytes is too big. Limit %'lld", rx->length, conn->limits->receiveBodySize);
            return 0;
        }
        if (rx->streaming) {
            httpCreatePipeline(conn);
            /*
                Delay starting uploads until the files are extracted.
             */
            if (!rx->upload) {
                httpStartPipeline(conn);
            }
        }
#if ME_HTTP_WEB_SOCKETS
    } else {
        if (conn->upgraded && !httpVerifyWebSocketsHandshake(conn)) {
            httpSetState(conn, HTTP_STATE_FINALIZED);
            return 1;
        }
#endif
    }
    httpSetState(conn, HTTP_STATE_CONTENT);
    if (rx->remainingContent == 0) {
        httpSetEof(conn);
    }
    if (rx->eof && tx->started) {
        q = tx->queue[HTTP_QUEUE_RX];
        httpPutPacketToNext(q, httpCreateEndPacket());
        httpSetState(conn, HTTP_STATE_READY);
    }
    return 1;
}


/*
    Filter the packet data and determine the number of useful bytes in the packet.
    The packet may be split if it contains chunk data for the next chunk.
    Set *more to true if there is more useful (non-chunk header) data to be processed.
    Packet may be null.
 */
static ssize filterPacket(HttpConn *conn, HttpPacket *packet, int *more)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprOff      size;
    ssize       nbytes;

    rx = conn->rx;
    tx = conn->tx;
    *more = 0;

    if (mprIsSocketEof(conn->sock) || conn->connError) {
        httpSetEof(conn);
    }
    if (rx->chunkState) {
        nbytes = httpFilterChunkData(tx->queue[HTTP_QUEUE_RX], packet);
        if (rx->chunkState == HTTP_CHUNK_EOF) {
            httpSetEof(conn);
            assert(rx->remainingContent == 0);
        }
    } else {
        nbytes = min((ssize) rx->remainingContent, conn->lastRead);
        if (!conn->upgraded && (rx->remainingContent - nbytes) <= 0) {
            httpSetEof(conn);
        }
    }
    conn->lastRead = 0;

    assert(nbytes >= 0);
    rx->bytesRead += nbytes;
    if (!conn->upgraded) {
        rx->remainingContent -= nbytes;
        assert(rx->remainingContent >= 0);
    }

    /*
        Enforce sandbox limits
     */
    size = rx->bytesRead - rx->bytesUploaded;
    if (size >= conn->limits->receiveBodySize) {
        if (!rx->webSocket) {
            httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
                "Receive body of %'lld bytes (sofar) is too big. Limit %'lld", size, conn->limits->receiveBodySize);
        }
    } else if (rx->form && size >= conn->limits->receiveFormSize) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
            "Receive form of %'lld bytes (sofar) is too big. Limit %'lld", size, conn->limits->receiveFormSize);
    }
    if (packet && httpTracing(conn)) {
        httpTraceBody(conn, 0, packet, nbytes);
    }
    if (rx->eof) {
#if GITHUB_BUG || 1
        /*
            GitHub is doing a 302 redirection with a "Transfer-Encoding" with a "Connection:close" header without any body
         */
        if (conn->mustClose && (rx->chunkState && rx->chunkState != HTTP_CHUNK_EOF)) {
            rx->chunkState = HTTP_CHUNK_EOF;
        }
#endif
        if ((rx->remainingContent > 0 && (rx->length > 0 || !conn->mustClose)) ||
            (rx->chunkState && rx->chunkState != HTTP_CHUNK_EOF)) {
            /* Closing is the only way for HTTP/1.0 to signify the end of data */
            httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Connection lost");
            return 0;
        }
        if (nbytes > 0 && httpGetPacketLength(packet) > nbytes) {
            conn->input = httpSplitPacket(packet, nbytes);
            *more = 1;
        }
    } else {
        if (rx->chunkState && nbytes > 0 && httpGetPacketLength(packet) > nbytes) {
            /* Split data for next chunk */
            conn->input = httpSplitPacket(packet, nbytes);
            *more = 1;
        }
    }
    return nbytes;
}


static bool processContent(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;
    HttpPacket  *packet;
    ssize       nbytes;
    int         moreData;

    assert(conn);
    rx = conn->rx;
    tx = conn->tx;

    q = tx->queue[HTTP_QUEUE_RX];
    packet = conn->input;
    /* Packet may be null */

    if ((nbytes = filterPacket(conn, packet, &moreData)) > 0) {
        if (conn->state < HTTP_STATE_FINALIZED) {
            if (rx->inputPipeline) {
                httpPutPacketToNext(q, packet);
            } else {
                httpPutForService(q, packet, HTTP_DELAY_SERVICE);
            }
        }
        if (packet == conn->input) {
            conn->input = 0;
        }
    }
    if (rx->eof) {
        if (conn->state < HTTP_STATE_FINALIZED) {
            if (httpServerConn(conn)) {
                if (!rx->route) {
                    if (httpAddBodyParams(conn) < 0) {
                        httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad request parameters");
                    } else {
                        mapMethod(conn);
                    }
                    httpRouteRequest(conn);
                    httpCreatePipeline(conn);
                    /*
                        Transfer buffered input body data into the pipeline
                     */
                    while ((packet = httpGetPacket(q)) != 0) {
                        httpPutPacketToNext(q, packet);
                    }
                }
                httpPutPacketToNext(q, httpCreateEndPacket());
                if (!tx->started) {
                    httpStartPipeline(conn);
                }
            } else {
                httpPutPacketToNext(q, httpCreateEndPacket());
            }
            httpSetState(conn, HTTP_STATE_READY);
        }
        return 1;
    }
    if (tx->started) {
        /*
            Some requests (websockets) remain in the content state while still generating output
         */
        moreData += getOutput(conn);
    }
    return (conn->connError || moreData || mprNeedYield());
}


/*
    In the ready state after all content has been received
 */
static bool processReady(HttpConn *conn)
{
    httpReadyHandler(conn);
    httpSetState(conn, HTTP_STATE_RUNNING);
    if (httpClientConn(conn) && !conn->upgraded) {
        httpFinalize(conn);
    }
    return 1;
}


static bool processRunning(HttpConn *conn)
{
    assert(conn->rx->eof);

    if (conn->tx->finalized && conn->tx->finalizedConnector) {
        httpSetState(conn, HTTP_STATE_FINALIZED);
        return 1;
    }
    if (httpServerConn(conn)) {
        return getOutput(conn) || httpQueuesNeedService(conn) || mprNeedYield();
    }
    return 0;
}


/*
    Get more output by invoking the handler's writable callback. Called by processRunning.
    Also issues an HTTP_EVENT_WRITABLE for application level notification.
 */
static bool getOutput(HttpConn *conn)
{
    HttpQueue   *q;
    HttpTx      *tx;
    ssize       count;

    tx = conn->tx;
    if (tx->started && !tx->writeBlocked) {
        q = conn->writeq;
        count = q->count;
        if (!tx->finalizedOutput) {
            HTTP_NOTIFY(conn, HTTP_EVENT_WRITABLE, 0);
            if (tx->handler->writable) {
                tx->handler->writable(q);
            }
        }
        if (count != q->count) {
            return 1;
        }
    }
    return 0;
}


static void createErrorRequest(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpPacket  *packet;
    MprBuf      *buf;
    char        *cp, *headers, *originalUri;
    int         key;

    rx = conn->rx;
    tx = conn->tx;
    if (!rx->headerPacket) {
        return;
    }
    httpTrace(conn, "request.errordoc", "context", "location:'%s',status:%d", tx->errorDocument, tx->status);

    originalUri = rx->uri;
    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);

    /* Preserve the old status */
    conn->tx->status = tx->status;
    conn->rx->originalUri = originalUri;
    conn->error = 0;
    conn->errorMsg = 0;
    conn->upgraded = 0;
    conn->worker = 0;

    packet = httpCreateDataPacket(ME_MAX_BUFFER);
    mprPutToBuf(packet->content, "%s %s %s\r\n", rx->method, tx->errorDocument, conn->protocol);
    /*
        Sever the old Rx and Tx for GC
     */
    rx->conn = 0;
    tx->conn = 0;

    /*
        Reconstruct the headers. Change nulls to '\r', ' ', or ':' as appropriate
     */
    key = 0;
    headers = 0;
    /*
        Ensure buffer always has a trailing null (one past buf->end)
     */
    buf = rx->headerPacket->content;
    mprAddNullToBuf(buf);
    for (cp = buf->data; cp < &buf->end[-1]; cp++) {
        if (*cp == '\0') {
            if (cp[1] == '\n') {
                if (!headers) {
                    headers = &cp[2];
                }
                *cp = '\r';
                if (&cp[4] <= buf->end && cp[2] == '\r' && cp[3] == '\n') {
                    cp[4] = '\0';
                }
                key = 0;
            } else if (!key) {
                *cp = ':';
                key = 1;
            } else {
                *cp = ' ';
            }
        }
    }
    if (!headers || headers >= buf->end) {
        headers = "\r\n";
    }
    mprPutStringToBuf(packet->content, headers);
    conn->input = packet;
    conn->state = HTTP_STATE_CONNECTED;
}


static bool processFinalized(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;

    rx = conn->rx;
    tx = conn->tx;
    assert(tx->finalized);
    assert(tx->finalizedOutput);
    assert(tx->finalizedConnector);

#if ME_TRACE_MEM
    mprDebug(1, "Request complete, status %d, error %d, connError %d, %s%s, memsize %.2f MB",
        tx->status, conn->error, conn->connError, rx->hostHeader, rx->uri, mprGetMem() / 1024 / 1024.0);
#endif
    httpClosePipeline(conn);

    if (httpServerConn(conn) && rx) {
        httpMonitorEvent(conn, HTTP_COUNTER_NETWORK_IO, tx->bytesWritten);
    }
    httpSetState(conn, HTTP_STATE_COMPLETE);
    if (tx->errorDocument && !conn->connError && !smatch(tx->errorDocument, rx->uri)) {
        createErrorRequest(conn);
    }
    return 1;
}




static bool processCompletion(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprTicks    elapsed;
    MprOff      received;
    int         status;

    rx = conn->rx;
    tx = conn->tx;

    if (rx->session) {
        httpWriteSession(conn);
    }
    if (httpServerConn(conn) && conn->activeRequest) {
        httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
        conn->activeRequest = 0;
    }
    elapsed = mprGetTicks() - conn->started;
    if (httpTracing(conn)) {
        status = conn->endpoint ? tx->status : rx->status;
        received = rx->headerPacketLength + rx->bytesRead;
#if MPR_HIGH_RES_TIMER
        httpTrace(conn, 
            "request.completion", "result",
            "status:%d,error:%d,connError:%d,elapsed:%llu,elapsedTicks:%llu,received:%lld,sent:%lld",
            status, conn->error, conn->connError, elapsed, mprGetHiResTicks() - conn->startMark, 
            received, tx->bytesWritten);
#else
        httpTrace(conn, "request.completion", "result", 
            "status:%d,error:%d,connError:%d,elapsed:%llu,received:%lld,sent:%lld",
            status, conn->error, conn->connError, elapsed, received, tx->bytesWritten);
#endif
    }
    if (conn->http->requestCallback) {
        (conn->http->requestCallback)(conn);
    }
    return 0;
}


PUBLIC void httpSetRequestCallback(HttpRequestCallback callback)
{
    if (HTTP) {
        HTTP->requestCallback = callback;
    }
}


/*
    Used by ejscript Request.close
 */
PUBLIC void httpCloseRx(HttpConn *conn)
{
    if (conn->rx && !conn->rx->remainingContent) {
        /* May not have consumed all read data, so cannot be assured the next request will be okay */
        conn->keepAliveCount = 0;
    }
    if (httpClientConn(conn)) {
        httpEnableConnEvents(conn);
    }
}


PUBLIC bool httpContentNotModified(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprTime     modified;
    bool        same;

    rx = conn->rx;
    tx = conn->tx;

    if (rx->flags & HTTP_IF_MODIFIED) {
        /*
            If both checks, the last modification time and etag, claim that the request doesn't need to be
            performed, skip the transfer.
         */
        assert(tx->fileInfo.valid);
        modified = (MprTime) tx->fileInfo.mtime * MPR_TICKS_PER_SEC;
        same = httpMatchModified(conn, modified) && httpMatchEtag(conn, tx->etag);
        if (tx->outputRanges && !same) {
            tx->outputRanges = 0;
        }
        return same;
    }
    return 0;
}


PUBLIC HttpRange *httpCreateRange(HttpConn *conn, MprOff start, MprOff end)
{
    HttpRange     *range;

    if ((range = mprAllocObj(HttpRange, manageRange)) == 0) {
        return 0;
    }
    range->start = start;
    range->end = end;
    range->len = end - start;
    return range;
}


static void manageRange(HttpRange *range, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(range->next);
    }
}


PUBLIC MprOff httpGetContentLength(HttpConn *conn)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return conn->rx->length;
}


PUBLIC cchar *httpGetCookies(HttpConn *conn)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return conn->rx->cookie;
}


PUBLIC cchar *httpGetCookie(HttpConn *conn, cchar *name)
{
    HttpRx  *rx;
    cchar   *cookie;
    char    *cp, *value;
    ssize   nlen;
    int     quoted;

    assert(conn);
    rx = conn->rx;
    assert(rx);

    if ((cookie = rx->cookie) == 0 || name == 0 || *name == '\0') {
        return 0;
    }
    nlen = slen(name);
    while ((value = strstr(cookie, name)) != 0) {
        /* Ignore corrupt cookies of the form "name=;" */
        if ((value == cookie || value[-1] == ' ' || value[-1] == ';') && value[nlen] == '=' && value[nlen+1] != ';') {
            break;
        }
        cookie += nlen;
    }
    if (value == 0) {
        return 0;
    }
    value += nlen;
    while (isspace((uchar) *value) || *value == '=') {
        value++;
    }
    quoted = 0;
    if (*value == '"') {
        value++;
        quoted++;
    }
    for (cp = value; *cp; cp++) {
        if (quoted) {
            if (*cp == '"' && cp[-1] != '\\') {
                break;
            }
        } else {
            if ((*cp == ',' || *cp == ';') && cp[-1] != '\\') {
                break;
            }
        }
    }
    return snclone(value, cp - value);
}


PUBLIC cchar *httpGetHeader(HttpConn *conn, cchar *key)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return mprLookupKey(conn->rx->headers, slower(key));
}


PUBLIC char *httpGetHeadersFromHash(MprHash *hash)
{
    MprKey      *kp;
    char        *headers, *cp;
    ssize       len;

    for (len = 0, kp = 0; (kp = mprGetNextKey(hash, kp)) != 0; ) {
        len += strlen(kp->key) + 2 + strlen(kp->data) + 1;
    }
    if ((headers = mprAlloc(len + 1)) == 0) {
        return 0;
    }
    for (kp = 0, cp = headers; (kp = mprGetNextKey(hash, kp)) != 0; ) {
        strcpy(cp, kp->key);
        cp += strlen(cp);
        *cp++ = ':';
        *cp++ = ' ';
        strcpy(cp, kp->data);
        cp += strlen(cp);
        *cp++ = '\n';
    }
    *cp = '\0';
    return headers;
}


PUBLIC char *httpGetHeaders(HttpConn *conn)
{
    return httpGetHeadersFromHash(conn->rx->headers);
}


PUBLIC MprHash *httpGetHeaderHash(HttpConn *conn)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return conn->rx->headers;
}


PUBLIC cchar *httpGetQueryString(HttpConn *conn)
{
    return (conn->rx && conn->rx->parsedUri) ? conn->rx->parsedUri->query : 0;
}


PUBLIC int httpGetStatus(HttpConn *conn)
{
    return (conn->rx) ? conn->rx->status : 0;
}


PUBLIC char *httpGetStatusMessage(HttpConn *conn)
{
    return (conn->rx) ? conn->rx->statusMessage : 0;
}


PUBLIC void httpSetMethod(HttpConn *conn, cchar *method)
{
    conn->rx->method = sclone(method);
    parseMethod(conn);
}


static int setParsedUri(HttpConn *conn)
{
    HttpRx      *rx;
    HttpUri     *up;
    cchar       *hostname;

    rx = conn->rx;
    if (httpSetUri(conn, rx->uri) < 0 || rx->pathInfo[0] != '/') {
        httpBadRequestError(conn, HTTP_CODE_BAD_REQUEST, "Bad URL");
        rx->parsedUri = httpCreateUri("", 0);
        /* Continue to render a response */
    }
    /*
        Complete the URI based on the connection state.
        Must have a complete scheme, host, port and path.
     */
    up = rx->parsedUri;
    up->scheme = sclone(conn->secure ? "https" : "http");
    hostname = rx->hostHeader ? rx->hostHeader : conn->host->name;
    if (!hostname) {
        hostname = conn->sock->acceptIp;
    }
    mprParseSocketAddress(hostname, &up->host, NULL, NULL, 0);
    up->port = conn->sock->listenSock->port;
    return 0;
}


PUBLIC int httpSetUri(HttpConn *conn, cchar *uri)
{
    HttpRx      *rx;
    char        *pathInfo;

    rx = conn->rx;
    if ((rx->parsedUri = httpCreateUri(uri, 0)) == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    if ((pathInfo = httpValidateUriPath(rx->parsedUri->path)) == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    rx->pathInfo = pathInfo;
    rx->uri = rx->parsedUri->path;
    conn->tx->ext = httpGetExt(conn);
    /*
        Start out with no scriptName and the entire URI in the pathInfo. Stages may rewrite.
     */
    rx->scriptName = mprEmptyString();
    return 0;
}


PUBLIC ssize httpGetReadCount(HttpConn *conn)
{
    return conn->readq->count;
}


PUBLIC bool httpIsEof(HttpConn *conn)
{
    return conn->rx == 0 || conn->rx->eof;
}


PUBLIC cchar *httpGetBodyInput(HttpConn *conn)
{
    HttpQueue   *q;
    HttpRx      *rx;
    MprBuf      *content;

    rx = conn->rx;
    if (!rx->eof) {
        return 0;
    }
    q = conn->readq;
    if (q->first) {
        httpJoinPackets(q, -1);
        if ((content = q->first->content) != 0) {
            mprAddNullToBuf(content);
            return mprGetBufStart(content);
        }
    }
    return 0;
}


/*
    Set the connector as write blocked and cannot proceed.
 */
PUBLIC void httpSocketBlocked(HttpConn *conn)
{
    conn->tx->writeBlocked = 1;
}


static void addMatchEtag(HttpConn *conn, char *etag)
{
    HttpRx   *rx;

    rx = conn->rx;
    if (rx->etags == 0) {
        rx->etags = mprCreateList(-1, MPR_LIST_STABLE);
    }
    mprAddItem(rx->etags, sclone(etag));
}


/*
    Get the next input token. The content buffer is advanced to the next token. This routine always returns a
    non-zero token. The empty string means the delimiter was not found. The delimiter is a string to match and not
    a set of characters. If null, it means use white space (space or tab) as a delimiter.
 */
static char *getToken(HttpConn *conn, cchar *delim)
{
    MprBuf  *buf;
    char    *token, *endToken, *nextToken;

    buf = conn->input->content;
    token = mprGetBufStart(buf);
    nextToken = mprGetBufEnd(buf);
    for (token = mprGetBufStart(buf); (*token == ' ' || *token == '\t') && token < mprGetBufEnd(buf); token++) {}

    if (delim == 0) {
        delim = " \t";
        if ((endToken = strpbrk(token, delim)) != 0) {
            nextToken = endToken + strspn(endToken, delim);
            *endToken = '\0';
        }
    } else {
        if ((endToken = strstr(token, delim)) != 0) {
            *endToken = '\0';
            /* Only eat one occurence of the delimiter */
            nextToken = endToken + strlen(delim);
        }
    }
    buf->start = nextToken;
    return token;
}


/*
    Match the entity's etag with the client's provided etag.
 */
PUBLIC bool httpMatchEtag(HttpConn *conn, char *requestedEtag)
{
    HttpRx  *rx;
    char    *tag;
    int     next;

    rx = conn->rx;
    if (rx->etags == 0) {
        return 1;
    }
    if (requestedEtag == 0) {
        return 0;
    }
    for (next = 0; (tag = mprGetNextItem(rx->etags, &next)) != 0; ) {
        if (strcmp(tag, requestedEtag) == 0) {
            return (rx->ifMatch) ? 0 : 1;
        }
    }
    return (rx->ifMatch) ? 1 : 0;
}


/*
    If an IF-MODIFIED-SINCE was specified, then return true if the resource has not been modified. If using
    IF-UNMODIFIED, then return true if the resource was modified.
 */
PUBLIC bool httpMatchModified(HttpConn *conn, MprTime time)
{
    HttpRx   *rx;

    rx = conn->rx;

    if (rx->since == 0) {
        /*  If-Modified or UnModified not supplied. */
        return 1;
    }
    if (rx->ifModified) {
        /*  Return true if the file has not been modified.  */
        return !(time > rx->since);
    } else {
        /*  Return true if the file has been modified.  */
        return (time > rx->since);
    }
}


/*
    Format is:  Range: bytes=n1-n2,n3-n4,...
    Where n1 is first byte pos and n2 is last byte pos

    Examples:
        Range: bytes=0-49             first 50 bytes
        Range: bytes=50-99,200-249    Two 50 byte ranges from 50 and 200
        Range: bytes=-50              Last 50 bytes
        Range: bytes=1-               Skip first byte then emit the rest

    Return 1 if more ranges, 0 if end of ranges, -1 if bad range.
 */
static bool parseRange(HttpConn *conn, char *value)
{
    HttpTx      *tx;
    HttpRange   *range, *last, *next;
    char        *tok, *ep;

    tx = conn->tx;
    value = sclone(value);
    if (value == 0) {
        return 0;
    }
    /*
        Step over the "bytes="
     */
    stok(value, "=", &value);

    for (last = 0; value && *value; ) {
        if ((range = mprAllocObj(HttpRange, manageRange)) == 0) {
            return 0;
        }
        /*
            A range "-7" will set the start to -1 and end to 8
         */
        tok = stok(value, ",", &value);
        if (*tok != '-') {
            range->start = (ssize) stoi(tok);
        } else {
            range->start = -1;
        }
        range->end = -1;

        if ((ep = strchr(tok, '-')) != 0) {
            if (*++ep != '\0') {
                /*
                    End is one beyond the range. Makes the math easier.
                 */
                range->end = (ssize) stoi(ep) + 1;
            }
        }
        if (range->start >= 0 && range->end >= 0) {
            range->len = (int) (range->end - range->start);
        }
        if (last == 0) {
            tx->outputRanges = range;
        } else {
            last->next = range;
        }
        last = range;
    }

    /*
        Validate ranges
     */
    for (range = tx->outputRanges; range; range = range->next) {
        if (range->end != -1 && range->start >= range->end) {
            return 0;
        }
        if (range->start < 0 && range->end < 0) {
            return 0;
        }
        next = range->next;
        if (range->start < 0 && next) {
            /* This range goes to the end, so cannot have another range afterwards */
            return 0;
        }
        if (next) {
            if (range->end < 0) {
                return 0;
            }
            if (next->start >= 0 && range->end > next->start) {
                return 0;
            }
        }
    }
    conn->tx->currentRange = tx->outputRanges;
    return (last) ? 1: 0;
}


PUBLIC void httpSetEof(HttpConn *conn)
{
    conn->rx->eof = 1;
}


PUBLIC void httpSetStageData(HttpConn *conn, cchar *key, cvoid *data)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->requestData == 0) {
        rx->requestData = mprCreateHash(-1, 0);
    }
    mprAddKey(rx->requestData, key, data);
}


PUBLIC cvoid *httpGetStageData(HttpConn *conn, cchar *key)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->requestData == 0) {
        return NULL;
    }
    return mprLookupKey(rx->requestData, key);
}


PUBLIC char *httpGetPathExt(cchar *path)
{
    char    *ep, *ext;

    if ((ext = strrchr(path, '.')) != 0) {
        ext = sclone(++ext);
        for (ep = ext; *ep && isalnum((uchar) *ep); ep++) {
            ;
        }
        *ep = '\0';
    }
    return ext;
}


/*
    Get the request extension. Look first at the URI pathInfo. If no extension, look at the filename if defined.
    Return NULL if no extension.
 */
PUBLIC char *httpGetExt(HttpConn *conn)
{
    HttpRx  *rx;
    char    *ext;

    rx = conn->rx;
    if ((ext = httpGetPathExt(rx->pathInfo)) == 0) {
        if (conn->tx->filename) {
            ext = httpGetPathExt(conn->tx->filename);
        }
    }
    return ext;
}


//  FUTURE - can this just use the default compare
static int compareLang(char **s1, char **s2)
{
    return scmp(*s1, *s2);
}


PUBLIC HttpLang *httpGetLanguage(HttpConn *conn, MprHash *spoken, cchar *defaultLang)
{
    HttpRx      *rx;
    HttpLang    *lang;
    MprList     *list;
    cchar       *accept;
    char        *nextTok, *tok, *quality, *language;
    int         next;

    rx = conn->rx;
    if (rx->lang) {
        return rx->lang;
    }
    if (spoken == 0) {
        return 0;
    }
    list = mprCreateList(-1, MPR_LIST_STABLE);
    if ((accept = httpGetHeader(conn, "Accept-Language")) != 0) {
        for (tok = stok(sclone(accept), ",", &nextTok); tok; tok = stok(nextTok, ",", &nextTok)) {
            language = stok(tok, ";", &quality);
            if (quality == 0) {
                quality = "1";
            }
            mprAddItem(list, sfmt("%03d %s", (int) (atof(quality) * 100), language));
        }
        mprSortList(list, (MprSortProc) compareLang, 0);
        for (next = 0; (language = mprGetNextItem(list, &next)) != 0; ) {
            if ((lang = mprLookupKey(rx->route->languages, &language[4])) != 0) {
                rx->lang = lang;
                return lang;
            }
        }
    }
    if (defaultLang && (lang = mprLookupKey(rx->route->languages, defaultLang)) != 0) {
        rx->lang = lang;
        return lang;
    }
    return 0;
}


/*
    Trim extra path information after the uri extension. This is used by CGI and PHP only. The strategy is to
    heuristically find the script name in the uri. This is assumed to be the original uri up to and including
    first path component containing a "." Any path information after that is regarded as extra path.
    WARNING: Extra path is an old, unreliable, CGI specific technique. Do not use directories with embedded periods.
 */
PUBLIC void httpTrimExtraPath(HttpConn *conn)
{
    HttpRx      *rx;
    char        *cp, *extra;
    ssize       len;

    rx = conn->rx;
    if (!(rx->flags & (HTTP_OPTIONS | HTTP_TRACE))) {
        if ((cp = strchr(rx->pathInfo, '.')) != 0 && (extra = strchr(cp, '/')) != 0) {
            len = extra - rx->pathInfo;
            if (0 < len && len < slen(rx->pathInfo)) {
                rx->extraPath = sclone(&rx->pathInfo[len]);
                rx->pathInfo[len] = '\0';
            }
        }
        if ((cp = strchr(rx->target, '.')) != 0 && (extra = strchr(cp, '/')) != 0) {
            len = extra - rx->target;
            if (0 < len && len < slen(rx->target)) {
                rx->target[len] = '\0';
            }
        }
    }
}


/*
    Sends an 100 Continue response to the client. This bypasses the transmission pipeline, writing directly to the socket.
 */
static int sendContinue(HttpConn *conn)
{
    cchar      *response;

    assert(conn);

    if (!conn->tx->finalized && !conn->tx->bytesWritten) {
        response = sfmt("%s 100 Continue\r\n\r\n", conn->protocol);
        mprWriteSocket(conn->sock, response, slen(response));
        mprFlushSocket(conn->sock);
    }
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/sendConnector.c ************/


/*
    sendConnector.c -- Send file connector.

    The Sendfile connector supports the optimized transmission of whole static files. It uses operating system
    sendfile APIs to eliminate reading the document into user space and multiple socket writes. The send connector
    is not a general purpose connector. It cannot handle dynamic data or ranged requests. It does support chunked requests.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/**************************** Forward Declarations ****************************/
#if !ME_ROM

static void addPacketForSend(HttpQueue *q, HttpPacket *packet);
static void adjustSendVec(HttpQueue *q, MprOff written);
static MprOff buildSendVec(HttpQueue *q);
static void freeSendPackets(HttpQueue *q, MprOff written);
static void sendClose(HttpQueue *q);

/*********************************** Code *************************************/

PUBLIC int httpOpenSendConnector()
{
    HttpStage     *stage;

    if ((stage = httpCreateConnector("sendConnector", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->open = httpSendOpen;
    stage->close = sendClose;
    stage->outgoingService = httpSendOutgoingService;
    HTTP->sendConnector = stage;
    return 0;
}


/*
    Initialize the send connector for a request
 */
PUBLIC int httpSendOpen(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;

    if (tx->connector != conn->http->sendConnector) {
        httpAssignQueue(q, tx->connector, HTTP_QUEUE_TX);
        tx->connector->open(q);
        return 0;
    }
    if (!(tx->flags & HTTP_TX_NO_BODY)) {
        assert(tx->fileInfo.valid);
        if (tx->fileInfo.size > conn->limits->transmissionBodySize) {
            httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                "Http transmission aborted. File size exceeds max body of %'lld bytes", conn->limits->transmissionBodySize);
            return MPR_ERR_CANT_OPEN;
        }
        tx->file = mprOpenFile(tx->filename, O_RDONLY | O_BINARY, 0);
        if (tx->file == 0) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot open document: %s, err %d", tx->filename, mprGetError());
        }
    }
    return 0;
}


static void sendClose(HttpQueue *q)
{
    HttpTx  *tx;

    tx = q->conn->tx;
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
}


PUBLIC void httpSendOutgoingService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    MprFile     *file;
    MprOff      written;
    int         errCode;

    conn = q->conn;
    tx = conn->tx;
    conn->lastActivity = conn->http->now;

    if (tx->finalizedConnector) {
        return;
    }
    if (tx->flags & HTTP_TX_NO_BODY) {
        httpDiscardQueueData(q, 1);
    }
    if ((tx->bytesWritten + q->ioCount) > conn->limits->transmissionBodySize) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE | ((tx->bytesWritten) ? HTTP_ABORT : 0),
            "Http transmission aborted. Exceeded max body of %'lld bytes", conn->limits->transmissionBodySize);
        if (tx->bytesWritten) {
            httpFinalizeConnector(conn);
            return;
        }
    }
    tx->writeBlocked = 0;

    if (q->ioIndex == 0) {
        buildSendVec(q);
    }
    /*
        No need to loop around as send file tries to write as much of the file as possible.
        If not eof, will always have the socket blocked.
     */
    file = q->ioFile ? tx->file : 0;
    written = mprSendFileToSocket(conn->sock, file, q->ioPos, q->ioCount, q->iovec, q->ioIndex, NULL, 0);
    if (written < 0) {
        errCode = mprGetError();
        if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
            /*  Socket full, wait for an I/O event */
            tx->writeBlocked = 1;
        } else {
            if (errCode != EPIPE && errCode != ECONNRESET && errCode != ECONNABORTED && errCode != ENOTCONN) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "sendConnector: error, errCode %d", errCode);
            } else {
                httpDisconnect(conn);
            }
            httpFinalizeConnector(conn);
        }
        httpTrace(conn, "connection.io.error", "error", "msg:'Connector write error',errno:%d", errCode);

    } else if (written > 0) {
        tx->bytesWritten += written;
        freeSendPackets(q, written);
        adjustSendVec(q, written);
    }
    if (q->first && q->first->flags & HTTP_PACKET_END) {
        httpFinalizeConnector(conn);
    }
}


/*
    Build the IO vector. This connector uses the send file API which permits multiple IO blocks to be written with
    file data. This is used to write transfer the headers and chunk encoding boundaries. Return the count of bytes to
    be written. Return -1 for EOF.
 */
static MprOff buildSendVec(HttpQueue *q)
{
    HttpPacket  *packet, *prev;

    assert(q->ioIndex == 0);
    q->ioCount = 0;
    q->ioFile = 0;

    /*
        Examine each packet and accumulate as many packets into the I/O vector as possible. Can only have one data packet at
        a time due to the limitations of the sendfile API (on Linux). And the data packet must be after all the
        vector entries. Leave the packets on the queue for now, they are removed after the IO is complete for the
        entire packet.
     */
    for (packet = prev = q->first; packet && !(packet->flags & HTTP_PACKET_END); packet = packet->next) {
        if (packet->flags & HTTP_PACKET_HEADER) {
            httpWriteHeaders(q, packet);
        }
        if (q->ioFile || q->ioIndex >= (ME_MAX_IOVEC - 2)) {
            /* Only one file entry allowed */
            break;
        }
        if (packet->prefix || packet->esize || httpGetPacketLength(packet) > 0) {
            addPacketForSend(q, packet);
        } else {
            /* Remove empty packets */
            prev->next = packet->next;
            continue;
        }
        prev = packet;
    }
    return q->ioCount;
}


/*
    Add one entry to the io vector
 */
static void addToSendVector(HttpQueue *q, char *ptr, ssize bytes)
{
    assert(ptr > 0);
    assert(bytes > 0);

    q->iovec[q->ioIndex].start = ptr;
    q->iovec[q->ioIndex].len = bytes;
    q->ioCount += bytes;
    q->ioIndex++;
}


/*
    Add a packet to the io vector. Return the number of bytes added to the vector.
 */
static void addPacketForSend(HttpQueue *q, HttpPacket *packet)
{
    HttpConn     *conn;

    conn = q->conn;
    assert(q->count >= 0);
    assert(q->ioIndex < (ME_MAX_IOVEC - 2));

    if (packet->prefix) {
        addToSendVector(q, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix));
    }
    if (packet->esize > 0) {
        assert(q->ioFile == 0);
        q->ioFile = 1;
        q->ioCount += packet->esize;

    } else if (httpGetPacketLength(packet) > 0) {
        /*
            Header packets have actual content. File data packets are virtual and only have a count.
         */
        addToSendVector(q, mprGetBufStart(packet->content), httpGetPacketLength(packet));
        if (httpTracing(conn) && packet->flags & HTTP_PACKET_DATA) {
            httpTraceBody(conn, 1, packet, -1);
        }
    }
}


static void freeSendPackets(HttpQueue *q, MprOff bytes)
{
    HttpPacket  *packet;
    ssize       len;

    assert(q->first);
    assert(q->count >= 0);
    assert(bytes >= 0);

    /*
        Loop while data to be accounted for and we have not hit the end of data packet
        There should be 2-3 packets on the queue. A header packet for the HTTP response headers, an optional
        data packet with packet->esize set to the size of the file, and an end packet with no content.
        Must leave this routine with the end packet still on the queue and all bytes accounted for.
     */
    while ((packet = q->first) != 0 && !(packet->flags & HTTP_PACKET_END) && bytes > 0) {
        if (packet->prefix) {
            len = mprGetBufLength(packet->prefix);
            len = (ssize) min(len, bytes);
            mprAdjustBufStart(packet->prefix, len);
            bytes -= len;
            /* Prefixes don't count in the q->count. No need to adjust */
            if (mprGetBufLength(packet->prefix) == 0) {
                packet->prefix = 0;
            }
        }
        if (packet->esize) {
            len = (ssize) min(packet->esize, bytes);
            packet->esize -= len;
            packet->epos += len;
            bytes -= len;
            assert(packet->esize >= 0);

        } else if ((len = httpGetPacketLength(packet)) > 0) {
            /* Header packets come here */
            len = (ssize) min(len, bytes);
            mprAdjustBufStart(packet->content, len);
            bytes -= len;
            q->count -= len;
            assert(q->count >= 0);
        }
        if (packet->esize == 0 && httpGetPacketLength(packet) == 0) {
            /* Done with this packet - consume it */
            assert(!(packet->flags & HTTP_PACKET_END));
            httpGetPacket(q);
        } else {
            break;
        }
    }
    assert(bytes == 0);
}


/*
    Clear entries from the IO vector that have actually been transmitted. This supports partial writes due to the socket
    being full. Don't come here if we've seen all the packets and all the data has been completely written. ie. small files
    don't come here.
 */
static void adjustSendVec(HttpQueue *q, MprOff written)
{
    MprIOVec    *iovec;
    ssize       len;
    int         i, j;

    iovec = q->iovec;
    for (i = 0; i < q->ioIndex; i++) {
        len = iovec[i].len;
        if (written < len) {
            iovec[i].start += (ssize) written;
            iovec[i].len -= (ssize) written;
            return;
        }
        written -= len;
        q->ioCount -= len;
        for (j = i + 1; i < q->ioIndex; ) {
            iovec[j++] = iovec[i++];
        }
        q->ioIndex--;
        i--;
    }
    if (written > 0 && q->ioFile) {
        /* All remaining data came from the file */
        q->ioPos += written;
    }
    q->ioIndex = 0;
    q->ioCount = 0;
    q->ioFile = 0;
}


#else
PUBLIC int httpOpenSendConnector() { return 0; }
PUBLIC int httpSendOpen(HttpQueue *q) {}
PUBLIC void httpSendOutgoingService(HttpQueue *q) {}
#endif /* !ME_ROM */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/session.c ************/


/**
    session.c - Session data storage

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Forwards  *********************************/

static cchar *createSecurityToken(HttpConn *conn);
static void manageSession(HttpSession *sp, int flags);

/************************************* Code ***********************************/
/*
    Allocate a http session state object. This keeps a local hash for session state items.
    This is written via httpWriteSession to the backend session state store.
 */
static HttpSession *allocSessionObj(HttpConn *conn, cchar *id, cchar *data)
{
    HttpSession *sp;

    assert(conn);
    assert(id && *id);
    assert(conn->http);
    assert(conn->http->sessionCache);

    if ((sp = mprAllocObj(HttpSession, manageSession)) == 0) {
        return 0;
    }
    sp->lifespan = conn->limits->sessionTimeout;
    sp->id = sclone(id);
    sp->cache = conn->http->sessionCache;
    if (data) {
        sp->data = mprDeserialize(data);
    }
    if (!sp->data) {
        sp->data = mprCreateHash(ME_MAX_SESSION_HASH, 0);
    }
    return sp;
}


PUBLIC bool httpLookupSessionID(cchar *id)
{
    return mprLookupCache(HTTP->sessionCache, id, 0, 0) != 0;
}


/*
    Public API to create or re-create a session. Always returns with a new session store.
 */
PUBLIC HttpSession *httpCreateSession(HttpConn *conn)
{
    httpDestroySession(conn);
    return httpGetSession(conn, 1);
}


PUBLIC void httpSetSessionNotify(MprCacheProc callback)
{
    mprSetCacheNotify(HTTP->sessionCache, callback);
}


PUBLIC void httpDestroySession(HttpConn *conn)
{
    Http        *http;
    HttpRx      *rx;
    HttpSession *sp;
    cchar       *cookie;

    http = conn->http;
    rx = conn->rx;
    assert(http);

    lock(http);
    if ((sp = httpGetSession(conn, 0)) != 0) {
        cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
        httpRemoveCookie(conn, cookie);
        mprExpireCacheItem(sp->cache, sp->id, 0);
        sp->id = 0;
        rx->session = 0;
    }
    rx->sessionProbed = 0;
    unlock(http);
}


static void manageSession(HttpSession *sp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(sp->id);
        mprMark(sp->cache);
        mprMark(sp->data);
    }
}


/*
    Optionally create if "create" is true. Will not re-create.
 */
PUBLIC HttpSession *httpGetSession(HttpConn *conn, int create)
{
    Http        *http;
    HttpRx      *rx;
    cchar       *cookie, *data, *id;
    static int  seqno = 0;
    int         flags, thisSeqno;

    assert(conn);
    rx = conn->rx;
    http = conn->http;
    assert(rx);

    if (!rx->session) {
        if ((id = httpGetSessionID(conn)) != 0) {
            if ((data = mprReadCache(conn->http->sessionCache, id, 0, 0)) != 0) {
                rx->session = allocSessionObj(conn, id, data);
            }
        }
        if (!rx->session && create) {
            lock(http);
            thisSeqno = ++seqno;
            id = sfmt("%08x%08x%d", PTOI(conn->data) + PTOI(conn), (int) mprGetTicks(), thisSeqno);
            id = mprGetMD5WithPrefix(id, slen(id), "-http.session-");
            id = sfmt("%d%s", thisSeqno, mprGetMD5WithPrefix(id, slen(id), "::http.session::"));

            mprGetCacheStats(http->sessionCache, &http->activeSessions, NULL);
            if (http->activeSessions >= conn->limits->sessionMax) {
                unlock(http);
                httpLimitError(conn, HTTP_CODE_SERVICE_UNAVAILABLE, 
                    "Too many sessions %d/%d", http->activeSessions, conn->limits->sessionMax);
                return 0;
            }
            unlock(http);

            rx->session = allocSessionObj(conn, id, NULL);
            flags = (rx->route->flags & HTTP_ROUTE_VISIBLE_SESSION) ? 0 : HTTP_COOKIE_HTTP;
            cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
            httpSetCookie(conn, cookie, rx->session->id, "/", NULL, rx->session->lifespan, flags);
            httpTrace(conn, "request.session.create", "context", "cookie:'%s',session:'%s'", cookie, rx->session->id);

            if ((rx->route->flags & HTTP_ROUTE_XSRF) && rx->securityToken) {
                httpSetSessionVar(conn, ME_XSRF_COOKIE, rx->securityToken);
            }
        }
    }
    return rx->session;
}


PUBLIC MprHash *httpGetSessionObj(HttpConn *conn, cchar *key)
{
    HttpSession *sp;
    MprKey      *kp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 0)) != 0) {
        if ((kp = mprLookupKeyEntry(sp->data, key)) != 0) {
            return mprDeserialize(kp->data);
        }
    }
    return 0;
}


PUBLIC cchar *httpGetSessionVar(HttpConn *conn, cchar *key, cchar *defaultValue)
{
    HttpSession *sp;
    MprKey      *kp;
    cchar       *result;

    assert(conn);
    assert(key && *key);

    result = 0;
    if ((sp = httpGetSession(conn, 0)) != 0) {
        if ((kp = mprLookupKeyEntry(sp->data, key)) != 0) {
            if (kp->type == MPR_JSON_OBJ) {
                /* Wrong type */
                mprDebug("http session", 0, "Session var is an object");
                return defaultValue;
            } else {
                result = kp->data;
            }
        }
    }
    return result ? result : defaultValue;
}


PUBLIC int httpSetSessionObj(HttpConn *conn, cchar *key, MprHash *obj)
{
    HttpSession *sp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (obj == 0) {
        httpRemoveSessionVar(conn, key);
    } else {
        mprAddKey(sp->data, key, mprSerialize(obj, 0));
    }
    sp->dirty = 1;
    return 0;
}


/*
    Set a session variable. This will create the session store if it does not already exist.
    Note: If the headers have been emitted, the chance to set a cookie header has passed. So this value will go
    into a session that will be lost. Solution is for apps to create the session first.
    Value of null means remove the session.
 */
PUBLIC int httpSetSessionVar(HttpConn *conn, cchar *key, cchar *value)
{
    HttpSession  *sp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    if (value == 0) {
        httpRemoveSessionVar(conn, key);
    } else {
        mprAddKey(sp->data, key, sclone(value));
    }
    sp->dirty = 1;
    return 0;
}


PUBLIC int httpSetSessionLink(HttpConn *conn, void *link)
{
    HttpSession  *sp;

    assert(conn);

    if ((sp = httpGetSession(conn, 1)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    mprSetCacheLink(sp->cache, sp->id, link);
    return 0;
}


PUBLIC int httpRemoveSessionVar(HttpConn *conn, cchar *key)
{
    HttpSession  *sp;

    assert(conn);
    assert(key && *key);

    if ((sp = httpGetSession(conn, 0)) == 0) {
        return 0;
    }
    sp->dirty = 1;
    return mprRemoveKey(sp->data, key);
}


PUBLIC int httpWriteSession(HttpConn *conn)
{
    HttpSession     *sp;

    if ((sp = conn->rx->session) != 0) {
        if (sp->dirty) {
            if (mprWriteCache(sp->cache, sp->id, mprSerialize(sp->data, 0), 0, sp->lifespan, 0, MPR_CACHE_SET) == 0) {
                mprLog("error http session", 0, "Cannot persist session cache");
                return MPR_ERR_CANT_WRITE;
            }
            sp->dirty = 0;
        }
    }
    return 0;
}


PUBLIC cchar *httpGetSessionID(HttpConn *conn)
{
    HttpRx  *rx;
    cchar   *cookie;

    assert(conn);
    rx = conn->rx;
    assert(rx);

    if (rx->session) {
        assert(rx->session->id);
        assert(*rx->session->id);
        return rx->session->id;
    }
    if (rx->sessionProbed) {
        return 0;
    }
    rx->sessionProbed = 1;
    cookie = rx->route->cookie ? rx->route->cookie : HTTP_SESSION_COOKIE;
    return httpGetCookie(conn, cookie);
}


/*
    Create a security token to use to mitiate CSRF threats. Security tokens are expected to be sent with POST requests to 
    verify the request is not being forged.

    Note: the HttpSession API prevents session hijacking by pairing with the client IP
 */
static cchar *createSecurityToken(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (!rx->securityToken) {
        rx->securityToken = mprGetRandomString(32);
    }
    return rx->securityToken;
}


/*
    Get the security token from the session. Create one if one does not exist. Store the token in session store.
    Recreate if required.
 */
PUBLIC cchar *httpGetSecurityToken(HttpConn *conn, bool recreate)
{
    HttpRx      *rx;

    rx = conn->rx;

    if (recreate) {
        rx->securityToken = 0;
    } else {
        rx->securityToken = (char*) httpGetSessionVar(conn, ME_XSRF_COOKIE, 0);
    }
    if (rx->securityToken == 0) {
        createSecurityToken(conn);
        httpSetSessionVar(conn, ME_XSRF_COOKIE, rx->securityToken);
    }
    return rx->securityToken;
}


/*
    Add the security token to a XSRF cookie and response header
    Set recreate to true to force a recreation of the token.
 */
PUBLIC int httpAddSecurityToken(HttpConn *conn, bool recreate) 
{
    cchar   *securityToken;

    securityToken = httpGetSecurityToken(conn, recreate);
    httpSetCookie(conn, ME_XSRF_COOKIE, securityToken, "/", NULL,  0, 0);
    httpSetHeaderString(conn, ME_XSRF_HEADER, securityToken);
    return 0;
}


/*
    Check the security token with the request. This must match the last generated token stored in the session state.
    It is expected the client will set the X-XSRF-TOKEN header with the token.
 */
PUBLIC bool httpCheckSecurityToken(HttpConn *conn) 
{
    cchar   *requestToken, *sessionToken;

    if ((sessionToken = httpGetSessionVar(conn, ME_XSRF_COOKIE, 0)) != 0) {
        requestToken = httpGetHeader(conn, ME_XSRF_HEADER);
        if (!requestToken) {
            requestToken = httpGetParam(conn, ME_XSRF_PARAM, 0);
            if (!requestToken) {
                httpTrace(conn, "request.xsrf.error", "error", "msg:'Missing security token in request'");
            }
        }
        if (!smatch(sessionToken, requestToken)) {
            /*
                Potential CSRF attack. Deny request. Re-create a new security token so legitimate clients can retry.
             */
            httpTrace(conn, "request.xsrf.error", "error",   
                "msg:'Security token in request does not match session token',xsrf:'%s',sessionXsrf:'%s'", 
                requestToken, sessionToken);
            httpAddSecurityToken(conn, 1);
            return 0;
        }
    }
    return 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/stage.c ************/


/*
    stage.c -- Stages are the building blocks of the Http request pipeline.

    Stages support the extensible and modular processing of HTTP requests. Handlers are a kind of stage that are the 
    first line processing of a request. Connectors are the last stage in a chain to send/receive data over a network.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************* Forwards ***********************************/

static void manageStage(HttpStage *stage, int flags);

/*********************************** Code *************************************/
/*
    Put packets on the service queue.
 */
static void outgoing(HttpQueue *q, HttpPacket *packet)
{
    int     enableService;

    /*
        Handlers service routines must only be auto-enabled if better than ready.
     */
    enableService = !(q->stage->flags & HTTP_STAGE_HANDLER) || (q->conn->state >= HTTP_STATE_READY) ? 1 : 0;
    httpPutForService(q, packet, enableService);
}


/*
    Incoming data routine.  Simply transfer the data upstream to the next filter or handler.
 */
static void incoming(HttpQueue *q, HttpPacket *packet)
{
    assert(q);
    assert(packet);

    if (q->nextQ->put) {
        httpPutPacketToNext(q, packet);
    } else {
        /* This queue is the last queue in the pipeline */
        if (httpGetPacketLength(packet) > 0) {
            if (packet->flags & HTTP_PACKET_SOLO) {
                httpPutForService(q, packet, HTTP_DELAY_SERVICE);
            } else {
                httpJoinPacketForService(q, packet, 0);
            }
        } else {
            /* Zero length packet means eof */
            httpPutForService(q, packet, HTTP_DELAY_SERVICE);
        }
        HTTP_NOTIFY(q->conn, HTTP_EVENT_READABLE, 0);
    }
}


PUBLIC void httpDefaultIncoming(HttpQueue *q, HttpPacket *packet)
{
    assert(q);
    assert(packet);
    httpPutForService(q, packet, HTTP_DELAY_SERVICE);
}


PUBLIC void httpDefaultOutgoingServiceStage(HttpQueue *q)
{
    HttpPacket    *packet;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!httpWillNextQueueAcceptPacket(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        httpPutPacketToNext(q, packet);
    }
}


PUBLIC HttpStage *httpCreateStage(cchar *name, int flags, MprModule *module)
{
    HttpStage     *stage;

    assert(name && *name);

    if ((stage = httpLookupStage(name)) != 0) {
        if (!(stage->flags & HTTP_STAGE_UNLOADED)) {
            mprLog("error http", 0, "Stage %s already exists", name);
            return 0;
        }
    } else if ((stage = mprAllocObj(HttpStage, manageStage)) == 0) {
        return 0;
    }
    stage->flags = flags;
    stage->name = sclone(name);
    stage->incoming = incoming;
    stage->outgoing = outgoing;
    stage->outgoingService = httpDefaultOutgoingServiceStage;
    stage->module = module;
    httpAddStage(stage);
    return stage;
}


static void manageStage(HttpStage *stage, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(stage->name);
        mprMark(stage->path);
        mprMark(stage->stageData);
        mprMark(stage->module);
        mprMark(stage->extensions);
    }
}


PUBLIC HttpStage *httpCloneStage(HttpStage *stage)
{
    HttpStage   *clone;

    if ((clone = mprAllocObj(HttpStage, manageStage)) == 0) {
        return 0;
    }
    *clone = *stage;
    return clone;
}


PUBLIC HttpStage *httpCreateHandler(cchar *name, MprModule *module)
{
    return httpCreateStage(name, HTTP_STAGE_HANDLER, module);
}


PUBLIC HttpStage *httpCreateFilter(cchar *name, MprModule *module)
{
    return httpCreateStage(name, HTTP_STAGE_FILTER, module);
}


PUBLIC HttpStage *httpCreateConnector(cchar *name, MprModule *module)
{
    return httpCreateStage(name, HTTP_STAGE_CONNECTOR, module);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/trace.c ************/


/*
    trace.c -- Trace data
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    Event type default labels:

        request: 1
        result:  2
        context: 3
        form:    4
        body:    5
        debug:   5
 */

/********************************* Includes ***********************************/



/*********************************** Code *************************************/

static void manageTrace(HttpTrace *trace, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(trace->file);
        mprMark(trace->format);
        mprMark(trace->lastTime);
        mprMark(trace->buf);
        mprMark(trace->path);
        mprMark(trace->events);
        mprMark(trace->mutex);
    }
}

/*
    Parent may be null
 */
PUBLIC HttpTrace *httpCreateTrace(HttpTrace *parent)
{
    HttpTrace   *trace;

    if ((trace = mprAllocObj(HttpTrace, manageTrace)) == 0) {
        return 0;
    }
    if (parent) {
        *trace = *parent;
        trace->parent = parent;
    } else {
        if ((trace->events = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
            return 0;
        }
        mprAddKey(trace->events, "request", ITOP(1));
        mprAddKey(trace->events, "result", ITOP(2));
        mprAddKey(trace->events, "error", ITOP(2));
        mprAddKey(trace->events, "context", ITOP(3));
        mprAddKey(trace->events, "form", ITOP(4));
        mprAddKey(trace->events, "body", ITOP(5));
        mprAddKey(trace->events, "debug", ITOP(5));

        trace->size = HTTP_TRACE_MAX_SIZE;
        trace->formatter = httpDetailTraceFormatter;
        trace->logger = httpWriteTraceLogFile;
        trace->mutex = mprCreateLock();
    }
    return trace;
}


PUBLIC void httpSetTraceContentSize(HttpTrace *trace, ssize size)
{
    trace->maxContent = size;
}


PUBLIC void httpSetTraceEventLevel(HttpTrace *trace, cchar *type, int level)
{
    assert(trace);
    mprAddKey(trace->events, type, ITOP(level));
}


PUBLIC int httpGetTraceLevel()
{
    return HTTP->traceLevel;
}


PUBLIC void httpSetTraceFormat(HttpTrace *trace, cchar *format)
{
    trace->format = sclone(format);
}


PUBLIC HttpTraceFormatter httpSetTraceFormatter(HttpTrace *trace, HttpTraceFormatter callback)
{
    HttpTraceFormatter  prior;

    prior = trace->formatter;
    trace->formatter = callback;
    return prior;
}


PUBLIC void httpSetTraceFormatterName(HttpTrace *trace, cchar *name)
{
    HttpTraceFormatter  formatter;

    if (name && smatch(name, "common")) {
        if ((trace->events = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
            return;
        }
        mprAddKey(trace->events, "complete", ITOP(0));
        formatter = httpCommonTraceFormatter;
    } else {
       formatter = httpDetailTraceFormatter;
    }
    httpSetTraceFormatter(trace, formatter);
}


PUBLIC void httpSetTraceLevel(int level)
{
    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    HTTP->traceLevel = level;
}


PUBLIC void httpSetTraceLogger(HttpTrace *trace, HttpTraceLogger callback)
{
    trace->logger = callback;
}


/*
    Internal convenience: Used for incoming and outgoing packets.
 */
PUBLIC bool httpTraceBody(HttpConn *conn, bool outgoing, HttpPacket *packet, ssize len)
{
    cchar   *event, *type;

    if (!conn) {
        return 0;
    }
    if (len < 0) {
        len = httpGetPacketLength(packet);
    }
    if (outgoing) {
        if (conn->endpoint) {
            type = "body";
            event = "tx.body.data";
        } else {
            if (sstarts(conn->tx->mimeType, "application/x-www-form-urlencoded")) {
                type = "form";
                event = "tx.body.form";
            } else {
                type = "body";
                event = "tx.body.data";
            }
        }
    } else {
        if (conn->endpoint) {
            if (conn->rx->form) {
                type = "form";
                event = "rx.body.form";
            } else {
                type = "body";
                event = "rx.body.data";
            }
        } else {
            type = "body";
            event = "rx.body.data";
        }
    }
    return httpTracePacket(conn, event, type, packet, "length: %zd", len);
}


/*
    Trace request body content
 */
PUBLIC bool httpTraceContent(HttpConn *conn, cchar *event, cchar *type, cchar *buf, ssize len, cchar *values, ...)
{
    Http        *http;
    HttpTrace   *trace;
    va_list     ap;
    int         level;

    assert(conn);
    assert(buf);

    http = HTTP;
    if (http->traceLevel == 0) {
        return 0;
    }
    if (conn) {
        if (conn->rx->skipTrace) {
            return 0;
        }
        trace = conn->trace;
    } else {
        trace = http->trace;
    }
    level = PTOI(mprLookupKey(trace->events, type));
    if (level == 0 || level > http->traceLevel) {
        return 0;
    }
    if (conn) {
        if ((smatch(event, "rx.body.data") && (conn->rx->bytesRead >= conn->trace->maxContent)) ||
            (smatch(event, "tx.body.data") && (conn->tx->bytesWritten >= conn->trace->maxContent))) {
            if (!conn->rx->webSocket) {
                conn->rx->skipTrace = 1;
                httpTrace(conn, event, type, "msg: 'Abbreviating body trace'");
            }
            return 0;
        }
    }
    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    httpFormatTrace(trace, conn, event, type, values, buf, len);
    return 1;
}


/*
    Trace any packet
 */
PUBLIC bool httpTracePacket(HttpConn *conn, cchar *event, cchar *type, HttpPacket *packet, cchar *values, ...)
{
    va_list     ap;
    int         level;

    assert(conn);
    assert(packet);

    if (!conn || conn->http->traceLevel == 0 || conn->rx->skipTrace) {
        return 0;
    }
    level = PTOI(mprLookupKey(conn->trace->events, type));
    if (level == 0 || level > conn->http->traceLevel) { \
        return 0;
    }
    if (packet->prefix) {
        httpTraceContent(conn, event, type, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix), 0);
    }
    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    if (packet->content) {
        if (values) {
            httpTraceContent(conn, event, type, mprGetBufStart(packet->content), httpGetPacketLength(packet), "%s", values);
        } else {
            httpTraceContent(conn, event, type, mprGetBufStart(packet->content), httpGetPacketLength(packet), 0);
        }
    }
    return 1;
}


/*
    Inner routine for httpTrace()
    Conn may be null.
 */
PUBLIC bool httpTraceProc(HttpConn *conn, cchar *event, cchar *type, cchar *values, ...)
{
    HttpTrace   *trace;
    va_list     ap;

    assert(event && *event);
    assert(type && *type);

    if (conn && conn->rx->skipTrace) {
        return 0;
    }
    trace = conn ? conn->trace : HTTP->trace;

    if (values) {
        va_start(ap, values);
        values = sfmtv(values, ap);
        va_end(ap);
    }
    httpFormatTrace(trace, conn, event, type, values, 0, 0);
    return 1;
}



PUBLIC void httpFormatTrace(HttpTrace *trace, HttpConn *conn, cchar *event, cchar *type, cchar *values, cchar *buf, 
    ssize len)
{
    (trace->formatter)(trace, conn, event, type, values, buf, len);
}


/*
    Low-level write routine to be used only by formatters
 */
PUBLIC void httpWriteTrace(HttpTrace *trace, cchar *buf, ssize len)
{
    (trace->logger)(trace, buf, len);
}


/*
    Get a printable version of a buffer. Return a pointer to the start of printable data.
    This will use the tx or rx mime type if possible.
    Skips UTF encoding prefixes
 */
PUBLIC cchar *httpMakePrintable(HttpTrace *trace, HttpConn *conn, cchar *event, cchar *buf, ssize *lenp)
{
    cchar   *start, *cp, *digits;
    char    *data, *dp;
    ssize   len;
    int     i;

    if (conn) {
        if (smatch(event, "rx.body")) {
            if (sstarts(mprLookupMime(0, conn->rx->mimeType), "text/")) {
                return buf;
            }
        } else if (smatch(event, "tx.body")) {
            if (sstarts(mprLookupMime(0, conn->tx->mimeType), "text/")) {
                return buf;
            }
        }
    }
    start = buf;
    len = *lenp;
    if (len > 3 && start[0] == (char) 0xef && start[1] == (char) 0xbb && start[2] == (char) 0xbf) {
        /* Step over UTF encoding */
        start += 3;
        *lenp -= 3;
    }
    len = min(len, trace->maxContent);

    for (i = 0; i < len; i++) {
        if (!isprint((uchar) start[i]) && start[i] != '\n' && start[i] != '\r' && start[i] != '\t') {
            data = mprAlloc(len * 3 + ((len / 16) + 1) + 1);
            digits = "0123456789ABCDEF";
            for (i = 0, cp = start, dp = data; cp < &start[len]; cp++) {
                *dp++ = digits[(*cp >> 4) & 0x0f];
                *dp++ = digits[*cp & 0x0f];
                *dp++ = ' ';
                if ((++i % 16) == 0) {
                    *dp++ = '\n';
                }
            }
            *dp++ = '\n';
            *dp = '\0';
            start = data;
            *lenp = dp - start;
            break;
        }
    }
    return start;
}


/*
    Format a detailed request message
 */
PUBLIC void httpDetailTraceFormatter(HttpTrace *trace, HttpConn *conn, cchar *event, cchar *type, cchar *values, 
    cchar *data, ssize len)
{
    MprBuf      *buf;
    MprTime     now;
    char        *cp;
    int         client, sessionSeqno, gotColon;

    assert(trace);
    assert(event);
    assert(type);

    lock(trace);
    if (!trace->buf) {
        trace->buf = mprCreateBuf(0, 0);
    }
    buf = trace->buf;
    mprFlushBuf(buf);

    if (conn) {
        now = mprGetTime();
        if (trace->lastMark < (now + TPS) || trace->lastTime == 0) {
            trace->lastTime = mprGetDate("%T");
            trace->lastMark = now;
        }
        client = conn->address ? conn->address->seqno : 0;
        sessionSeqno = conn->rx->session ? (int) stoi(conn->rx->session->id) : 0;
        mprPutToBuf(buf, "\n%s %d-%d-%d-%d %s", trace->lastTime, client, sessionSeqno, conn->seqno, conn->rx->seqno, event);
    } else {
        mprPutToBuf(buf, "\n%s: %s", trace->lastTime, event);
    }
    if (values) {
        mprPutCharToBuf(buf, ' ');
        gotColon = 0;
        for (cp = (char*) values; *cp; cp++) {
            if (cp[0] == ':' && !gotColon) {
                cp[0] = '=';
                gotColon = 1;
            } else if (cp[0] == ',') {
                cp[0] = ' ';
                gotColon = 0;
            }
        }
        mprPutStringToBuf(buf, values);
        mprPutCharToBuf(buf, '\n');
    }
    if (data) {
        mprPutToBuf(buf, "\n----\n");
        data = httpMakePrintable(trace, conn, event, data, &len);
        mprPutBlockToBuf(buf, data, len);
        if (len > 0 && data[len - 1] != '\n') {
            mprPutCharToBuf(buf, '\n');
        }
        mprPutToBuf(buf, "----\n");
    }
    httpWriteTrace(trace, mprGetBufStart(buf), mprGetBufLength(buf));
    unlock(trace);
}


/************************************** TraceLogFile **************************/

static int backupTraceLogFile(HttpTrace *trace)
{
    MprPath     info;

    assert(trace->path);

    if (trace->file == MPR->logFile) {
        return 0;
    }
    if (trace->backupCount > 0 || (trace->flags & MPR_LOG_ANEW)) {
        lock(trace);
        if (trace->path && trace->parent && smatch(trace->parent->path, trace->path)) {
            unlock(trace);
            return backupTraceLogFile(trace->parent);
        }
        mprGetPathInfo(trace->path, &info);
        if (info.valid && ((trace->flags & MPR_LOG_ANEW) || info.size > trace->size)) {
            if (trace->file) {
                mprCloseFile(trace->file);
                trace->file = 0;
            }
            if (trace->backupCount > 0) {
                mprBackupLog(trace->path, trace->backupCount);
            }
        }
        unlock(trace);
    }
    return 0;
}


/*
    Open the request log file
 */
PUBLIC int httpOpenTraceLogFile(HttpTrace *trace)
{
    MprFile     *file;
    int         mode;

    if (!trace->file && trace->path) {
        if (smatch(trace->path, "-")) {
            file = MPR->logFile;
        } else {
            backupTraceLogFile(trace);
            mode = O_CREAT | O_WRONLY | O_TEXT;
            if (trace->flags & MPR_LOG_ANEW) {
                mode |= O_TRUNC;
            }
            if (smatch(trace->path, "stdout")) {
                file = MPR->stdOutput;
            } else if (smatch(trace->path, "stderr")) {
                file = MPR->stdError;
            } else if ((file = mprOpenFile(trace->path, mode, 0664)) == 0) {
                mprLog("error http trace", 0, "Cannot open log file %s", trace->path);
                return MPR_ERR_CANT_OPEN;
            }
        }
        trace->file = file;
        trace->flags &= ~MPR_LOG_ANEW;
    }
    return 0;
}


/*
    Start tracing when instructed via a command line option. No backup, max size or custom format.
 */
PUBLIC int httpStartTracing(cchar *traceSpec)
{
    HttpTrace   *trace;
    char        *lspec;

    if (HTTP == 0 || HTTP->trace == 0 || traceSpec == 0 || *traceSpec == '\0') {
        assert(HTTP);
        return MPR_ERR_BAD_STATE;
    }
    trace = HTTP->trace;
    trace->flags = MPR_LOG_ANEW | MPR_LOG_CMDLINE;
    trace->path = stok(sclone(traceSpec), ":", &lspec);
    HTTP->traceLevel = (int) stoi(lspec);
    return httpOpenTraceLogFile(trace);
}


/*
    Configure the trace log file
 */
PUBLIC int httpSetTraceLogFile(HttpTrace *trace, cchar *path, ssize size, int backup, cchar *format, int flags)
{
    assert(trace);
    assert(path && *path);

    if (format == NULL || *format == '\0') {
        format = ME_HTTP_LOG_FORMAT;
    }
    trace->backupCount = backup;
    trace->flags = flags;
    trace->format = sclone(format);
    trace->size = size;
    trace->path = sclone(path);
    return httpOpenTraceLogFile(trace);
}


/*
    Write a message to the trace log
 */
PUBLIC void httpWriteTraceLogFile(HttpTrace *trace, cchar *buf, ssize len)
{
    static int  skipCheck = 0;

    lock(trace);
    if (trace->backupCount > 0) {
        if ((++skipCheck % 50) == 0) {
            backupTraceLogFile(trace);
        }
    }
    if (!trace->file && trace->path && httpOpenTraceLogFile(trace) < 0) {
        unlock(trace);
        return;
    }
    mprWriteFile(trace->file, buf, len);
    unlock(trace);
}


/*
    Common Log Formatter (NCSA)
    This formatter only emits messages only for connections at their complete event.
 */
PUBLIC void httpCommonTraceFormatter(HttpTrace *trace, HttpConn *conn, cchar *type, cchar *event, cchar *valuesUnused,
    cchar *bufUnused, ssize lenUnused)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprBuf      *buf;
    cchar       *fmt, *cp, *qualifier, *timeText, *value;
    char        c, keyBuf[80];
    int         len;

    assert(trace);
    assert(type && *type);
    assert(event && *event);

    if (!conn) {
        return;
    }
    assert(type && *type);
    assert(event && *event);

    if (!smatch(event, "request.completion")) {
        return;
    }
    rx = conn->rx;
    tx = conn->tx;
    fmt = trace->format;
    if (fmt == 0) {
        fmt = ME_HTTP_LOG_FORMAT;
    }
    len = ME_MAX_URI + 256;
    buf = mprCreateBuf(len, len);

    while ((c = *fmt++) != '\0') {
        if (c != '%' || (c = *fmt++) == '%') {
            mprPutCharToBuf(buf, c);
            continue;
        }
        switch (c) {
        case 'a':                           /* Remote IP */
            mprPutStringToBuf(buf, conn->ip);
            break;

        case 'A':                           /* Local IP */
            mprPutStringToBuf(buf, conn->sock->listenSock->ip);
            break;

        case 'b':
            if (tx->bytesWritten == 0) {
                mprPutCharToBuf(buf, '-');
            } else {
                mprPutIntToBuf(buf, tx->bytesWritten);
            }
            break;

        case 'B':                           /* Bytes written (minus headers) */
            mprPutIntToBuf(buf, (tx->bytesWritten - tx->headerSize));
            break;

        case 'h':                           /* Remote host */
            mprPutStringToBuf(buf, conn->ip);
            break;

        case 'l':                           /* user identity - unknown */
            mprPutCharToBuf(buf, '-');
            break;

        case 'n':                           /* Local host */
            mprPutStringToBuf(buf, rx->parsedUri->host);
            break;

        case 'O':                           /* Bytes written (including headers) */
            mprPutIntToBuf(buf, tx->bytesWritten);
            break;

        case 'r':                           /* First line of request */
            mprPutToBuf(buf, "%s %s %s", rx->method, rx->uri, conn->protocol);
            break;

        case 's':                           /* Response code */
            mprPutIntToBuf(buf, tx->status);
            break;

        case 't':                           /* Time */
            mprPutCharToBuf(buf, '[');
            timeText = mprFormatLocalTime(MPR_DEFAULT_DATE, mprGetTime());
            mprPutStringToBuf(buf, timeText);
            mprPutCharToBuf(buf, ']');
            break;

        case 'u':                           /* Remote username */
            mprPutStringToBuf(buf, conn->username ? conn->username : "-");
            break;

        case '{':                           /* Header line "{header}i" */
            qualifier = fmt;
            if ((cp = schr(qualifier, '}')) != 0) {
                fmt = &cp[1];
                scopy(keyBuf, sizeof(keyBuf), "HTTP_");
                sncopy(&keyBuf[5], sizeof(keyBuf) - 5, qualifier, qualifier - cp);
                switch (*fmt++) {
                case 'i':
                    value = (char*) mprLookupKey(rx->headers, supper(keyBuf));
                    mprPutStringToBuf(buf, value ? value : "-");
                    break;
                default:
                    mprPutSubStringToBuf(buf, qualifier, qualifier - cp);
                }

            } else {
                mprPutCharToBuf(buf, c);
            }
            break;

        case '>':
            if (*fmt == 's') {
                fmt++;
                mprPutIntToBuf(buf, tx->status);
            }
            break;

        default:
            mprPutCharToBuf(buf, c);
            break;
        }
    }
    mprPutCharToBuf(buf, '\n');
    httpWriteTrace(trace, mprBufToString(buf), mprGetBufLength(buf));
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/tx.c ************/


/*
    tx.c - Http transmitter for server responses and client requests.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/***************************** Forward Declarations ***************************/

static void manageTx(HttpTx *tx, int flags);

/*********************************** Code *************************************/

PUBLIC HttpTx *httpCreateTx(HttpConn *conn, MprHash *headers)
{
    HttpTx      *tx;

    if ((tx = mprAllocObj(HttpTx, manageTx)) == 0) {
        return 0;
    }
    conn->tx = tx;
    tx->conn = conn;
    tx->status = HTTP_CODE_OK;
    tx->length = -1;
    tx->entityLength = -1;
    tx->chunkSize = -1;
    tx->cookies = mprCreateHash(HTTP_SMALL_HASH_SIZE, 0);
    tx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, 0);
    tx->queue[HTTP_QUEUE_TX] = httpCreateQueueHead(conn, "TxHead");
    conn->writeq = tx->queue[HTTP_QUEUE_TX]->nextQ;
    tx->queue[HTTP_QUEUE_RX] = httpCreateQueueHead(conn, "RxHead");
    conn->readq = tx->queue[HTTP_QUEUE_RX]->prevQ;

    if (headers) {
        tx->headers = headers;
    } else {
        tx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
        if (httpClientConn(conn)) {
            httpAddHeaderString(conn, "User-Agent", sclone(ME_HTTP_SOFTWARE));
        }
    }
    return tx;
}


PUBLIC void httpDestroyTx(HttpTx *tx)
{
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
    if (tx->conn) {
        tx->conn->tx = 0;
        tx->conn = 0;
    }
}


static void manageTx(HttpTx *tx, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(tx->altBody);
        mprMark(tx->authType);
        mprMark(tx->cache);
        mprMark(tx->cacheBuffer);
        mprMark(tx->cachedContent);
        mprMark(tx->conn);
        mprMark(tx->connector);
        mprMark(tx->cookies);
        mprMark(tx->currentRange);
        mprMark(tx->ext);
        mprMark(tx->etag);
        mprMark(tx->errorDocument);
        mprMark(tx->file);
        mprMark(tx->filename);
        mprMark(tx->handler);
        mprMark(tx->headers);
        mprMark(tx->method);
        mprMark(tx->mimeType);
        mprMark(tx->outputPipeline);
        mprMark(tx->outputRanges);
        mprMark(tx->parsedUri);
        mprMark(tx->queue[0]);
        mprMark(tx->queue[1]);
        mprMark(tx->rangeBoundary);
        mprMark(tx->webSockKey);
    }
}


/*
    Add key/value to the header hash. If already present, update the value
*/
static void setHdr(HttpConn *conn, cchar *key, cchar *value)
{
    assert(key && *key);
    assert(value);

    mprAddKey(conn->tx->headers, key, value);
}


PUBLIC int httpRemoveHeader(HttpConn *conn, cchar *key)
{
    assert(key && *key);
    if (conn->tx == 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    return mprRemoveKey(conn->tx->headers, key);
}


/*
    Add a http header if not already defined
 */
PUBLIC void httpAddHeader(HttpConn *conn, cchar *key, cchar *fmt, ...)
{
    char        *value;
    va_list     vargs;

    assert(key && *key);
    assert(fmt && *fmt);

    if (fmt) {
        va_start(vargs, fmt);
        value = sfmtv(fmt, vargs);
        va_end(vargs);
    } else {
        value = MPR->emptyString;
    }
    if (conn->tx && !mprLookupKey(conn->tx->headers, key)) {
        setHdr(conn, key, value);
    }
}


/*
    Add a header string if not already defined
 */
PUBLIC void httpAddHeaderString(HttpConn *conn, cchar *key, cchar *value)
{
    assert(key && *key);
    assert(value);

    if (conn->tx && !mprLookupKey(conn->tx->headers, key)) {
        setHdr(conn, key, sclone(value));
    }
}


/*
   Append a header. If already defined, the value is catenated to the pre-existing value after a ", " separator.
   As per the HTTP/1.1 spec. Except for Set-Cookie which HTTP permits multiple headers but not of the same cookie. Ugh!
 */
PUBLIC void httpAppendHeader(HttpConn *conn, cchar *key, cchar *fmt, ...)
{
    va_list     vargs;
    MprKey      *kp;
    char        *value;
    cchar       *cookie;

    if (!conn->tx) {
        return;
    }
    assert(key && *key);
    assert(fmt && *fmt);

    va_start(vargs, fmt);
    value = sfmtv(fmt, vargs);
    va_end(vargs);

    /*
        HTTP permits Set-Cookie to have multiple cookies. Other headers must comma separate multiple values.
        For Set-Cookie, must allow duplicates but not of the same cookie.
     */
    kp = mprLookupKeyEntry(conn->tx->headers, key);
    if (kp) {
        if (scaselessmatch(key, "Set-Cookie")) {
            cookie = stok(sclone(value), "=", NULL);
            while (kp) {
                if (scaselessmatch(kp->key, "Set-Cookie")) {
                    if (sstarts(kp->data, cookie)) {
                        kp->data = value;
                        break;
                    }
                }
                kp = kp->next;
            }
            if (!kp) {
                mprAddDuplicateKey(conn->tx->headers, key, value);
            }
        } else {
            setHdr(conn, key, sfmt("%s, %s", (char*) kp->data, value));
        }
    } else {
        setHdr(conn, key, value);
    }
}


/*
   Append a header string. If already defined, the value is catenated to the pre-existing value after a ", " separator.
   As per the HTTP/1.1 spec.
 */
PUBLIC void httpAppendHeaderString(HttpConn *conn, cchar *key, cchar *value)
{
    cchar   *oldValue;

    assert(key && *key);
    assert(value && *value);

    if (!conn->tx) {
        return;
    }
    oldValue = mprLookupKey(conn->tx->headers, key);
    if (oldValue) {
        if (scaselessmatch(key, "Set-Cookie")) {
            mprAddDuplicateKey(conn->tx->headers, key, sclone(value));
        } else {
            setHdr(conn, key, sfmt("%s, %s", oldValue, value));
        }
    } else {
        setHdr(conn, key, sclone(value));
    }
}


/*
    Set a http header. Overwrite if present.
 */
PUBLIC void httpSetHeader(HttpConn *conn, cchar *key, cchar *fmt, ...)
{
    char        *value;
    va_list     vargs;

    assert(key && *key);
    assert(fmt && *fmt);

    va_start(vargs, fmt);
    value = sfmtv(fmt, vargs);
    va_end(vargs);
    setHdr(conn, key, value);
}


PUBLIC void httpSetHeaderString(HttpConn *conn, cchar *key, cchar *value)
{
    assert(key && *key);
    assert(value);

    setHdr(conn, key, sclone(value));
}


/*
    Called by connectors (ONLY) when writing the entire output transmission is complete
 */
PUBLIC void httpFinalizeConnector(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    tx->finalizedConnector = 1;
    tx->finalizedOutput = 1;
}


/*
    Finalize the request. This means the caller is totally completed with the request. They have sent all
    output and have read all input. Further input can be discarded. Note that output may not yet have drained from
    the socket and so the connection state will not be transitioned to FINALIIZED until that happens and all
    remaining input has been dealt with.
 */
PUBLIC void httpFinalize(HttpConn *conn)
{
    HttpTx  *tx;

    tx = conn->tx;
    if (!tx || tx->finalized) {
        return;
    }
    tx->finalized = 1;
    if (conn->rx->session) {
        httpWriteSession(conn);
    }
    httpFinalizeOutput(conn);
}


/*
    This means the caller has generated the entire transmit body. Note: the data may not yet have drained from
    the pipeline or socket and the caller may not have read a response.
 */
PUBLIC void httpFinalizeOutput(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (!tx || tx->finalizedOutput) {
        return;
    }
    assert(conn->writeq);

    tx->responded = 1;
    tx->finalizedOutput = 1;
    if (!(tx->flags & HTTP_TX_PIPELINE)) {
        /* Tx Pipeline not yet created */
        tx->pendingFinalize = 1;
        return;
    }
    httpPutForService(conn->writeq, httpCreateEndPacket(), HTTP_SCHEDULE_QUEUE);
}


PUBLIC int httpIsFinalized(HttpConn *conn)
{
    return conn->tx->finalized;
}


PUBLIC int httpIsOutputFinalized(HttpConn *conn)
{
    return conn->tx->finalizedOutput;
}


/*
    This formats a response and sets the altBody. The response is not HTML escaped.
    This is the lowest level for formatResponse.
 */
PUBLIC ssize httpFormatResponsev(HttpConn *conn, cchar *fmt, va_list args)
{
    HttpTx      *tx;
    char        *body;

    tx = conn->tx;
    tx->responded = 1;
    body = fmt ? sfmtv(fmt, args) : conn->errorMsg;
    tx->altBody = body;
    tx->length = slen(tx->altBody);
    tx->flags |= HTTP_TX_NO_BODY;
    httpDiscardData(conn, HTTP_QUEUE_TX);
    return (ssize) tx->length;
}


/*
    This formats a response and sets the altBody. The response is not HTML escaped.
 */
PUBLIC ssize httpFormatResponse(HttpConn *conn, cchar *fmt, ...)
{
    va_list     args;
    ssize       rc;

    va_start(args, fmt);
    rc = httpFormatResponsev(conn, fmt, args);
    va_end(args);
    return rc;
}


/*
    This formats a complete response. Depending on the Accept header, the response will be either HTML or plain text.
    The response is not HTML escaped. This calls httpFormatResponse.
 */
PUBLIC ssize httpFormatResponseBody(HttpConn *conn, cchar *title, cchar *fmt, ...)
{
    va_list     args;
    char        *msg, *body;

    va_start(args, fmt);
    body = fmt ? sfmtv(fmt, args) : conn->errorMsg;

    if (scmp(conn->rx->accept, "text/plain") == 0) {
        msg = body;
    } else {
        msg = sfmt(
            "<!DOCTYPE html>\r\n"
            "<html><head><title>%s</title></head>\r\n"
            "<body>\r\n%s\r\n</body>\r\n</html>\r\n",
            title, body);
    }
    va_end(args);
    return httpFormatResponse(conn, "%s", msg);
}


PUBLIC void *httpGetQueueData(HttpConn *conn)
{
    HttpQueue     *q;

    q = conn->tx->queue[HTTP_QUEUE_TX];
    return q->nextQ->queueData;
}


PUBLIC void httpOmitBody(HttpConn *conn)
{
    HttpTx  *tx;

    tx = conn->tx;
    if (!tx) {
        return;
    }
    tx->flags |= HTTP_TX_NO_BODY;
    tx->length = -1;
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        /* Connectors will detect this also and disconnect */
    } else {
        httpDiscardData(conn, HTTP_QUEUE_TX);
    }
}


static bool localEndpoint(cchar *host)
{
    return smatch(host, "localhost") || smatch(host, "127.0.0.1") || smatch(host, "::1");
}


/*
    Redirect the user to another URI. The targetUri may or may not have a scheme or hostname.
 */
PUBLIC void httpRedirect(HttpConn *conn, int status, cchar *targetUri)
{
    HttpTx          *tx;
    HttpRx          *rx;
    HttpUri         *target, *base;
    HttpEndpoint    *endpoint;
    cchar           *msg;
    char            *dir, *cp;

    assert(targetUri);
    rx = conn->rx;
    tx = conn->tx;

    if (tx->finalized) {
        /* A response has already been formulated */
        mprLog("error", 0, "Response already prepared, so redirect ignored: %s", targetUri); 
        return;
    }
    tx->status = status;

    /*
        Expand the target for embedded tokens. Resolve relative to the current request URI
        This may add "localhost" if the host is missing in the targetUri.
     */
    targetUri = httpLink(conn, targetUri);
    msg = httpLookupStatus(status);

    if (300 <= status && status <= 399) {
        if (targetUri == 0) {
            targetUri = "/";
        }
        target = httpCreateUri(targetUri, 0);
        base = rx->parsedUri;
        /*
            Support URIs without a host:  https:///path. This is used to redirect onto the same host but with a
            different scheme. So find a suitable local endpoint to supply the port for the scheme.
        */
        if (!target->port && (target->scheme && !smatch(target->scheme, base->scheme))) {
            if (!target->host || smatch(base->host, target->host) || (localEndpoint(base->host) && localEndpoint(target->host))) {
                endpoint = smatch(target->scheme, "https") ? conn->host->secureEndpoint : conn->host->defaultEndpoint;
                if (endpoint) {
                    target->port = endpoint->port;
                } else if (smatch(target->scheme, "https")) {
                    mprLog("error", 0, "Missing secure endpoint to use with https redirection");
                }
            }
        }
        if (target->path && target->path[0] != '/') {
            /*
                Relative file redirection to a file in the same directory as the previous request.
             */
            dir = sclone(rx->pathInfo);
            if ((cp = strrchr(dir, '/')) != 0) {
                /* Remove basename */
                *cp = '\0';
            }
            target->path = sjoin(dir, "/", target->path, NULL);
        }
        target = httpCompleteUri(target, base);
        targetUri = httpUriToString(target, 0);
        httpSetHeader(conn, "Location", "%s", targetUri);
        httpFormatResponse(conn,
            "<!DOCTYPE html>\r\n"
            "<html><head><title>%s</title></head>\r\n"
            "<body><h1>%s</h1>\r\n<p>The document has moved <a href=\"%s\">here</a>.</p></body></html>\r\n",
            msg, msg, targetUri);
        httpTrace(conn, "request.redirect", "context", "status:%d,location:'%s'", status, targetUri);
    } else {
        httpFormatResponse(conn,
            "<!DOCTYPE html>\r\n"
            "<html><head><title>%s</title></head>\r\n"
            "<body><h1>%s</h1>\r\n</body></html>\r\n",
            msg, msg);
    }
    httpFinalize(conn);
}


PUBLIC void httpSetContentLength(HttpConn *conn, MprOff length)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return;
    }
    tx->length = length;
    httpSetHeader(conn, "Content-Length", "%lld", tx->length);
}


/*
    Set lifespan < 0 to delete the cookie in the client.
    Set lifespan == 0 for no expiry.
    WARNING: Some browsers (Chrome, Firefox) do not delete session cookies when you exit the browser.
 */
PUBLIC void httpSetCookie(HttpConn *conn, cchar *name, cchar *value, cchar *path, cchar *cookieDomain, 
    MprTicks lifespan, int flags)
{
    HttpRx      *rx;
    char        *cp, *expiresAtt, *expires, *domainAtt, *domain, *secure, *httponly;
    int         port;

    rx = conn->rx;
    if (path == 0) {
        path = "/";
    }
    domain = 0;
    if (cookieDomain) {
        if (*cookieDomain) {
            domain = (char*) cookieDomain;
        } else {
            /* Omit domain if set to empty string */
        }
    } else if (rx->hostHeader) {
        mprParseSocketAddress(rx->hostHeader, &domain, &port, NULL, 0);
        if (domain && port) {
            domain = 0;
        }
    }
    domainAtt = domain ? "; domain=" : "";
    if (domain && !strchr(domain, '.')) {
        if (smatch(domain, "localhost")) {
            domainAtt = domain = "";
        } else {
            domain = sjoin(".", domain, NULL);
        }
    }
    if (lifespan) {
        expiresAtt = "; expires=";
        expires = mprFormatUniversalTime(MPR_HTTP_DATE, mprGetTime() + lifespan);
    } else {
        expires = expiresAtt = "";
    }
    secure = (conn->secure & (flags & HTTP_COOKIE_SECURE)) ? "; secure" : "";
    httponly = (flags & HTTP_COOKIE_HTTP) ?  "; httponly" : "";

    mprAddKey(conn->tx->cookies, name, 
        sjoin(value, "; path=", path, domainAtt, domain, expiresAtt, expires, secure, httponly, NULL));

    if ((cp = mprLookupKey(conn->tx->headers, "Cache-Control")) == 0 || !scontains(cp, "no-cache")) {
        httpAppendHeader(conn, "Cache-Control", "no-cache=\"set-cookie\"");
    }
}


PUBLIC void httpRemoveCookie(HttpConn *conn, cchar *name)
{
    mprAddKey(conn->tx->cookies, name, MPR->emptyString);
}


static void setCorsHeaders(HttpConn *conn)
{
    HttpRoute   *route;
    cchar       *origin;

    route = conn->rx->route;

    /*
        Cannot use wildcard origin response if allowing credentials
     */
    if (*route->corsOrigin && !route->corsCredentials) {
        httpSetHeaderString(conn, "Access-Control-Allow-Origin", route->corsOrigin);
    } else {
        origin = httpGetHeader(conn, "Origin");
        httpSetHeaderString(conn, "Access-Control-Allow-Origin", origin ? origin : "*");
    }
    if (route->corsCredentials) {
        httpSetHeaderString(conn, "Access-Control-Allow-Credentials", "true");
    }
    if (route->corsHeaders) {
        httpSetHeaderString(conn, "Access-Control-Allow-Headers", route->corsHeaders);
    }
    if (route->corsMethods) {
        httpSetHeaderString(conn, "Access-Control-Allow-Methods", route->corsMethods);
    }
    if (route->corsAge) {
        httpSetHeader(conn, "Access-Control-Max-Age", "%d", route->corsAge);
    }
}


/*
    Set headers for httpWriteHeaders. This defines standard headers.
 */
static void setHeaders(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    HttpRange   *range;
    MprKeyValue *item;
    MprKey      *kp;
    MprOff      length;
    int         next;

    assert(packet->flags == HTTP_PACKET_HEADER);

    rx = conn->rx;
    tx = conn->tx;
    route = rx->route;

    /*
        Create headers for cookies
     */
    for (ITERATE_KEYS(tx->cookies, kp)) {
        httpAppendHeaderString(conn, "Set-Cookie", sjoin(kp->key, "=", kp->data, NULL));
    }

    /*
        Mandatory headers that must be defined here use httpSetHeader which overwrites existing values.
     */
    httpAddHeaderString(conn, "Date", conn->http->currentDate);

    if (tx->ext && route) {
        if (conn->error) {
            tx->mimeType = sclone("text/html");
        } else if ((tx->mimeType = (char*) mprLookupMime(route->mimeTypes, tx->ext)) == 0) {
            tx->mimeType = sclone("text/html");
        }
        httpAddHeaderString(conn, "Content-Type", tx->mimeType);
    }
    if (tx->etag) {
        httpAddHeader(conn, "ETag", "%s", tx->etag);
    }
    length = tx->length > 0 ? tx->length : 0;
    if (rx->flags & HTTP_HEAD) {
        conn->tx->flags |= HTTP_TX_NO_BODY;
        httpDiscardData(conn, HTTP_QUEUE_TX);
        if (tx->chunkSize <= 0) {
            httpAddHeader(conn, "Content-Length", "%lld", length);
        }

    } else if (tx->length < 0 && tx->chunkSize > 0) {
        httpSetHeaderString(conn, "Transfer-Encoding", "chunked");

    } else if (httpServerConn(conn)) {
        /* Server must not emit a content length header for 1XX, 204 and 304 status */
        if (!((100 <= tx->status && tx->status <= 199) || tx->status == 204 || tx->status == 304 || 
                tx->flags & HTTP_TX_NO_LENGTH)) {
            if (length >= 0) {
                httpAddHeader(conn, "Content-Length", "%lld", length);
            }
        }

    } else if (tx->length > 0) {
        /* client with body */
        httpAddHeader(conn, "Content-Length", "%lld", length);
    }
    if (tx->outputRanges) {
        if (tx->outputRanges->next == 0) {
            range = tx->outputRanges;
            if (tx->entityLength > 0) {
                httpSetHeader(conn, "Content-Range", "bytes %lld-%lld/%lld", range->start, range->end - 1, tx->entityLength);
            } else {
                httpSetHeader(conn, "Content-Range", "bytes %lld-%lld/*", range->start, range->end - 1);
            }
        } else {
            tx->mimeType = sfmt("multipart/byteranges; boundary=%s", tx->rangeBoundary);
            httpSetHeaderString(conn, "Content-Type", tx->mimeType);
        }
        httpSetHeader(conn, "Accept-Ranges", "bytes");
    }
    if (httpServerConn(conn)) {
        if (!(route->flags & HTTP_ROUTE_STEALTH)) {
            httpAddHeaderString(conn, "Server", conn->http->software);
        }
        /*
            If keepAliveCount == 1
         */
        if (--conn->keepAliveCount > 0) {
            assert(conn->keepAliveCount >= 1);
            httpAddHeaderString(conn, "Connection", "Keep-Alive");
            httpAddHeader(conn, "Keep-Alive", "timeout=%lld, max=%d", conn->limits->inactivityTimeout / 1000, 
                conn->keepAliveCount);
        } else {
            /* Tell the peer to close the connection */
            httpAddHeaderString(conn, "Connection", "close");
        }
        if (route->flags & HTTP_ROUTE_CORS) {
            setCorsHeaders(conn);
        }
        /*
            Apply route headers
         */
        for (ITERATE_ITEMS(route->headers, item, next)) {
            if (item->flags == HTTP_ROUTE_ADD_HEADER) {
                httpAddHeaderString(conn, item->key, item->value);
            } else if (item->flags == HTTP_ROUTE_APPEND_HEADER) {
                httpAppendHeaderString(conn, item->key, item->value);
            } else if (item->flags == HTTP_ROUTE_REMOVE_HEADER) {
                httpRemoveHeader(conn, item->key);
            } else if (item->flags == HTTP_ROUTE_SET_HEADER) {
                httpSetHeaderString(conn, item->key, item->value);
            }
        }
    }
}


PUBLIC void httpSetEntityLength(HttpConn *conn, int64 len)
{
    HttpTx      *tx;

    tx = conn->tx;
    tx->entityLength = len;
    if (tx->outputRanges == 0) {
        tx->length = len;
    }
}


/*
    Set the filename. The filename may be outside the route documents. So caller must take care.
    This will update HttpTx.ext and HttpTx.fileInfo.
 */
PUBLIC void httpSetFilename(HttpConn *conn, cchar *filename, int flags)
{
    HttpTx      *tx;
    MprPath     *info;

    assert(conn);

    tx = conn->tx;
    info = &tx->fileInfo;
    tx->flags &= ~(HTTP_TX_NO_CHECK | HTTP_TX_NO_MAP);
    tx->flags |= (flags & (HTTP_TX_NO_CHECK | HTTP_TX_NO_MAP));

    if (filename == 0) {
        tx->filename = 0;
        tx->ext = 0;
        info->checked = info->valid = 0;
        return;
    }
    if (!(tx->flags & HTTP_TX_NO_CHECK)) {
        if (!mprIsAbsPathContained(filename, conn->rx->route->documents)) {
            info->checked = 1;
            info->valid = 0;
            httpError(conn, HTTP_CODE_BAD_REQUEST, "Filename outside published documents");
            return;
        }
    }
    if (!tx->ext || tx->ext[0] == '\0') {
        tx->ext = httpGetPathExt(filename);
    }
    mprGetPathInfo(filename, info);
    if (info->valid) {
        //  OPT - using inodes mean this is harder to cache when served from multiple servers.
        tx->etag = sfmt("\"%llx-%llx-%llx\"", (int64) info->inode, (int64) info->size, (int64) info->mtime);
    }
    tx->filename = sclone(filename);

    if (tx->flags & HTTP_TX_PIPELINE) {
        /* Filename being revised after pipeline created */
        httpTrace(conn, "request.document", "context", "filename:'%s'", tx->filename);
    }
}


PUBLIC void httpSetResponded(HttpConn *conn)
{
    conn->tx->responded = 1;
}


PUBLIC void httpSetStatus(HttpConn *conn, int status)
{
    conn->tx->status = status;
    conn->tx->responded = 1;
}


PUBLIC void httpSetContentType(HttpConn *conn, cchar *mimeType)
{
    conn->tx->mimeType = sclone(mimeType);
    httpSetHeaderString(conn, "Content-Type", conn->tx->mimeType);
}


PUBLIC void httpWriteHeaders(HttpQueue *q, HttpPacket *packet)
{
    Http        *http;
    HttpConn    *conn;
    HttpTx      *tx;
    HttpUri     *parsedUri;
    HttpPacket  *altPacket;
    MprKey      *kp;
    MprBuf      *buf;

    assert(packet->flags == HTTP_PACKET_HEADER);

    conn = q->conn;
    http = conn->http;
    tx = conn->tx;
    buf = packet->content;

    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return;
    }
    tx->flags |= HTTP_TX_HEADERS_CREATED;
    tx->responded = 1;
    if (conn->headersCallback) {
        /* Must be before headers below */
        (conn->headersCallback)(conn->headersCallbackArg);
    }
    if (tx->flags & HTTP_TX_USE_OWN_HEADERS && !conn->error) {
        conn->keepAliveCount = 0;
        return;
    }
    setHeaders(conn, packet);

    if (httpServerConn(conn)) {
        mprPutStringToBuf(buf, conn->protocol);
        mprPutCharToBuf(buf, ' ');
        mprPutIntToBuf(buf, tx->status);
        mprPutCharToBuf(buf, ' ');
        mprPutStringToBuf(buf, httpLookupStatus(tx->status));
        /* Server tracing of status happens in the "complete" event */

    } else {
        mprPutStringToBuf(buf, tx->method);
        mprPutCharToBuf(buf, ' ');
        parsedUri = tx->parsedUri;
        if (http->proxyHost && *http->proxyHost) {
            if (parsedUri->query && *parsedUri->query) {
                mprPutToBuf(buf, "http://%s:%d%s?%s %s", http->proxyHost, http->proxyPort,
                    parsedUri->path, parsedUri->query, conn->protocol);
            } else {
                mprPutToBuf(buf, "http://%s:%d%s %s", http->proxyHost, http->proxyPort, parsedUri->path, conn->protocol);
            }
        } else {
            if (parsedUri->query && *parsedUri->query) {
                mprPutToBuf(buf, "%s?%s %s", parsedUri->path, parsedUri->query, conn->protocol);
            } else {
                mprPutStringToBuf(buf, parsedUri->path);
                mprPutCharToBuf(buf, ' ');
                mprPutStringToBuf(buf, conn->protocol);
            }
        }
        /* Client side trace */
        httpTrace(conn, "tx.first.client", "request", "method:'%s',uri:'%s',protocol:'%s'", tx->method, 
            parsedUri->path, conn->protocol);
    }
    mprPutStringToBuf(buf, "\r\n");

    /*
        Output headers
     */
    kp = mprGetFirstKey(conn->tx->headers);
    while (kp) {
        mprPutStringToBuf(packet->content, kp->key);
        mprPutStringToBuf(packet->content, ": ");
        if (kp->data) {
            mprPutStringToBuf(packet->content, kp->data);
        }
        mprPutStringToBuf(packet->content, "\r\n");
        kp = mprGetNextKey(conn->tx->headers, kp);
    }
    httpTracePacket(conn, conn->endpoint ? "tx.headers.server" : "tx.headers.client", "context", packet, 0);

    /*
        By omitting the "\r\n" delimiter after the headers, chunks can emit "\r\nSize\r\n" as a single chunk delimiter
     */
    if (tx->length >= 0 || tx->chunkSize <= 0) {
        mprPutStringToBuf(buf, "\r\n");
    }
    tx->headerSize = mprGetBufLength(buf);
    tx->flags |= HTTP_TX_HEADERS_CREATED;
    tx->authType = conn->authType;
    q->count += httpGetPacketLength(packet);

    if (tx->altBody) {
        /* Error responses are emitted here */
        httpDiscardQueueData(tx->queue[HTTP_QUEUE_TX]->nextQ, 0);
        altPacket = httpCreateDataPacket(slen(tx->altBody));
        mprPutStringToBuf(altPacket->content, tx->altBody);
        packet = httpGetPacket(q);
        httpPutBackPacket(q, altPacket);
        httpPutBackPacket(q, packet);
    }
}


PUBLIC bool httpFileExists(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (!tx->fileInfo.checked) {
        mprGetPathInfo(tx->filename, &tx->fileInfo);
    }
    return tx->fileInfo.valid;
}


/*
    Write a block of data. This is the lowest level write routine for data. This will buffer the data and flush if
    the queue buffer is full. Flushing is done by calling httpFlushQueue which will service queues as required. This
    may call the queue outgoing service routine and disable downstream queues if they are full.
 */
PUBLIC ssize httpWriteBlock(HttpQueue *q, cchar *buf, ssize len, int flags)
{
    HttpPacket  *packet;
    HttpConn    *conn;
    HttpTx      *tx;
    ssize       totalWritten, packetSize, thisWrite;

    assert(q == q->conn->writeq);
    conn = q->conn;
    tx = conn->tx;

    if (tx == 0 || tx->finalizedOutput) {
        return MPR_ERR_CANT_WRITE;
    }
    if (flags == 0) {
        flags = HTTP_BUFFER;
    }
    tx->responded = 1;

    for (totalWritten = 0; len > 0; ) {
        if (conn->state >= HTTP_STATE_FINALIZED) {
            return MPR_ERR_CANT_WRITE;
        }
        if (q->last && q->last != q->first && q->last->flags & HTTP_PACKET_DATA && mprGetBufSpace(q->last->content) > 0) {
            packet = q->last;
        } else {
            packetSize = (tx->chunkSize > 0) ? tx->chunkSize : q->packetSize;
            if ((packet = httpCreateDataPacket(packetSize)) == 0) {
                return MPR_ERR_MEMORY;
            }
            httpPutForService(q, packet, HTTP_DELAY_SERVICE);
        }
        assert(mprGetBufSpace(packet->content) > 0);
        thisWrite = min(len, mprGetBufSpace(packet->content));
        if (flags & (HTTP_BLOCK | HTTP_NON_BLOCK)) {
            thisWrite = min(thisWrite, q->max - q->count);
        }
        if (thisWrite > 0) {
            if ((thisWrite = mprPutBlockToBuf(packet->content, buf, thisWrite)) == 0) {
                return MPR_ERR_MEMORY;
            }
            buf += thisWrite;
            len -= thisWrite;
            q->count += thisWrite;
            totalWritten += thisWrite;
        }
        if (q->count >= q->max) {
            httpFlushQueue(q, flags);
            if (q->count >= q->max && (flags & HTTP_NON_BLOCK)) {
                break;
            }
        }
    }
    if (conn->error) {
        return MPR_ERR_CANT_WRITE;
    }
    if (httpClientConn(conn)) {
        httpEnableConnEvents(conn);
    }
    return totalWritten;
}


PUBLIC ssize httpWriteString(HttpQueue *q, cchar *s)
{
    return httpWriteBlock(q, s, strlen(s), HTTP_BUFFER);
}


PUBLIC ssize httpWriteSafeString(HttpQueue *q, cchar *s)
{
    return httpWriteString(q, mprEscapeHtml(s));
}


PUBLIC ssize httpWrite(HttpQueue *q, cchar *fmt, ...)
{
    va_list     vargs;
    char        *buf;

    va_start(vargs, fmt);
    buf = sfmtv(fmt, vargs);
    va_end(vargs);
    return httpWriteString(q, buf);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/uploadFilter.c ************/


/*
    uploadFilter.c - Upload file filter.
    The upload filter processes post data according to RFC-1867 ("multipart/form-data" post data).
    It saves the uploaded files in a configured upload directory.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/



/*********************************** Locals ***********************************/
/*
    Upload state machine states
 */
#define HTTP_UPLOAD_REQUEST_HEADER        1   /* Request header */
#define HTTP_UPLOAD_BOUNDARY              2   /* Boundary divider */
#define HTTP_UPLOAD_CONTENT_HEADER        3   /* Content part header */
#define HTTP_UPLOAD_CONTENT_DATA          4   /* Content encoded data */
#define HTTP_UPLOAD_CONTENT_END           5   /* End of multipart message */

/*
    Per upload context
 */
typedef struct Upload {
    HttpUploadFile  *currentFile;       /* Current file context */
    MprFile         *file;              /* Current file I/O object */
    char            *boundary;          /* Boundary signature */
    ssize           boundaryLen;        /* Length of boundary */
    int             contentState;       /* Input states */
    char            *clientFilename;    /* Current file filename */
    char            *tmpPath;           /* Current temp filename for upload data */
    char            *name;              /* Form field name keyword value */
} Upload;

/********************************** Forwards **********************************/

static void closeUpload(HttpQueue *q);
static char *getBoundary(char *buf, ssize bufLen, char *boundary, ssize boundaryLen, bool *pureData);
static void incomingUpload(HttpQueue *q, HttpPacket *packet);
static void manageHttpUploadFile(HttpUploadFile *file, int flags);
static void manageUpload(Upload *up, int flags);
static int matchUpload(HttpConn *conn, HttpRoute *route, int dir);
static int openUpload(HttpQueue *q);
static int  processUploadBoundary(HttpQueue *q, char *line);
static int  processUploadHeader(HttpQueue *q, char *line);
static int  processUploadData(HttpQueue *q);

/************************************* Code ***********************************/

PUBLIC int httpOpenUploadFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("uploadFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->uploadFilter = filter;
    filter->match = matchUpload;
    filter->open = openUpload;
    filter->close = closeUpload;
    filter->incoming = incomingUpload;
    return 0;
}


/*
    Match if this request needs the upload filter. Return true if needed.
 */
static int matchUpload(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpRx  *rx;
    char    *pat;
    ssize   len;

    if (!(dir & HTTP_STAGE_RX)) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    rx = conn->rx;
    if (!(rx->flags & HTTP_POST) || rx->remainingContent <= 0) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    pat = "multipart/form-data";
    len = strlen(pat);
    if (sncaselesscmp(rx->mimeType, pat, len) == 0) {
        rx->upload = 1;
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


static cchar *getUploadDir(HttpRoute *route)
{
    cchar   *uploadDir;

    if ((uploadDir = httpGetDir(route, "upload")) == 0) {
#if ME_WIN_LIKE
        uploadDir = mprNormalizePath(getenv("TEMP"));
#else
        uploadDir = sclone("/tmp");
#endif
    }
    return uploadDir;
}


/*
    Initialize the upload filter for a new request
 */
static int openUpload(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    Upload      *up;
    cchar       *uploadDir;
    char        *boundary;

    conn = q->conn;
    rx = conn->rx;

    if ((up = mprAllocObj(Upload, manageUpload)) == 0) {
        return MPR_ERR_MEMORY;
    }
    q->queueData = up;
    up->contentState = HTTP_UPLOAD_BOUNDARY;
    rx->autoDelete = rx->route->autoDelete;

    uploadDir = getUploadDir(rx->route);
    httpSetParam(conn, "UPLOAD_DIR", uploadDir);

    if ((boundary = strstr(rx->mimeType, "boundary=")) != 0) {
        boundary += 9;
        up->boundary = sjoin("--", boundary, NULL);
        up->boundaryLen = strlen(up->boundary);
    }
    if (up->boundaryLen == 0 || *up->boundary == '\0') {
        httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad boundary");
        return MPR_ERR_BAD_ARGS;
    }
    return 0;
}


static void manageUpload(Upload *up, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(up->currentFile);
        mprMark(up->file);
        mprMark(up->boundary);
        mprMark(up->clientFilename);
        mprMark(up->tmpPath);
        mprMark(up->name);
    }
}


/*
    Cleanup when the entire request has complete
 */
static void closeUpload(HttpQueue *q)
{
    HttpUploadFile  *file;
    HttpRx          *rx;
    Upload          *up;

    rx = q->conn->rx;
    up = q->queueData;

    if (rx->autoDelete) {
        httpRemoveAllUploadedFiles(q->conn);
    }
    if (up->currentFile) {
        file = up->currentFile;
        file->filename = 0;
    }
}


/*
    Incoming data acceptance routine. The service queue is used, but not a service routine as the data is processed
    immediately. Partial data is buffered on the service queue until a correct mime boundary is seen.
 */
static void incomingUpload(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpRx      *rx;
    MprBuf      *content;
    Upload      *up;
    char        *line, *nextTok;
    ssize       count;
    int         done, rc;

    assert(packet);

    conn = q->conn;
    rx = conn->rx;
    up = q->queueData;
    if (conn->error) {
        return;
    }
    if (httpGetPacketLength(packet) == 0) {
        if (up->contentState != HTTP_UPLOAD_CONTENT_END) {
            httpError(conn, HTTP_CODE_BAD_REQUEST, "Client supplied insufficient upload data");
        }
        httpPutPacketToNext(q, packet);
        return;
    }
    /*
        Put the packet data onto the service queue for buffering. This aggregates input data incase we don't have
        a complete mime record yet.
     */
    httpJoinPacketForService(q, packet, 0);

    packet = q->first;
    content = packet->content;
    count = httpGetPacketLength(packet);

    for (done = 0, line = 0; !done; ) {
        if  (up->contentState == HTTP_UPLOAD_BOUNDARY || up->contentState == HTTP_UPLOAD_CONTENT_HEADER) {
            /*
                Parse the next input line
             */
            line = mprGetBufStart(content);
            if ((nextTok = memchr(line, '\n', mprGetBufLength(content))) == 0) {
                /* Incomplete line */
                break;
            }
            *nextTok++ = '\0';
            mprAdjustBufStart(content, (int) (nextTok - line));
            line = strim(line, "\r", MPR_TRIM_END);
        }
        switch (up->contentState) {
        case HTTP_UPLOAD_BOUNDARY:
            if (processUploadBoundary(q, line) < 0) {
                done++;
            }
            break;

        case HTTP_UPLOAD_CONTENT_HEADER:
            if (processUploadHeader(q, line) < 0) {
                done++;
            }
            break;

        case HTTP_UPLOAD_CONTENT_DATA:
            rc = processUploadData(q);
            if (rc < 0) {
                done++;
            }
            if (httpGetPacketLength(packet) < up->boundaryLen) {
                /*  Incomplete boundary - return to get more data */
                done++;
            }
            break;

        case HTTP_UPLOAD_CONTENT_END:
            done++;
            break;
        }
    }
    q->count -= (count - httpGetPacketLength(packet));
    assert(q->count >= 0);

    if (httpGetPacketLength(packet) == 0) {
        /*
            Quicker to remove the buffer so the packets don't have to be joined the next time
         */
        httpGetPacket(q);
    } else {
        /*
            Compact the buffer to prevent memory growth. There is often residual data after the boundary for the next block.
         */
        if (packet != rx->headerPacket) {
            mprCompactBuf(content);
        }
    }
}


/*
    Process the mime boundary division
    Returns  < 0 on a request or state error
            == 0 if successful
 */
static int processUploadBoundary(HttpQueue *q, char *line)
{
    HttpConn    *conn;
    Upload      *up;

    conn = q->conn;
    up = q->queueData;

    /*
        Expecting a multipart boundary string
     */
    if (strncmp(up->boundary, line, up->boundaryLen) != 0) {
        httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad upload state. Incomplete boundary");
        return MPR_ERR_BAD_STATE;
    }
    if (line[up->boundaryLen] && strcmp(&line[up->boundaryLen], "--") == 0) {
        up->contentState = HTTP_UPLOAD_CONTENT_END;
    } else {
        up->contentState = HTTP_UPLOAD_CONTENT_HEADER;
    }
    return 0;
}


/*
    Expecting content headers. A blank line indicates the start of the data.
    Returns  < 0  Request or state error
    Returns == 0  Successfully parsed the input line.
 */
static int processUploadHeader(HttpQueue *q, char *line)
{
    HttpConn        *conn;
    HttpRx          *rx;
    HttpUploadFile  *file;
    Upload          *up;
    cchar           *uploadDir;
    char            *key, *headerTok, *rest, *nextPair, *value;

    conn = q->conn;
    rx = conn->rx;
    up = q->queueData;

    if (line[0] == '\0') {
        up->contentState = HTTP_UPLOAD_CONTENT_DATA;
        return 0;
    }

    headerTok = line;
    stok(line, ": ", &rest);

    if (scaselesscmp(headerTok, "Content-Disposition") == 0) {
        /*
            The content disposition header describes either a form
            variable or an uploaded file.

            Content-Disposition: form-data; name="field1"
            >>blank line
            Field Data
            ---boundary

            Content-Disposition: form-data; name="field1" ->
                filename="user.file"
            >>blank line
            File data
            ---boundary
         */
        key = rest;
        up->name = up->clientFilename = 0;
        while (key && stok(key, ";\r\n", &nextPair)) {

            key = strim(key, " ", MPR_TRIM_BOTH);
            stok(key, "= ", &value);
            value = strim(value, "\"", MPR_TRIM_BOTH);

            if (scaselesscmp(key, "form-data") == 0) {
                /* Nothing to do */

            } else if (scaselesscmp(key, "name") == 0) {
                up->name = sclone(value);

            } else if (scaselesscmp(key, "filename") == 0) {
                if (up->name == 0) {
                    httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad upload state. Missing name field");
                    return MPR_ERR_BAD_STATE;
                }
                up->clientFilename = sclone(value);
                /*
                    Create the file to hold the uploaded data
                 */
                uploadDir = getUploadDir(rx->route);
                up->tmpPath = mprGetTempPath(uploadDir);
                if (up->tmpPath == 0) {
                    if (!mprPathExists(uploadDir, X_OK)) {
                        mprLog("http error", 0, "Cannot access upload directory %s", uploadDir);
                    }
                    httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR,
                        "Cannot create upload temp file %s. Check upload temp dir %s", up->tmpPath, uploadDir);
                    return MPR_ERR_CANT_OPEN;
                }
                httpTrace(conn, "request.upload.file", "context", "clientFilename:'%s',filename:'%s'", 
                    up->clientFilename, up->tmpPath);

                up->file = mprOpenFile(up->tmpPath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
                if (up->file == 0) {
                    httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot open upload temp file %s", up->tmpPath);
                    return MPR_ERR_BAD_STATE;
                }
                /*
                    Create the files[id]
                 */
                file = up->currentFile = mprAllocObj(HttpUploadFile, manageHttpUploadFile);
                file->clientFilename = up->clientFilename;
                file->filename = up->tmpPath;
                file->name = up->name;
                httpAddUploadFile(conn, file);
            }
            key = nextPair;
        }

    } else if (scaselesscmp(headerTok, "Content-Type") == 0) {
        if (up->clientFilename) {
            up->currentFile->contentType = sclone(rest);
        }
    }
    return 0;
}


static void manageHttpUploadFile(HttpUploadFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->name);
        mprMark(file->filename);
        mprMark(file->clientFilename);
        mprMark(file->contentType);
    }
}


static void defineFileFields(HttpQueue *q, Upload *up)
{
    HttpConn        *conn;
    HttpUploadFile  *file;
    char            *key;

    conn = q->conn;
    if (conn->tx->handler == conn->http->ejsHandler) {
        /*
            Ejscript manages this for itself
         */
        return;
    }
    up = q->queueData;
    file = up->currentFile;
    key = sjoin("FILE_CLIENT_FILENAME_", up->name, NULL);
    httpSetParam(conn, key, file->clientFilename);

    key = sjoin("FILE_CONTENT_TYPE_", up->name, NULL);
    httpSetParam(conn, key, file->contentType);

    key = sjoin("FILE_FILENAME_", up->name, NULL);
    httpSetParam(conn, key, file->filename);

    key = sjoin("FILE_SIZE_", up->name, NULL);
    httpSetIntParam(conn, key, (int) file->size);
}


static int writeToFile(HttpQueue *q, char *data, ssize len)
{
    HttpConn        *conn;
    HttpUploadFile  *file;
    HttpLimits      *limits;
    Upload          *up;
    ssize           rc;

    conn = q->conn;
    limits = conn->limits;
    up = q->queueData;
    file = up->currentFile;

    if ((file->size + len) > limits->uploadSize) {
        /*
            Abort the connection as we don't want the load of receiving the entire body
         */
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE, "Uploaded file exceeds maximum %'lld", 
            limits->uploadSize);
        return MPR_ERR_CANT_WRITE;
    }
    if (len > 0) {
        /*
            File upload. Write the file data.
         */
        rc = mprWriteFile(up->file, data, len);
        if (rc != len) {
            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR,
                "Cannot write to upload temp file %s, rc %zd, errno %d", up->tmpPath, rc, mprGetOsError());
            return MPR_ERR_CANT_WRITE;
        }
        file->size += len;
        conn->rx->bytesUploaded += len;
    }
    return 0;
}


/*
    Process the content data.
    Returns < 0 on error
            == 0 when more data is needed
            == 1 when data successfully written
 */
static int processUploadData(HttpQueue *q)
{
    HttpConn        *conn;
    HttpPacket      *packet;
    MprBuf          *content;
    Upload          *up;
    ssize           size, dataLen;
    bool            pureData;
    char            *data, *bp, *key;

    conn = q->conn;
    up = q->queueData;
    content = q->first->content;
    packet = 0;

    size = mprGetBufLength(content);
    if (size < up->boundaryLen) {
        /*  Incomplete boundary. Return and get more data */
        return 0;
    }
    bp = getBoundary(mprGetBufStart(content), size, up->boundary, up->boundaryLen, &pureData);
    if (bp == 0) {
        if (up->clientFilename) {
            /*
                No signature found yet. probably more data to come. Must handle split boundaries.
             */
            data = mprGetBufStart(content);
            dataLen = pureData ? size : (size - (up->boundaryLen - 1));
            if (dataLen > 0) {
                if (writeToFile(q, mprGetBufStart(content), dataLen) < 0) {
                    return MPR_ERR_CANT_WRITE;
                }
            }
            mprAdjustBufStart(content, dataLen);
            return 0;       /* Get more data */
        }
    }
    data = mprGetBufStart(content);
    dataLen = (bp) ? (bp - data) : mprGetBufLength(content);

    if (dataLen > 0) {
        mprAdjustBufStart(content, dataLen);
        /*
            This is the CRLF before the boundary
         */
        if (dataLen >= 2 && data[dataLen - 2] == '\r' && data[dataLen - 1] == '\n') {
            dataLen -= 2;
        }
        if (up->clientFilename) {
            /*
                Write the last bit of file data and add to the list of files and define environment variables
             */
            if (writeToFile(q, data, dataLen) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            defineFileFields(q, up);

        } else {
            /*
                Normal string form data variables
             */
            data[dataLen] = '\0';
#if KEEP
            httpTrace(conn, "request.upload.variables", "context", "'%s':'%s'", up->name, data);
#endif
            key = mprUriDecode(up->name);
            data = mprUriDecode(data);
            httpSetParam(conn, key, data);

            if (packet == 0) {
                packet = httpCreatePacket(ME_MAX_BUFFER);
            }
            if (httpGetPacketLength(packet) > 0) {
                /*
                    Need to add www-form-urlencoding separators
                 */
                mprPutCharToBuf(packet->content, '&');
            } else {
                conn->rx->mimeType = sclone("application/x-www-form-urlencoded");

            }
            mprPutToBuf(packet->content, "%s=%s", up->name, data);
        }
    }
    if (up->clientFilename) {
        /*
            Now have all the data (we've seen the boundary)
         */
        mprCloseFile(up->file);
        up->file = 0;
        up->clientFilename = 0;
    }
    if (packet) {
        httpPutPacketToNext(q, packet);
    }
    up->contentState = HTTP_UPLOAD_BOUNDARY;
    return 0;
}


/*
    Find the boundary signature in memory. Returns pointer to the first match.
 */
static char *getBoundary(char *buf, ssize bufLen, char *boundary, ssize boundaryLen, bool *pureData)
{
    char    *cp, *endp;
    char    first;

    assert(buf);
    assert(boundary);
    assert(boundaryLen > 0);

    first = *boundary & 0xff;
    endp = &buf[bufLen];

    for (cp = buf; cp < endp; cp++) {
        if ((cp = memchr(cp, first, endp - cp)) == 0) {
            *pureData = 1;
            return 0;
        }
        /* Potential boundary */
        if ((endp - cp) < boundaryLen) {
            *pureData = 0;
            return 0;
        }
        if (memcmp(cp, boundary, boundaryLen) == 0) {
            *pureData = 0;
            return cp;
        }
    }
    *pureData = 0;
    return 0;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/uri.c ************/


/*
    uri.c - URI manipulation routines
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static cchar *expandRouteName(HttpConn *conn, cchar *routeName);
static int getPort(HttpUri *uri);
static int getDefaultPort(cchar *scheme);
static void manageUri(HttpUri *uri, int flags);
static void trimPathToDirname(HttpUri *uri);
static char *actionRoute(HttpRoute *route, cchar *controller, cchar *action);

/************************************ Code ************************************/
/*
    Create and initialize a URI. This accepts full URIs with schemes (http:) and partial URLs
    Support IPv4 and [IPv6]. Supported forms:

        SCHEME://[::]:PORT/URI
        SCHEME://HOST:PORT/URI
        [::]:PORT/URI
        :PORT/URI
        HOST:PORT/URI
        PORT/URI
        /URI
        URI

        NOTE: the following is not supported and requires a scheme prefix. This is because it is ambiguous with Uri path.
        HOST/URI

    Missing fields are null or zero.
 */
PUBLIC HttpUri *httpCreateUri(cchar *uri, int flags)
{
    HttpUri     *up;
    char        *tok, *next;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    tok = up->uri = sclone(uri);

    /*
        [scheme://][hostname[:port]][/path[.ext]][#ref][?query]
        First trim query and then reference from the end
     */
    if ((next = schr(tok, '?')) != 0) {
        *next++ = '\0';
        up->query = sclone(next);
    }
    if ((next = schr(tok, '#')) != 0) {
        *next++ = '\0';
        up->reference = sclone(next);
    }

    /*
        [scheme://][hostname[:port]][/path]
     */
    if ((next = scontains(tok, "://")) != 0) {
        up->scheme = snclone(tok, (next - tok));
        if (smatch(up->scheme, "http")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 80;
            }
        } else if (smatch(up->scheme, "ws")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 80;
            }
            up->webSockets = 1;
        } else if (smatch(up->scheme, "https")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 443;
            }
            up->secure = 1;
        } else if (smatch(up->scheme, "wss")) {
            if (flags & HTTP_COMPLETE_URI) {
                up->port = 443;
            }
            up->secure = 1;
            up->webSockets = 1;
        }
        tok = &next[3];
    }

    /*
        [hostname[:port]][/path]
     */
    if (*tok == '[' && ((next = strchr(tok, ']')) != 0)) {
        /* IPv6  [::]:port/uri */
        up->host = snclone(&tok[1], (next - tok) - 1);
        tok = ++next;

    } else if (*tok && *tok != '/' && *tok != ':' && (up->scheme || strchr(tok, ':'))) {
        /*
            Supported forms:
                scheme://hostname
                hostname:port
         */
        if ((next = spbrk(tok, ":/")) == 0) {
            next = &tok[slen(tok)];
        }
        up->host = snclone(tok, next - tok);
        tok = next;
    }
    assert(tok);

    /* [:port][/path] */
    if (*tok == ':') {
        up->port = atoi(++tok);
        if ((tok = schr(tok, '/')) == 0) {
            tok = "";
        }
    }
    assert(tok);

    /* [/path] */
    if (*tok) {
        up->path = sclone(tok);
        /* path[.ext[/extra]] */
        if ((tok = srchr(up->path, '.')) != 0) {
            if (tok[1]) {
                if ((next = srchr(up->path, '/')) != 0) {
                    if (next < tok) {
                        up->ext = sclone(++tok);
                    }
                } else {
                    up->ext = sclone(++tok);
                }
            }
        }
    }
    if (flags & (HTTP_COMPLETE_URI | HTTP_COMPLETE_URI_PATH)) {
        if (up->path == 0 || *up->path == '\0') {
            up->path = sclone("/");
        }
    }
    if (flags & HTTP_COMPLETE_URI) {
        if (!up->scheme) {
            up->scheme = sclone("http");
        }
        if (!up->host) {
            up->host = sclone("localhost");
        }
        if (!up->port) {
            up->port = 80;
        }
    }
    return up;
}


static void manageUri(HttpUri *uri, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(uri->scheme);
        mprMark(uri->host);
        mprMark(uri->path);
        mprMark(uri->ext);
        mprMark(uri->reference);
        mprMark(uri->query);
        mprMark(uri->uri);
    }
}


/*
    Create and initialize a URI. This accepts full URIs with schemes (http:) and partial URLs
 */
PUBLIC HttpUri *httpCreateUriFromParts(cchar *scheme, cchar *host, int port, cchar *path, cchar *reference, cchar *query, 
        int flags)
{
    HttpUri     *up;
    char        *cp, *tok;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    if (scheme) {
        up->scheme = sclone(scheme);
        up->secure = (smatch(up->scheme, "https") || smatch(up->scheme, "wss"));
        up->webSockets = (smatch(up->scheme, "ws") || smatch(up->scheme, "wss"));
    } else if (flags & HTTP_COMPLETE_URI) {
        up->scheme = "http";
    }
    if (host) {
        if (*host == '[' && ((cp = strchr(host, ']')) != 0)) {
            up->host = snclone(&host[1], (cp - host) - 2);
            if ((cp = schr(++cp, ':')) && port == 0) {
                port = (int) stoi(++cp);
            }
        } else {
            up->host = sclone(host);
            if ((cp = schr(up->host, ':')) && port == 0) {
                port = (int) stoi(++cp);
            }
        }
    } else if (flags & HTTP_COMPLETE_URI) {
        up->host = sclone("localhost");
    }
    if (port) {
        up->port = port;
    }
    if (path) {
        while (path[0] == '/' && path[1] == '/') {
            path++;
        }
        up->path = sclone(path);
    }
    if (flags & (HTTP_COMPLETE_URI | HTTP_COMPLETE_URI_PATH)) {
        if (up->path == 0 || *up->path == '\0') {
            up->path = sclone("/");
        }
    }
    if (reference) {
        up->reference = sclone(reference);
    }
    if (query) {
        up->query = sclone(query);
    }
    if ((tok = srchr(up->path, '.')) != 0) {
        if ((cp = srchr(up->path, '/')) != 0) {
            if (cp <= tok) {
                up->ext = sclone(&tok[1]);
            }
        } else {
            up->ext = sclone(&tok[1]);
        }
    }
    return up;
}


PUBLIC HttpUri *httpCloneUri(HttpUri *base, int flags)
{
    HttpUri     *up;
    char        *path, *cp, *tok;

    if ((up = mprAllocObj(HttpUri, manageUri)) == 0) {
        return 0;
    }
    if (base->scheme) {
        up->scheme = sclone(base->scheme);
    } else if (flags & HTTP_COMPLETE_URI) {
        up->scheme = sclone("http");
    }
    up->secure = (smatch(up->scheme, "https") || smatch(up->scheme, "wss"));
    up->webSockets = (smatch(up->scheme, "ws") || smatch(up->scheme, "wss"));
    if (base->host) {
        up->host = sclone(base->host);
    } else if (flags & HTTP_COMPLETE_URI) {
        up->host = sclone("localhost");
    }
    if (base->port) {
        up->port = base->port;
    } else if (flags & HTTP_COMPLETE_URI) {
        up->port = (smatch(up->scheme, "https") || smatch(up->scheme, "wss"))? 443 : 80;
    }
    path = base->path;
    if (path) {
        while (path[0] == '/' && path[1] == '/') {
            path++;
        }
        up->path = sclone(path);
    }
    if (flags & (HTTP_COMPLETE_URI | HTTP_COMPLETE_URI_PATH)) {
        if (up->path == 0 || *up->path == '\0') {
            up->path = sclone("/");
        }
    }
    if (base->reference) {
        up->reference = sclone(base->reference);
    }
    if (base->query) {
        up->query = sclone(base->query);
    }
    if (up->path && (tok = srchr(up->path, '.')) != 0) {
        if ((cp = srchr(up->path, '/')) != 0) {
            if (cp <= tok) {
                up->ext = sclone(&tok[1]);
            }
        } else {
            up->ext = sclone(&tok[1]);
        }
    }
    return up;
}


/*
    Complete the "uri" using missing parts from base
 */
PUBLIC HttpUri *httpCompleteUri(HttpUri *uri, HttpUri *base)
{
    if (!base) {
        if (!uri->scheme) {
            uri->scheme = sclone("http");
        }
        if (!uri->host) {
            uri->host = sclone("localhost");
        }
        if (!uri->path) {
            uri->path = sclone("/");
        }
    } else {
        if (!uri->host) {
            uri->host = base->host;
            if (!uri->port) {
                uri->port = base->port;
            }
        }
        if (!uri->scheme) {
            uri->scheme = base->scheme;
        }
        if (!uri->path) {
            uri->path = base->path;
            if (!uri->query) {
                uri->query = base->query;
            }
            if (!uri->reference) {
                uri->reference = base->reference;
            }
        }
    }
    uri->secure = (smatch(uri->scheme, "https") || smatch(uri->scheme, "wss"));
    uri->webSockets = (smatch(uri->scheme, "ws") || smatch(uri->scheme, "wss"));
    return uri;
}


/*
    Format a string URI from parts
 */
PUBLIC char *httpFormatUri(cchar *scheme, cchar *host, int port, cchar *path, cchar *reference, cchar *query, int flags)
{
    char    *uri;
    cchar   *portStr, *hostDelim, *portDelim, *pathDelim, *queryDelim, *referenceDelim, *cp;

    portDelim = "";
    portStr = "";

    if ((flags & HTTP_COMPLETE_URI) || host || scheme) {
        if (scheme == 0 || *scheme == '\0') {
            scheme = "http";
        }
        if (host == 0 || *host == '\0') {
            if (port || path || reference || query) {
                host = "localhost";
            }
        }
        hostDelim = "://";
    } else {
        host = hostDelim = "";
    }
    if (host) {
        if (mprIsIPv6(host)) {
            if (*host != '[') {
                host = sfmt("[%s]", host);
            } else if ((cp = scontains(host, "]:")) != 0) {
                port = 0;
            }
        } else if (schr(host, ':')) {
            port = 0;
        }
    }
    if (port != 0 && port != getDefaultPort(scheme)) {
        portStr = itos(port);
        portDelim = ":";
    }
    if (scheme == 0) {
        scheme = "";
    }
    if (path && *path) {
        if (*hostDelim) {
            pathDelim = (*path == '/') ? "" :  "/";
        } else {
            pathDelim = "";
        }
    } else {
        pathDelim = path = "";
    }
    if (reference && *reference) {
        referenceDelim = "#";
    } else {
        referenceDelim = reference = "";
    }
    if (query && *query) {
        queryDelim = "?";
    } else {
        queryDelim = query = "";
    }
    if (portDelim) {
        uri = sjoin(scheme, hostDelim, host, portDelim, portStr, pathDelim, path, referenceDelim, reference, queryDelim, query, NULL);
    } else {
        uri = sjoin(scheme, hostDelim, host, pathDelim, path, referenceDelim, reference, queryDelim, query, NULL);
    }
    return uri;
}


/*
    This returns a URI relative to the base for the given target

    uri = target.relative(base)
 */
PUBLIC HttpUri *httpGetRelativeUri(HttpUri *base, HttpUri *target, int clone)
{
    HttpUri     *uri;
    char        *basePath, *bp, *cp, *tp, *startDiff;
    int         i, baseSegments, commonSegments;

    if (target == 0) {
        return (clone) ? httpCloneUri(base, 0) : base;
    }
    if (!(target->path && target->path[0] == '/') || !((base->path && base->path[0] == '/'))) {
        /* If target is relative, just use it. If base is relative, cannot use it because we don't know where it is */
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if (base->scheme && target->scheme && scmp(base->scheme, target->scheme) != 0) {
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if (base->host && target->host && (base->host && scmp(base->host, target->host) != 0)) {
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if (getPort(base) != getPort(target)) {
        return (clone) ? httpCloneUri(target, 0) : target;
    }
    if ((basePath = httpNormalizeUriPath(base->path)) == 0) {
        return 0;
    }
    /* Count trailing "/" */
    for (baseSegments = 0, bp = basePath; *bp; bp++) {
        if (*bp == '/') {
            baseSegments++;
        }
    }

    /*
        Find portion of path that matches the base, if any.
     */
    commonSegments = 0;
    for (bp = base->path, tp = startDiff = target->path; *bp && *tp; bp++, tp++) {
        if (*bp == '/') {
            if (*tp == '/') {
                commonSegments++;
                startDiff = tp;
            }
        } else {
            if (*bp != *tp) {
                break;
            }
        }
    }
    if (*startDiff == '/') {
        startDiff++;
    }

    if ((uri = httpCloneUri(target, 0)) == 0) {
        return 0;
    }
    uri->host = 0;
    uri->scheme = 0;
    uri->port = 0;

    uri->path = cp = mprAlloc(baseSegments * 3 + (int) slen(target->path) + 2);
    for (i = commonSegments; i < baseSegments; i++) {
        *cp++ = '.';
        *cp++ = '.';
        *cp++ = '/';
    }
    if (*startDiff) {
        strcpy(cp, startDiff);
    } else if (cp > uri->path) {
        /*
            Cleanup trailing separators ("../" is the end of the new path)
         */
        cp[-1] = '\0';
    } else {
        strcpy(uri->path, ".");
    }
    return uri;
}


//  FUTURE - rethink API, makes chaining hard if result must be supplied
/*
    result = base.join(other)
 */
PUBLIC HttpUri *httpJoinUriPath(HttpUri *result, HttpUri *base, HttpUri *other)
{
    char    *sep;

    if (other->path) {
        if (other->path[0] == '/') {
            result->path = sclone(other->path);
        } else {
            sep = ((base->path[0] == '\0' || base->path[slen(base->path) - 1] == '/') || 
                   (other->path[0] == '\0' || other->path[0] == '/'))  ? "" : "/";
            result->path = sjoin(base->path, sep, other->path, NULL);
        }
    }
    return result;
}


PUBLIC HttpUri *httpJoinUri(HttpUri *uri, int argc, HttpUri **others)
{
    HttpUri     *other;
    int         i;

    uri = httpCloneUri(uri, 0);

    for (i = 0; i < argc; i++) {
        other = others[i];
        if (other->scheme) {
            uri->scheme = sclone(other->scheme);
            uri->port = other->port;
        }
        if (other->host) {
            uri->host = sclone(other->host);
            uri->port = other->port;
        }
        if (other->path) {
            httpJoinUriPath(uri, uri, other);
        }
        if (other->reference) {
            uri->reference = sclone(other->reference);
        }
        if (other->query) {
            uri->query = sclone(other->query);
        }
    }
    uri->ext = mprGetPathExt(uri->path);
    return uri;
}


/*
    Create and resolve a URI link given a set of options.
 */
PUBLIC HttpUri *httpMakeUriLocal(HttpUri *uri)
{
    if (uri) {
        uri->host = 0;
        uri->scheme = 0;
        uri->port = 0;
    }
    return uri;
}


PUBLIC HttpUri *httpNormalizeUri(HttpUri *uri)
{
    uri->path = httpNormalizeUriPath(uri->path);
    return uri;
}


/*
    Normalize a URI path to remove redundant "./" and cleanup "../" and make separator uniform. Does not make an abs path.
    It does not map separators nor change case. 
 */
PUBLIC char *httpNormalizeUriPath(cchar *pathArg)
{
    char    *dupPath, *path, *sp, *dp, *mark, **segments;
    int     firstc, j, i, nseg, len;

    if (pathArg == 0 || *pathArg == '\0') {
        return mprEmptyString();
    }
    len = (int) slen(pathArg);
    if ((dupPath = mprAlloc(len + 2)) == 0) {
        return 0;
    }
    strcpy(dupPath, pathArg);

    if ((segments = mprAlloc(sizeof(char*) * (len + 1))) == 0) {
        return 0;
    }
    nseg = len = 0;
    firstc = *dupPath;
    for (mark = sp = dupPath; *sp; sp++) {
        if (*sp == '/') {
            *sp = '\0';
            while (sp[1] == '/') {
                sp++;
            }
            segments[nseg++] = mark;
            len += (int) (sp - mark);
            mark = sp + 1;
        }
    }
    segments[nseg++] = mark;
    len += (int) (sp - mark);
    for (j = i = 0; i < nseg; i++, j++) {
        sp = segments[i];
        if (sp[0] == '.') {
            if (sp[1] == '\0')  {
                if ((i+1) == nseg) {
                    /* Trim trailing "." */
                    segments[j] = "";
                } else {
                    /* Trim intermediate "." */
                    j--;
                }
            } else if (sp[1] == '.' && sp[2] == '\0')  {
                j = max(j - 2, -1);
                if ((i+1) == nseg) {
                    nseg--;
                }
            } else {
                /* .more-chars */
                segments[j] = segments[i];
            }
        } else {
            segments[j] = segments[i];
        }
    }
    nseg = j;
    assert(nseg >= 0);
    if ((path = mprAlloc(len + nseg + 1)) != 0) {
        for (i = 0, dp = path; i < nseg; ) {
            strcpy(dp, segments[i]);
            len = (int) slen(segments[i]);
            dp += len;
            if (++i < nseg || (nseg == 1 && *segments[0] == '\0' && firstc == '/')) {
                *dp++ = '/';
            }
        }
        *dp = '\0';
    }
    return path;
}


PUBLIC HttpUri *httpResolveUri(HttpUri *base, int argc, HttpUri **others, bool local)
{
    HttpUri     *current, *other;
    int         i;

    if ((current = httpCloneUri(base, 0)) == 0) {
        return 0;
    }
    if (local) {
        current->host = 0;
        current->scheme = 0;
        current->port = 0;
    }
    /*
        Must not inherit the query or reference
     */
    current->query = 0;
    current->reference = 0;

    for (i = 0; i < argc; i++) {
        other = others[i];
        if (other->scheme && !smatch(current->scheme, other->scheme)) {
            current->scheme = sclone(other->scheme);
            /*
                If the scheme is changed (test above), then accept an explict port.
                If no port, then must not use the current port as the scheme has changed.
             */
            if (other->port) {
                current->port = other->port;
            } else if (current->port) {
                current->port = 0;
            }
        }
        if (other->host) {
            current->host = sclone(other->host);
        }
        if (other->port) {
            current->port = other->port;
        }
        if (other->path) {
            trimPathToDirname(current);
            httpJoinUriPath(current, current, other);
        }
        if (other->reference) {
            current->reference = sclone(other->reference);
        }
        if (other->query) {
            current->query = sclone(other->query);
        }
    }
    current->ext = mprGetPathExt(current->path);
    return current;
}


PUBLIC HttpUri *httpLinkUri(HttpConn *conn, cchar *target, MprHash *options)
{
    HttpRoute       *route, *lroute;
    HttpRx          *rx;
    HttpUri         *uri;
    cchar           *routeName, *action, *controller, *originalAction, *tplate;
    char            *rest;

    rx = conn->rx;
    route = rx->route;
    controller = 0;

    if (target == 0) {
        target = "";
    }
    if (*target == '@') {
        target = sjoin("{action: '", target, "'}", NULL);
    } 
    if (*target != '{') {
        tplate = target;
        if (!options) {
            options = route->vars;
        }
    } else  {
        if (options) {
            options = mprBlendHash(httpGetOptions(target), options);
        } else {
            options = httpGetOptions(target);
        }
        options = mprBlendHash(options, route->vars);

        /*
            Prep the action. Forms are:
                . @action               # Use the current controller
                . @controller/          # Use "index" as the action
                . @controller/action
         */
        if ((action = httpGetOption(options, "action", 0)) != 0) {
            originalAction = action;
            if (*action == '@') {
                action = &action[1];
            }
            if (strchr(action, '/')) {
                controller = stok((char*) action, "/", (char**) &action);
                action = stok((char*) action, "/", &rest);
            }
            if (controller) {
                httpSetOption(options, "controller", controller);
            } else {
                controller = httpGetParam(conn, "controller", 0);
            }
            if (action == 0 || *action == '\0') {
                action = "list";
            }
            if (action != originalAction) {
                httpSetOption(options, "action", action);
            }
        }
        /*
            Find the template to use. Strategy is this order:
                . options.template
                . options.route.template
                . options.action mapped to a route.template, via:
                . /app/STAR/action
                . /app/controller/action
                . /app/STAR/default
                . /app/controller/default
         */
        if ((tplate = httpGetOption(options, "template", 0)) == 0) {
            if ((routeName = httpGetOption(options, "route", 0)) != 0) {
                routeName = expandRouteName(conn, routeName);
                lroute = httpLookupRoute(conn->host, routeName);
            } else {
                lroute = 0;
            }
            if (!lroute) {
                if ((lroute = httpLookupRoute(conn->host, actionRoute(route, controller, action))) == 0) {
                    if ((lroute = httpLookupRoute(conn->host, actionRoute(route, "{controller}", action))) == 0) {
                        if ((lroute = httpLookupRoute(conn->host, actionRoute(route, controller, "default"))) == 0) {
                            lroute = httpLookupRoute(conn->host, actionRoute(route, "{controller}", "default"));
                        }
                    }
                }
            }
            if (lroute) {
                tplate = lroute->tplate;
            }
        }
        if (!tplate) {
            mprLog("error http", 0, "Cannot find template for URI %s", target);
            target = "/";
        }
    }
    //  OPT
    target = httpTemplate(conn, tplate, options);
    uri = httpCreateUri(target, 0);
    /*
        This was changed from: httpCreateUri(rx->uri) to rx->parsedUri.
        The use case was appweb: /auth/form/login which redirects using: https:///auth/form/login on localhost:4443
        This must extract the existing host and port from the prior request
     */
    uri = httpResolveUri(rx->parsedUri, 1, &uri, 0);
    return httpNormalizeUri(uri);
}


PUBLIC char *httpLink(HttpConn *conn, cchar *target)
{
    return httpLinkEx(conn, target, 0);
}


PUBLIC char *httpLinkEx(HttpConn *conn, cchar *target, MprHash *options)
{
    return httpUriToString(httpLinkUri(conn, target, options), 0);
}


PUBLIC char *httpUriToString(HttpUri *uri, int flags)
{
    return httpFormatUri(uri->scheme, uri->host, uri->port, uri->path, uri->reference, uri->query, flags);
}


/*
    Validate a URI path for use in a HTTP request line
    The URI must contain only valid characters and must being with "/" both before and after decoding.
    A decoded, normalized URI path is returned.
 */
PUBLIC char *httpValidateUriPath(cchar *uri)
{
    char    *up;

    if (uri == 0 || *uri != '/') {
        return 0;
    }
    if (!httpValidUriChars(uri)) {
        return 0;
    }
    up = mprUriDecode(uri);
    if ((up = httpNormalizeUriPath(up)) == 0) {
        return 0;
    }
    if (*up != '/' || strchr(up, '\\')) {
        return 0;
    }
    return up;
}


/*
    This tests if the URI has only characters valid to use in a URI before decoding. i.e. It will permit %NN encodings.
 */
PUBLIC bool httpValidUriChars(cchar *uri)
{
    ssize   pos;

    if (uri == 0 || *uri == 0) {
        return 1;
    }
    pos = strspn(uri, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=%");
    if (pos < slen(uri)) {
        return 0;
    }
    return 1;
}


static int getPort(HttpUri *uri)
{
    if (uri->port) {
        return uri->port;
    }
    return (uri->scheme && (smatch(uri->scheme, "https") || smatch(uri->scheme, "wss"))) ? 443 : 80;
}


static int getDefaultPort(cchar *scheme)
{
    return (scheme && (smatch(scheme, "https") || smatch(scheme, "wss"))) ? 443 : 80;
}


static void trimPathToDirname(HttpUri *uri) 
{
    char        *path, *cp;
    int         len;

    path = uri->path;
    len = (int) slen(path);
    if (path[len - 1] == '/') {
        if (len > 1) {
            path[len - 1] = '\0';
        }
    } else {
        if ((cp = srchr(path, '/')) != 0) {
            if (cp > path) {
                *cp = '\0';
            } else {
                cp[1] = '\0';
            }
        } else if (*path) {
            path[0] = '\0';
        }
    }
}


/*
    Limited expansion of route names. Support ~/, |/ and ${app} at the start of the route name
 */
static cchar *expandRouteName(HttpConn *conn, cchar *routeName)
{
    HttpRoute   *route;

    route = conn->rx->route;
    if (routeName[0] == '~') {
        return sjoin(route->prefix, &routeName[1], NULL);
    }
    if (sstarts(routeName, "${app}")) {
        return sjoin(route->prefix, &routeName[6], NULL);
    }
    if (routeName[0] == ME_SERVER_PREFIX_CHAR) {
        return sjoin(route->prefix, route->serverPrefix, &routeName[1], NULL);
    }
    return routeName;
}


/*
    Calculate a qualified route name. The form is: /{app}/{controller}/action
 */
static char *actionRoute(HttpRoute *route, cchar *controller, cchar *action)
{
    cchar   *controllerPrefix;

    if (action == 0 || *action == '\0') {
        action = "default";
    }
    if (controller) {
        controllerPrefix = (controller && smatch(controller, "{controller}")) ? "*" : controller;
        return sjoin(route->prefix, route->serverPrefix, "/", controllerPrefix, "/", action, NULL);
    } else {
        return sjoin(route->prefix, route->serverPrefix, "/", action, NULL);
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/user.c ************/


/*
    user.c - User and Role management

    An internal cache of users is kept for authenticated users.
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************* Forwards ***********************************/

#undef  GRADUATE_HASH
#define GRADUATE_HASH(auth, field) \
    if (!auth->field) { \
        if (auth->parent && auth->field && auth->field == auth->parent->field) { \
            auth->field = mprCloneHash(auth->parent->field); \
        } else { \
            auth->field = mprCreateHash(0, MPR_HASH_STABLE); \
        } \
    }

static void manageRole(HttpRole *role, int flags);
static void manageUser(HttpUser *user, int flags);

/*********************************** Code *************************************/

static void manageRole(HttpRole *role, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(role->name);
        mprMark(role->abilities);
    }
}


PUBLIC HttpRole *httpAddRole(HttpAuth *auth, cchar *name, cchar *abilities)
{
    HttpRole    *role;
    char        *ability, *tok;

    GRADUATE_HASH(auth, roles);
    if ((role = mprLookupKey(auth->roles, name)) == 0) {
        if ((role = mprAllocObj(HttpRole, manageRole)) == 0) {
            return 0;
        }
        role->name = sclone(name);
    }
    role->abilities = mprCreateHash(0, 0);
    for (ability = stok(sclone(abilities), " \t", &tok); ability; ability = stok(NULL, " \t", &tok)) {
        mprAddKey(role->abilities, ability, role);
    }
    if (mprAddKey(auth->roles, name, role) == 0) {
        return 0;
    }
    mprDebug("http auth", 5, "Role \"%s\" defined, abilities=\"%s\"", role->name, abilities);
    return role;
}


/*
    Compute a set of abilities for a role. Role strings can be either roles or abilities.
    The abilities hash is updated.
 */
PUBLIC void httpComputeRoleAbilities(HttpAuth *auth, MprHash *abilities, cchar *role)
{
    MprKey      *ap;
    HttpRole    *rp;

    if ((rp = mprLookupKey(auth->roles, role)) != 0) {
        /* Interpret as a role */
        for (ITERATE_KEYS(rp->abilities, ap)) {
            if (!mprLookupKey(abilities, ap->key)) {
                mprAddKey(abilities, ap->key, MPR->oneString);
            }
        }
    } else {
        /* Not found as a role: Interpret role as an ability */
        mprAddKey(abilities, role, MPR->oneString);
    }
}


/*
    Compute the set of user abilities from the user roles. User ability strings can be either roles or abilities. Expand
    roles into the equivalent set of abilities.
 */
PUBLIC void httpComputeUserAbilities(HttpAuth *auth, HttpUser *user)
{
    char        *ability, *tok;

    user->abilities = mprCreateHash(0, 0);
    for (ability = stok(sclone(user->roles), " \t,", &tok); ability; ability = stok(NULL, " \t,", &tok)) {
        httpComputeRoleAbilities(auth, user->abilities, ability);
    }
}


/*
    Recompute all user abilities. Used if the role definitions change
 */
PUBLIC void httpComputeAllUserAbilities(HttpAuth *auth)
{
    MprKey      *kp;
    HttpUser    *user;

    for (ITERATE_KEY_DATA(auth->userCache, kp, user)) {
        httpComputeUserAbilities(auth, user);
    }
}


PUBLIC char *httpRolesToAbilities(HttpAuth *auth, cchar *roles, cchar *separator)
{
    MprKey      *ap;
    HttpRole    *rp;
    MprBuf      *buf;
    char        *role, *tok;

    buf = mprCreateBuf(0, 0);
    for (role = stok(sclone(roles), " \t,", &tok); role; role = stok(NULL, " \t,", &tok)) {
        if ((rp = mprLookupKey(auth->roles, role)) != 0) {
            /* Interpret as a role */
            for (ITERATE_KEYS(rp->abilities, ap)) {
                mprPutStringToBuf(buf, ap->key);
                mprPutStringToBuf(buf, separator);
            }
        } else {
            /* Not found as a role: Interpret role as an ability */
            mprPutStringToBuf(buf, role);
            mprPutStringToBuf(buf, separator);
        }
    }
    if (mprGetBufLength(buf) > 0) {
        mprAdjustBufEnd(buf, - slen(separator));
        mprAddNullToBuf(buf);
    }
    return mprBufToString(buf);
}


PUBLIC HttpRole *httpLookupRole(HttpAuth *auth, cchar *role)
{
    return mprLookupKey(auth->roles, role);
}


PUBLIC int httpRemoveRole(HttpAuth *auth, cchar *role)
{
    if (auth->roles == 0 || !mprLookupKey(auth->roles, role)) {
        return MPR_ERR_CANT_ACCESS;
    }
    mprRemoveKey(auth->roles, role);
    return 0;
}


static void manageUser(HttpUser *user, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(user->password);
        mprMark(user->name);
        mprMark(user->abilities);
        mprMark(user->roles);
    }
}


PUBLIC HttpUser *httpAddUser(HttpAuth *auth, cchar *name, cchar *password, cchar *roles)
{
    HttpUser    *user;

    if (!auth->userCache) {
        auth->userCache = mprCreateHash(0, 0);
    }
    if ((user = mprLookupKey(auth->userCache, name)) == 0) {
        if ((user = mprAllocObj(HttpUser, manageUser)) == 0) {
            return 0;
        }
        user->name = sclone(name);
    }
    user->password = sclone(password);
    if (roles) {
        user->roles = sclone(roles);
        httpComputeUserAbilities(auth, user);
    }
    if (mprAddKey(auth->userCache, name, user) == 0) {
        return 0;
    }
    return user;
}


PUBLIC HttpUser *httpLookupUser(HttpAuth *auth, cchar *name)
{
    return mprLookupKey(auth->userCache, name);
}


PUBLIC int httpRemoveUser(HttpAuth *auth, cchar *name)
{
    if (!mprLookupKey(auth->userCache, name)) {
        return MPR_ERR_CANT_ACCESS;
    }
    mprRemoveKey(auth->userCache, name);
    return 0;
}


PUBLIC void httpSetConnUser(HttpConn *conn, HttpUser *user)
{
    conn->user = user;
}

#undef  GRADUATE_HASH

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/var.c ************/


/*
    var.c -- Manage the request variables
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/********************************** Defines ***********************************/

#define HTTP_VAR_HASH_SIZE  61           /* Hash size for vars and params */

/*********************************** Code *************************************/
/*
    Define standard CGI variables
 */
PUBLIC void httpCreateCGIParams(HttpConn *conn)
{
    HttpRx          *rx;
    HttpTx          *tx;
    HttpHost        *host;
    HttpUploadFile  *file;
    MprSocket       *sock;
    MprHash         *svars;
    MprJson         *params;
    int             index;

    rx = conn->rx;
    if ((svars = rx->svars) != 0) {
        /* Do only once */
        return;
    }
    svars = rx->svars = mprCreateHash(HTTP_VAR_HASH_SIZE, MPR_HASH_STABLE);
    tx = conn->tx;
    host = conn->host;
    sock = conn->sock;

    mprAddKey(svars, "ROUTE_HOME", rx->route->home);

    mprAddKey(svars, "AUTH_TYPE", conn->authType);
    mprAddKey(svars, "AUTH_USER", conn->username);
    mprAddKey(svars, "AUTH_ACL", MPR->emptyString);
    mprAddKey(svars, "CONTENT_LENGTH", rx->contentLength);
    mprAddKey(svars, "CONTENT_TYPE", rx->mimeType);
    mprAddKey(svars, "DOCUMENTS", rx->route->documents);
    mprAddKey(svars, "GATEWAY_INTERFACE", sclone("CGI/1.1"));
    mprAddKey(svars, "QUERY_STRING", rx->parsedUri->query);
    mprAddKey(svars, "REMOTE_ADDR", conn->ip);
    mprAddKeyFmt(svars, "REMOTE_PORT", "%d", conn->port);

    /* 
        Set to the same as AUTH_USER 
     */
    mprAddKey(svars, "REMOTE_USER", conn->username);
    mprAddKey(svars, "REQUEST_METHOD", rx->method);
    mprAddKey(svars, "REQUEST_TRANSPORT", sclone((char*) ((conn->secure) ? "https" : "http")));
    mprAddKey(svars, "SERVER_ADDR", sock->acceptIp);
    mprAddKey(svars, "SERVER_NAME", host->name);
    mprAddKeyFmt(svars, "SERVER_PORT", "%d", sock->acceptPort);
    mprAddKey(svars, "SERVER_PROTOCOL", conn->protocol);
    mprAddKey(svars, "SERVER_SOFTWARE", conn->http->software);

    /*
        For PHP, REQUEST_URI must be the original URI. The SCRIPT_NAME will refer to the new pathInfo
     */
    mprAddKey(svars, "REQUEST_URI", rx->originalUri);

    /*
        URIs are broken into the following: http://{SERVER_NAME}:{SERVER_PORT}{SCRIPT_NAME}{PATH_INFO} 
        NOTE: Appweb refers to pathInfo as the app relative URI and scriptName as the app address before the pathInfo.
        In CGI|PHP terms, the scriptName is the appweb rx->pathInfo and the PATH_INFO is the extraPath. 
     */
    mprAddKey(svars, "PATH_INFO", rx->extraPath);
    mprAddKeyFmt(svars, "SCRIPT_NAME", "%s%s", rx->scriptName, rx->pathInfo);
    mprAddKey(svars, "SCRIPT_FILENAME", tx->filename);
    if (rx->extraPath) {
        /*
            Only set PATH_TRANSLATED if extraPath is set (CGI spec) 
         */
        assert(rx->extraPath[0] == '/');
        mprAddKey(svars, "PATH_TRANSLATED", mprNormalizePath(sfmt("%s%s", rx->route->documents, rx->extraPath)));
    }
    if (rx->files) {
        params = httpGetParams(conn);
        assert(params);
        for (ITERATE_ITEMS(rx->files, file, index)) {
            mprSetJson(params, sfmt("FILE_%d_FILENAME", index), file->filename);
            mprSetJson(params, sfmt("FILE_%d_CLIENT_FILENAME", index), file->clientFilename);
            mprSetJson(params, sfmt("FILE_%d_CONTENT_TYPE", index), file->contentType);
            mprSetJson(params, sfmt("FILE_%d_NAME", index), file->name);
            mprSetJson(params, sfmt("FILE_%d_SIZE", index), sfmt("%zd", file->size));
        }
    }
    if (conn->http->envCallback) {
        conn->http->envCallback(conn);
    }
}


/*
    Add variables to the params. This comes from the query string and urlencoded post data.
    Make variables for each keyword in a query string. The buffer must be url encoded 
    (ie. key=value&key2=value2..., spaces converted to '+' and all else should be %HEX encoded).
 */
static void addParamsFromBuf(HttpConn *conn, cchar *buf, ssize len)
{
    MprJson     *params, *prior;
    char        *newValue, *decoded, *keyword, *value, *tok;

    assert(conn);
    params = httpGetParams(conn);
    decoded = mprAlloc(len + 1);
    decoded[len] = '\0';
    memcpy(decoded, buf, len);

    keyword = stok(decoded, "&", &tok);
    while (keyword != 0) {
        if ((value = strchr(keyword, '=')) != 0) {
            *value++ = '\0';
            value = mprUriDecode(value);
        } else {
            value = MPR->emptyString;
        }
        keyword = mprUriDecode(keyword);
        if (*keyword) {
            /*
                Append to existing keywords
             */
            prior = mprLookupJsonObj(params, keyword);
            if (prior && prior->type == MPR_JSON_VALUE) {
                if (*value) {
                    newValue = sjoin(prior->value, " ", value, NULL);
                    mprSetJson(params, keyword, newValue);
                }
            } else {
                mprSetJson(params, keyword, value);
            }
        }
        keyword = stok(0, "&", &tok);
    }
}


PUBLIC void httpAddQueryParams(HttpConn *conn) 
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->parsedUri->query && !(rx->flags & HTTP_ADDED_QUERY_PARAMS)) {
        addParamsFromBuf(conn, rx->parsedUri->query, slen(rx->parsedUri->query));
        rx->flags |= HTTP_ADDED_QUERY_PARAMS;
    }
}


PUBLIC int httpAddBodyParams(HttpConn *conn)
{
    HttpRx      *rx;
    HttpQueue   *q;
    MprBuf      *content;

    rx = conn->rx;
    q = conn->readq;

    if (rx->eof && !(rx->flags & HTTP_ADDED_BODY_PARAMS)) {
        if (q->first && q->first->content) {
            httpJoinPackets(q, -1);
            content = q->first->content;
            if (rx->form || rx->upload) {
                mprAddNullToBuf(content);
                addParamsFromBuf(conn, mprGetBufStart(content), mprGetBufLength(content));

            } else if (sstarts(rx->mimeType, "application/json")) {
                if (mprParseJsonInto(httpGetBodyInput(conn), httpGetParams(conn)) == 0) {
                    return MPR_ERR_BAD_FORMAT;
                }
            }
        }
        rx->flags |= HTTP_ADDED_BODY_PARAMS;
    }
    return 0;
}


PUBLIC void httpAddJsonParams(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->eof && sstarts(rx->mimeType, "application/json")) {
        if (!(rx->flags & HTTP_ADDED_BODY_PARAMS)) {
            mprParseJsonInto(httpGetBodyInput(conn), httpGetParams(conn));
            rx->flags |= HTTP_ADDED_BODY_PARAMS;
        }
    }
}


PUBLIC MprJson *httpGetParams(HttpConn *conn)
{ 
    if (conn->rx->params == 0) {
        conn->rx->params = mprCreateJson(MPR_JSON_OBJ);
    }
    return conn->rx->params;
}


PUBLIC int httpTestParam(HttpConn *conn, cchar *var)
{
    return mprLookupJsonObj(httpGetParams(conn), var) != 0;
}


PUBLIC cchar *httpGetParam(HttpConn *conn, cchar *var, cchar *defaultValue)
{
    cchar       *value;

    value = mprLookupJson(httpGetParams(conn), var);
    return (value) ? value : defaultValue;
}


PUBLIC int httpGetIntParam(HttpConn *conn, cchar *var, int defaultValue)
{
    cchar       *value;

    value = mprLookupJson(httpGetParams(conn), var);
    return (value) ? (int) stoi(value) : defaultValue;
}


static int sortParam(MprJson **j1, MprJson **j2)
{
    return scmp((*j1)->name, (*j2)->name);
}


/*
    Return the request parameters as a string. 
    This will return the exact same string regardless of the order of form parameters.
 */
PUBLIC char *httpGetParamsString(HttpConn *conn)
{
    HttpRx      *rx;
    MprJson     *jp, *params;
    MprList     *list;
    char        *buf, *cp;
    ssize       len;
    int         ji, next;

    assert(conn);
    rx = conn->rx;

    if (rx->paramString == 0) {
        if ((params = conn->rx->params) != 0) {
            if ((list = mprCreateList(params->length, 0)) != 0) {
                len = 0;
                for (ITERATE_JSON(params, jp, ji)) {
                    if (jp->type & MPR_JSON_VALUE) {
                        mprAddItem(list, jp);
                        len += slen(jp->name) + slen(jp->value) + 2;
                    }
                }
                if ((buf = mprAlloc(len + 1)) != 0) {
                    mprSortList(list, (MprSortProc) sortParam, 0);
                    cp = buf;
                    for (next = 0; (jp = mprGetNextItem(list, &next)) != 0; ) {
                        strcpy(cp, jp->name); cp += slen(jp->name);
                        *cp++ = '=';
                        strcpy(cp, jp->value); cp += slen(jp->value);
                        *cp++ = '&';
                    }
                    cp[-1] = '\0';
                    rx->paramString = buf;
                }
            }
        }
    }
    return rx->paramString;
}


PUBLIC void httpRemoveParam(HttpConn *conn, cchar *var) 
{
    mprRemoveJson(httpGetParams(conn), var);
}


PUBLIC void httpSetParam(HttpConn *conn, cchar *var, cchar *value) 
{
    mprSetJson(httpGetParams(conn), var, value);
}


PUBLIC void httpSetIntParam(HttpConn *conn, cchar *var, int value) 
{
    mprSetJson(httpGetParams(conn), var, sfmt("%d", value));
}


PUBLIC bool httpMatchParam(HttpConn *conn, cchar *var, cchar *value)
{
    return smatch(value, httpGetParam(conn, var, " __UNDEF__ "));
}


PUBLIC void httpAddUploadFile(HttpConn *conn, HttpUploadFile *upfile)
{
    HttpRx   *rx;

    rx = conn->rx;
    if (rx->files == 0) {
        rx->files = mprCreateList(0, MPR_LIST_STABLE);
    }
    mprAddItem(rx->files, upfile);
}


PUBLIC void httpRemoveAllUploadedFiles(HttpConn *conn)
{
    HttpRx          *rx;
    HttpUploadFile  *file;
    int             index;

    rx = conn->rx;

    for (ITERATE_ITEMS(rx->files, file, index)) {
        if (file->filename) {
            mprDeletePath(file->filename);
            file->filename = 0;
        }
    }
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/webSockFilter.c ************/


/*
    webSockFilter.c - WebSockets filter support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_HTTP_WEB_SOCKETS
/********************************** Locals ************************************/
/*
    Message frame states
 */
#define WS_BEGIN       0
#define WS_EXT_DATA    1                /* Unused */
#define WS_MSG         2
#define WS_CLOSED      3

static char *codetxt[16] = {
    "cont", "text", "binary", "reserved", "reserved", "reserved", "reserved", "reserved",
    "close", "ping", "pong", "reserved", "reserved", "reserved", "reserved", "reserved",
};

/*
    Frame format

     Byte 0          Byte 1          Byte 2          Byte 3
     0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
    +-+-+-+-+-------+-+-------------+-------------------------------+
    |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
    |I|S|S|S|  (4)  |A|     (7)     |             (16/63)           |
    |N|V|V|V|       |S|             |   (if payload len==126/127)   |
    | |1|2|3|       |K|             |                               |
    +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
    |     Extended payload length continued, if payload len == 127  |
    + - - - - - - - - - - - - - - - +-------------------------------+
    |                               |Masking-key, if MASK set to 1  |
    +-------------------------------+-------------------------------+
    | Masking-key (continued)       |          Payload Data         |
    +-------------------------------- - - - - - - - - - - - - - - - +
    :                     Payload Data continued ...                :
    + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
    |                     Payload Data continued ...                |
    +---------------------------------------------------------------+

    Single message has
        fin == 1
    Fragmented message has
        fin == 0, opcode != 0
        fin == 0, opcode == 0
        fin == 1, opcode == 0

    Common first byte codes:
        0x9B    Fin | /SET

    NOTE: control frames (opcode >= 8) can be sent between fragmented frames
 */
#define GET_FIN(v)              (((v) >> 7) & 0x1)          /* Final fragment */
#define GET_RSV(v)              (((v) >> 4) & 0x7)          /* Reserved (used for extensions) */
#define GET_CODE(v)             ((v) & 0xf)                 /* Packet opcode */
#define GET_MASK(v)             (((v) >> 7) & 0x1)          /* True if dataMask in frame (client send) */
#define GET_LEN(v)              ((v) & 0x7f)                /* Low order 7 bits of length */

#define SET_FIN(v)              (((v) & 0x1) << 7)
#define SET_MASK(v)             (((v) & 0x1) << 7)
#define SET_CODE(v)             ((v) & 0xf)
#define SET_LEN(len, n)         ((uchar)(((len) >> ((n) * 8)) & 0xff))

/*
    Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
    See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.
 */
#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static const uchar utfTable[] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
    8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
    0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
    0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
    0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
    1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
    1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
    1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

/********************************** Forwards **********************************/

static void closeWebSock(HttpQueue *q);
static void incomingWebSockData(HttpQueue *q, HttpPacket *packet);
static void manageWebSocket(HttpWebSocket *ws, int flags);
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir);
static int openWebSock(HttpQueue *q);
static void outgoingWebSockService(HttpQueue *q);
static int processFrame(HttpQueue *q, HttpPacket *packet);
static void readyWebSock(HttpQueue *q);
static int validUTF8(HttpConn *conn, cchar *str, ssize len);
static bool validateText(HttpConn *conn, HttpPacket *packet);
static void webSockPing(HttpConn *conn);
static void webSockTimeout(HttpConn *conn);

static void traceErrorProc(HttpConn *conn, cchar *fmt, ...);

#define traceError(conn, ...) \
    if (conn->http->traceLevel > 0 && PTOI(mprLookupKey(conn->trace->events, "error")) <= conn->http->traceLevel) { \
        traceErrorProc(conn, __VA_ARGS__); \
    } else

/*********************************** Code *************************************/
/*
   WebSocket Filter initialization
 */
PUBLIC int httpOpenWebSockFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("webSocketFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->webSocketFilter = filter;
    filter->match = matchWebSock;
    filter->open = openWebSock;
    filter->ready = readyWebSock;
    filter->close = closeWebSock;
    filter->outgoingService = outgoingWebSockService;
    filter->incoming = incomingWebSockData;
    return 0;
}


/*
    Match if the filter is required for this request. This is called twice: once for TX and once for RX. RX first.
 */
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpWebSocket   *ws;
    HttpRx          *rx;
    HttpTx          *tx;
    char            *kind, *tok;
    cchar           *key, *protocols;
    int             version;

    assert(conn);
    assert(route);
    rx = conn->rx;
    tx = conn->tx;
    assert(rx);
    assert(tx);

    if (conn->error) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (httpClientConn(conn)) {
        if (rx->webSocket) {
            return HTTP_ROUTE_OK;
        } else if (tx->parsedUri && tx->parsedUri->webSockets) {
            /* ws:// URI. Client web sockets */
            if ((ws = mprAllocObj(HttpWebSocket, manageWebSocket)) == 0) {
                httpMemoryError(conn);
                return HTTP_ROUTE_OK;
            }
            rx->webSocket = ws;
            ws->state = WS_STATE_CONNECTING;
            return HTTP_ROUTE_OK;
        }
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (dir & HTTP_STAGE_TX) {
        return rx->webSocket ? HTTP_ROUTE_OK : HTTP_ROUTE_OMIT_FILTER;
    }
    if (!rx->upgrade || !scaselessmatch(rx->upgrade, "websocket")) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (!rx->hostHeader || !smatch(rx->method, "GET")) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    version = (int) stoi(httpGetHeader(conn, "sec-websocket-version"));
    if (version < WS_VERSION) {
        httpSetHeader(conn, "Sec-WebSocket-Version", "%d", WS_VERSION);
        httpError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Version");
        return HTTP_ROUTE_OK;
    }
    if ((key = httpGetHeader(conn, "sec-websocket-key")) == 0) {
        httpError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Bad Sec-WebSocket-Key");
        return HTTP_ROUTE_OK;
    }
    protocols = httpGetHeader(conn, "sec-websocket-protocol");

    if (dir & HTTP_STAGE_RX) {
        if ((ws = mprAllocObj(HttpWebSocket, manageWebSocket)) == 0) {
            httpMemoryError(conn);
            return HTTP_ROUTE_OK;
        }
        rx->webSocket = ws;
        ws->state = WS_STATE_OPEN;
        ws->preserveFrames = (rx->route->flags & HTTP_ROUTE_PRESERVE_FRAMES) ? 1 : 0;

        /* Just select the first protocol */
        if (route->webSocketsProtocol) {
            for (kind = stok(sclone(protocols), " \t,", &tok); kind; kind = stok(NULL, " \t,", &tok)) {
                if (smatch(route->webSocketsProtocol, kind)) {
                    break;
                }
            }
            if (!kind) {
                httpError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Protocol");
                return HTTP_ROUTE_OK;
            }
            ws->subProtocol = sclone(kind);
        } else {
            /* Just pick the first protocol */
            ws->subProtocol = stok(sclone(protocols), " ,", NULL);
        }
        httpSetStatus(conn, HTTP_CODE_SWITCHING);
        httpSetHeader(conn, "Connection", "Upgrade");
        httpSetHeader(conn, "Upgrade", "WebSocket");
        httpSetHeaderString(conn, "Sec-WebSocket-Accept", mprGetSHABase64(sjoin(key, WS_MAGIC, NULL)));
        if (ws->subProtocol && *ws->subProtocol) {
            httpSetHeaderString(conn, "Sec-WebSocket-Protocol", ws->subProtocol);
        }
        httpSetHeader(conn, "X-Request-Timeout", "%lld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
        httpSetHeader(conn, "X-Inactivity-Timeout", "%lld", conn->limits->inactivityTimeout / MPR_TICKS_PER_SEC);

        if (route->webSocketsPingPeriod) {
            ws->pingEvent = mprCreateEvent(conn->dispatcher, "webSocket", route->webSocketsPingPeriod,
                webSockPing, conn, MPR_EVENT_CONTINUOUS);
        }
        conn->keepAliveCount = 0;
        conn->upgraded = 1;
        rx->eof = 0;
        rx->remainingContent = MAXINT;
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


/*
    Open the filter for a new request
 */
static int openWebSock(HttpQueue *q)
{
    HttpConn        *conn;
    HttpWebSocket   *ws;
    HttpPacket      *packet;

    assert(q);
    conn = q->conn;
    ws = conn->rx->webSocket;
    assert(ws);

    q->packetSize = min(conn->limits->bufferSize, q->max);
    ws->closeStatus = WS_STATUS_NO_STATUS;
    conn->timeoutCallback = webSockTimeout;

    if ((packet = httpGetPacket(conn->writeq)) != 0) {
        assert(packet->flags & HTTP_PACKET_HEADER);
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);
    }
    conn->tx->responded = 0;
    return 0;
}


static void manageWebSocket(HttpWebSocket *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->currentFrame);
        mprMark(ws->currentMessage);
        mprMark(ws->tailMessage);
        mprMark(ws->pingEvent);
        mprMark(ws->subProtocol);
        mprMark(ws->errorMsg);
        mprMark(ws->closeReason);
        mprMark(ws->data);
    }
}


static void closeWebSock(HttpQueue *q)
{
    HttpWebSocket   *ws;

    if (q->conn && q->conn->rx) {
        ws = q->conn->rx->webSocket;
        assert(ws);
        if (ws) {
            ws->state = WS_STATE_CLOSED;
           if (ws->pingEvent) {
                mprRemoveEvent(ws->pingEvent);
                ws->pingEvent = 0;
           }
        }
    }
}


static void readyWebSock(HttpQueue *q)
{
    if (httpServerConn(q->conn)) {
        HTTP_NOTIFY(q->conn, HTTP_EVENT_APP_OPEN, 0);
    }
}


static void incomingWebSockData(HttpQueue *q, HttpPacket *packet)
{
    HttpConn        *conn;
    HttpWebSocket   *ws;
    HttpPacket      *tail;
    HttpLimits      *limits;
    MprBuf          *content;
    char            *fp, *cp;
    ssize           len, currentFrameLen, offset, frameLen;
    int             i, error, mask, lenBytes, opcode;

    assert(packet);
    conn = q->conn;
    assert(conn->rx);
    ws = conn->rx->webSocket;
    assert(ws);
    limits = conn->limits;

    if (packet->flags & HTTP_PACKET_DATA) {
        /*
            The service queue is used to hold data that is yet to be analyzed.
            The ws->currentFrame holds the current frame that is being read from the service queue.
         */
        httpJoinPacketForService(q, packet, 0);
    }
    httpTracePacket(conn, "body", "request.websockets.data", packet, "state:%d,frame:%d,length:%zu",
        ws->state, ws->frameState, httpGetPacketLength(packet));

    if (packet->flags & HTTP_PACKET_END) {
        /*
            EOF packet means the socket has been abortively closed
         */
        if (ws->state != WS_STATE_CLOSED) {
            ws->closing = 1;
            ws->frameState = WS_CLOSED;
            ws->state = WS_STATE_CLOSED;
            ws->closeStatus = WS_STATUS_COMMS_ERROR;
            HTTP_NOTIFY(conn, HTTP_EVENT_APP_CLOSE, ws->closeStatus);
            httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Connection lost");
        }
    }
    while ((packet = httpGetPacket(q)) != 0) {
        content = packet->content;
        error = 0;
        switch (ws->frameState) {
        case WS_CLOSED:
            if (httpGetPacketLength(packet) > 0) {
                traceError(conn, "Closed, ignore incoming packet");
            }
            httpFinalize(conn);
            httpSetState(conn, HTTP_STATE_FINALIZED);
            break;

        case WS_BEGIN:
            if (httpGetPacketLength(packet) < 2) {
                /* Need more data */
                httpPutBackPacket(q, packet);
                return;
            }
            fp = content->start;
            if (GET_RSV(*fp) != 0) {
                error = WS_STATUS_PROTOCOL_ERROR;
                traceError(conn, "Protocol error, bad reserved field");
                break;
            }
            packet->last = GET_FIN(*fp);
            opcode = GET_CODE(*fp);
            if (opcode == WS_MSG_CONT) {
                if (!ws->currentMessageType) {
                    traceError(conn, "Protocol error, continuation frame but not prior message");
                    error = WS_STATUS_PROTOCOL_ERROR;
                    break;
                }
            } else if (opcode < WS_MSG_CONTROL && ws->currentMessageType) {
                traceError(conn, "Protocol error, data frame received but expected a continuation frame");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            if (opcode > WS_MSG_PONG) {
                traceError(conn, "Protocol error, bad frame opcode");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            packet->type = opcode;
            if (opcode >= WS_MSG_CONTROL && !packet->last) {
                /* Control frame, must not be fragmented */
                traceError(conn, "Protocol error, fragmented control frame");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            fp++;
            len = GET_LEN(*fp);
            mask = GET_MASK(*fp);
            lenBytes = 1;
            if (len == 126) {
                lenBytes += 2;
                len = 0;
            } else if (len == 127) {
                lenBytes += 8;
                len = 0;
            }
            if (httpGetPacketLength(packet) < (lenBytes + 1 + (mask * 4))) {
                /* Return if we don't have the required packet control fields */
                httpPutBackPacket(q, packet);
                return;
            }
            fp++;
            while (--lenBytes > 0) {
                len <<= 8;
                len += (uchar) *fp++;
            }
            if (packet->type >= WS_MSG_CONTROL && len > WS_MAX_CONTROL) {
                /* Too big */
                traceError(conn, "Protocol error, control frame too big");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            ws->frameLength = len;
            ws->frameState = WS_MSG;
            ws->maskOffset = mask ? 0 : -1;
            if (mask) {
                for (i = 0; i < 4; i++) {
                    ws->dataMask[i] = *fp++;
                }
            }
            assert(content);
            assert(fp >= content->start);
            mprAdjustBufStart(content, fp - content->start);
            assert(q->count >= 0);
            /*
                Put packet onto the service queue
             */
            httpPutBackPacket(q, packet);
            ws->frameState = WS_MSG;
            break;

        case WS_MSG:
            currentFrameLen = httpGetPacketLength(ws->currentFrame);
            len = httpGetPacketLength(packet);
            if ((currentFrameLen + len) > ws->frameLength) {
                /*
                    Split packet if it contains data for the next frame. Do this even if this frame has no data.
                 */
                offset = ws->frameLength - currentFrameLen;
                if ((tail = httpSplitPacket(packet, offset)) != 0) {
                    content = packet->content;
                    httpPutBackPacket(q, tail);
                    len = httpGetPacketLength(packet);
                }
            }
            if ((currentFrameLen + len) > conn->limits->webSocketsMessageSize) {
                if (httpServerConn(conn)) {
                    httpMonitorEvent(conn, HTTP_COUNTER_LIMIT_ERRORS, 1);
                }
                traceError(conn, "Incoming message is too large, length %zd, max %zd", len, limits->webSocketsMessageSize);
                error = WS_STATUS_MESSAGE_TOO_LARGE;
                break;
            }
            if (ws->maskOffset >= 0) {
                for (cp = content->start; cp < content->end; cp++) {
                    *cp = *cp ^ ws->dataMask[ws->maskOffset++ & 0x3];
                }
            }
            if (packet->type == WS_MSG_CONT && ws->currentFrame) {
                httpJoinPacket(ws->currentFrame, packet);
                packet = ws->currentFrame;
                content = packet->content;
            }
#if KEEP
            if (packet->type == WS_MSG_TEXT) {
                /*
                    Validate the frame for fast-fail, provided the last frame does not have a partial codepoint.
                 */
                if (!ws->partialUTF) {
                    if (!validateText(conn, packet)) {
                        error = WS_STATUS_INVALID_UTF8;
                        break;
                    }
                    ws->partialUTF = 0;
                }
            }
#endif
            frameLen = httpGetPacketLength(packet);
            assert(frameLen <= ws->frameLength);
            if (frameLen == ws->frameLength) {
                if ((error = processFrame(q, packet)) != 0) {
                    break;
                }
                if (ws->state == WS_STATE_CLOSED) {
                    HTTP_NOTIFY(conn, HTTP_EVENT_APP_CLOSE, ws->closeStatus);
                    httpFinalize(conn);
                    ws->frameState = WS_CLOSED;
                    httpSetState(conn, HTTP_STATE_FINALIZED);
                    break;
                }
                ws->currentFrame = 0;
                ws->frameState = WS_BEGIN;
            } else {
                ws->currentFrame = packet;
            }
            break;

        default:
            traceError(conn, "Protocol error, unknown frame state");
            error = WS_STATUS_PROTOCOL_ERROR;
            break;
        }
        if (error) {
            /*
                Notify of the error and send a close to the peer. The peer may or may not be still there.
             */
            HTTP_NOTIFY(conn, HTTP_EVENT_ERROR, error);
            httpSendClose(conn, error, NULL);
            ws->frameState = WS_CLOSED;
            ws->state = WS_STATE_CLOSED;
            httpFinalize(conn);
            httpSetEof(conn);
            httpSetState(conn, HTTP_STATE_FINALIZED);
            return;
        }
    }
}


static int processFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpConn        *conn;
    HttpRx          *rx;
    HttpWebSocket   *ws;
    HttpLimits      *limits;
    MprBuf          *content;
    ssize           len;
    char            *cp;
    int             validated;

    conn = q->conn;
    limits = conn->limits;
    ws = conn->rx->webSocket;
    assert(ws);
    rx = conn->rx;
    assert(packet);
    content = packet->content;
    validated = 0;
    assert(content);

    mprAddNullToBuf(content);
    httpTrace(conn, "rx.websockets.packet", "body", "wsSeq:%d,wsTypeName:'%s',wsType:%d,wsLast:%d,wsLength:%zu",
         ws->rxSeq++, codetxt[packet->type], packet->type, packet->last, mprGetBufLength(content));

    switch (packet->type) {
    case WS_MSG_TEXT:
        httpTracePacket(conn, "rx.body.websockets.data", "body", packet, 0);
        /* Fall through */

    case WS_MSG_BINARY:
        ws->messageLength = 0;
        ws->currentMessageType = packet->type;
        /* Fall through */

    case WS_MSG_CONT:
        if (ws->closing) {
            break;
        }
        if (packet->type == WS_MSG_CONT) {
            if (!ws->currentMessageType) {
                traceError(conn, "Bad continuation packet");
                return WS_STATUS_PROTOCOL_ERROR;
            }
            packet->type = ws->currentMessageType;
        }
        /*
            Validate this frame if we don't have a partial codepoint from a prior frame.
         */
        if (packet->type == WS_MSG_TEXT && !ws->partialUTF) {
            if (!validateText(conn, packet)) {
                return WS_STATUS_INVALID_UTF8;
            }
            validated++;
        }
        if (ws->currentMessage && !ws->preserveFrames) {
            httpJoinPacket(ws->currentMessage, packet);
            ws->currentMessage->last = packet->last;
            packet = ws->currentMessage;
            content = packet->content;
            if (packet->type == WS_MSG_TEXT && !validated) {
                if (!validateText(conn, packet)) {
                    return WS_STATUS_INVALID_UTF8;
                }
            }
        }
        /*
            Send what we have if preserving frames or the current messages is over the packet limit size. 
            Otherwise, keep buffering.
         */
        for (ws->tailMessage = 0; packet; packet = ws->tailMessage, ws->tailMessage = 0) {
            if (!ws->preserveFrames && (httpGetPacketLength(packet) > limits->webSocketsPacketSize)) {
                ws->tailMessage = httpSplitPacket(packet, limits->webSocketsPacketSize);
                content = packet->content;
                packet->last = 0;
            }
            if (packet->last || ws->tailMessage || ws->preserveFrames) {
                packet->flags |= HTTP_PACKET_SOLO;
                ws->messageLength += httpGetPacketLength(packet);
                if (packet->type == WS_MSG_TEXT) {
                    mprAddNullToBuf(packet->content);
                }
                httpPutPacketToNext(q, packet);
                ws->currentMessage = 0;
            } else {
                ws->currentMessage = packet;
                break;
            }
            if (packet->last) {
                ws->currentMessageType = 0;
            }
        }
        break;

    case WS_MSG_CLOSE:
        cp = content->start;
        if (httpGetPacketLength(packet) == 0) {
            ws->closeStatus = WS_STATUS_OK;
        } else if (httpGetPacketLength(packet) < 2) {
            traceError(conn, "Missing close status");
            return WS_STATUS_PROTOCOL_ERROR;
        } else {
            ws->closeStatus = ((uchar) cp[0]) << 8 | (uchar) cp[1];

            /*
                WebSockets is a hideous spec, as if UTF validation wasn't bad enough, we must invalidate these codes:
                    1004, 1005, 1006, 1012-1016, 2000-2999
             */
            if (ws->closeStatus < 1000 || ws->closeStatus >= 5000 ||
                (1004 <= ws->closeStatus && ws->closeStatus <= 1006) ||
                (1012 <= ws->closeStatus && ws->closeStatus <= 1016) ||
                (1100 <= ws->closeStatus && ws->closeStatus <= 2999)) {
                traceError(conn, "Bad close status %d", ws->closeStatus);
                return WS_STATUS_PROTOCOL_ERROR;
            }
            mprAdjustBufStart(content, 2);
            if (httpGetPacketLength(packet) > 0) {
                ws->closeReason = mprCloneBufMem(content);
                if (!rx->route || !rx->route->ignoreEncodingErrors) {
                    if (validUTF8(conn, ws->closeReason, slen(ws->closeReason)) != UTF8_ACCEPT) {
                        traceError(conn, "Text packet has invalid UTF8");
                        return WS_STATUS_INVALID_UTF8;
                    }
                }
            }
        }
        httpTrace(conn, "rx.websockets.close", "context",
            "wsCloseStatus:%d,wsCloseReason:'%s',wsClosing:%d", ws->closeStatus, ws->closeReason, ws->closing);
        if (ws->closing) {
            httpDisconnect(conn);
        } else {
            /* Acknowledge the close. Echo the received status */
            httpSendClose(conn, WS_STATUS_OK, "OK");
            httpSetEof(conn);
            rx->remainingContent = 0;
            conn->keepAliveCount = 0;
        }
        ws->state = WS_STATE_CLOSED;
        break;

    case WS_MSG_PING:
        /* Respond with the same content as specified in the ping message */
        len = mprGetBufLength(content);
        len = min(len, WS_MAX_CONTROL);
        httpSendBlock(conn, WS_MSG_PONG, mprGetBufStart(content), mprGetBufLength(content), HTTP_BUFFER);
        break;

    case WS_MSG_PONG:
        /* Do nothing */
        break;

    default:
        traceError(conn, "Bad message type %d", packet->type);
        ws->state = WS_STATE_CLOSED;
        return WS_STATUS_PROTOCOL_ERROR;
    }
    return 0;
}


/*
    Send a text message. Caller must submit valid UTF8.
    Returns the number of data message bytes written. Should equal the length.
 */
PUBLIC ssize httpSend(HttpConn *conn, cchar *fmt, ...)
{
    va_list     args;
    char        *buf;

    va_start(args, fmt);
    buf = sfmtv(fmt, args);
    va_end(args);
    return httpSendBlock(conn, WS_MSG_TEXT, buf, slen(buf), HTTP_BUFFER);
}


/*
    Send a block of data with the specified message type. Set flags to HTTP_MORE to indicate there is more data 
    for this message.
 */
PUBLIC ssize httpSendBlock(HttpConn *conn, int type, cchar *buf, ssize len, int flags)
{
    HttpWebSocket   *ws;
    HttpPacket      *packet;
    HttpQueue       *q;
    ssize           room, thisWrite, totalWritten;

    assert(conn);

    ws = conn->rx->webSocket;
    conn->tx->responded = 1;

    /*
        Note: we can come here before the handshake is complete. The data is queued and if the connection handshake
        succeeds, then the data is sent.
     */
    if (!(HTTP_STATE_CONNECTED <= conn->state && conn->state < HTTP_STATE_FINALIZED) || !conn->upgraded) {
        return MPR_ERR_BAD_STATE;
    }
    if (type != WS_MSG_CONT && type != WS_MSG_TEXT && type != WS_MSG_BINARY && type != WS_MSG_CLOSE &&
            type != WS_MSG_PING && type != WS_MSG_PONG) {
        traceError(conn, "Bad message type %d", type);
        return MPR_ERR_BAD_ARGS;
    }
    q = conn->writeq;
    if (flags == 0) {
        flags = HTTP_BUFFER;
    }
    if (len < 0) {
        len = slen(buf);
    }
    if (len > conn->limits->webSocketsMessageSize) {
        if (httpServerConn(conn)) {
            httpMonitorEvent(conn, HTTP_COUNTER_LIMIT_ERRORS, 1);
        }
        traceError(conn, "Outgoing message is too large, length %zd max %zd", len, conn->limits->webSocketsMessageSize);
        return MPR_ERR_WONT_FIT;
    }
    totalWritten = 0;
    do {
        if ((room = q->max - q->count) == 0) {
            if (flags & HTTP_NON_BLOCK) {
                break;
            }
        }
        /*
            Break into frames if the user is not preserving frames and has not explicitly specified "more".
            The outgoingWebSockService will encode each packet as a frame.
         */
        if (ws->preserveFrames || (flags & HTTP_MORE)) {
            thisWrite = len;
        } else {
            thisWrite = min(len, conn->limits->webSocketsFrameSize);
        }
        thisWrite = min(thisWrite, q->packetSize);
        if (flags & (HTTP_BLOCK | HTTP_NON_BLOCK)) {
            thisWrite = min(thisWrite, room);
        }
        /*
            Must still send empty packets of zero length
         */
        if ((packet = httpCreateDataPacket(thisWrite)) == 0) {
            return MPR_ERR_MEMORY;
        }
        /*
            Spec requires type to be set only on the first frame
         */
        if (ws->more) {
            type = 0;
        }
        packet->type = type;
        type = 0;
        if (ws->preserveFrames || (flags & HTTP_MORE)) {
            packet->flags |= HTTP_PACKET_SOLO;
        }
        if (thisWrite > 0) {
            if (mprPutBlockToBuf(packet->content, buf, thisWrite) != thisWrite) {
                return MPR_ERR_MEMORY;
            }
        }
        len -= thisWrite;
        buf += thisWrite;
        totalWritten += thisWrite;
        packet->last = (len > 0) ? 0 : !(flags & HTTP_MORE);
        ws->more = !packet->last;
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);

        if (q->count >= q->max) {
            httpFlushQueue(q, flags);
            if (q->count >= q->max && (flags & HTTP_NON_BLOCK)) {
                break;
            }
        }
        if (httpRequestExpired(conn, 0)) {
            return MPR_ERR_TIMEOUT;
        }
    } while (len > 0);

    httpFlushQueue(q, flags);
    if (httpClientConn(conn)) {
        httpEnableConnEvents(conn);
    }
    return totalWritten;
}


/*
    The reason string is optional
 */
PUBLIC ssize httpSendClose(HttpConn *conn, int status, cchar *reason)
{
    HttpWebSocket   *ws;
    char            msg[128];
    ssize           len;

    assert(0 <= status && status <= WS_STATUS_MAX);
    ws = conn->rx->webSocket;
    assert(ws);
    if (ws->closing) {
        return 0;
    }
    ws->closing = 1;
    ws->state = WS_STATE_CLOSING;

    if (!(HTTP_STATE_CONNECTED <= conn->state && conn->state < HTTP_STATE_FINALIZED) || !conn->upgraded) {
        /* Ignore closes when already finalized or not yet connected */
        return 0;
    }
    len = 2;
    if (reason) {
        if (slen(reason) >= 124) {
            reason = "WebSockets close message was too big";
            traceError(conn, reason);
        }
        len += slen(reason) + 1;
    }
    msg[0] = (status >> 8) & 0xff;
    msg[1] = status & 0xff;
    if (reason) {
        scopy(&msg[2], len - 2, reason);
    }
    httpTrace(conn, "tx.websockets.close", "context", "wsCloseStatus:%d,wsCloseReason:'%s'", status, reason);
    return httpSendBlock(conn, WS_MSG_CLOSE, msg, len, HTTP_BUFFER);
}


/*
    This is the outgoing filter routine. It services packets on the outgoing queue and transforms them into
    WebSockets frames.
 */
static void outgoingWebSockService(HttpQueue *q)
{
    HttpConn        *conn;
    HttpPacket      *packet, *tail;
    HttpWebSocket   *ws;
    char            *ep, *fp, *prefix, dataMask[4];
    ssize           len;
    int             i, mask;

    conn = q->conn;
    ws = conn->rx->webSocket;
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!(packet->flags & (HTTP_PACKET_END | HTTP_PACKET_HEADER))) {
            if (!(packet->flags & HTTP_PACKET_SOLO)) {
                if (packet->esize > conn->limits->bufferSize) {
                    if ((tail = httpResizePacket(q, packet, conn->limits->bufferSize)) != 0) {
                        assert(tail->last == packet->last);
                        packet->last = 0;
                    }
                }
                if (!httpWillNextQueueAcceptPacket(q, packet)) {
                    httpPutBackPacket(q, packet);
                    return;
                }
            }
            if (packet->type < 0 || packet->type > WS_MSG_MAX) {
                httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Bad WebSocket packet type %d", packet->type);
                break;
            }
            len = httpGetPacketLength(packet);
            packet->prefix = mprCreateBuf(16, 16);
            prefix = packet->prefix->start;
            /*
                Server-side does not mask outgoing data
             */
            mask = httpServerConn(conn) ? 0 : 1;
            *prefix++ = SET_FIN(packet->last) | SET_CODE(packet->type);
            if (len <= WS_MAX_CONTROL) {
                *prefix++ = SET_MASK(mask) | SET_LEN(len, 0);
            } else if (len <= 65535) {
                *prefix++ = SET_MASK(mask) | 126;
                *prefix++ = SET_LEN(len, 1);
                *prefix++ = SET_LEN(len, 0);
            } else {
                *prefix++ = SET_MASK(mask) | 127;
                for (i = 7; i >= 0; i--) {
                    *prefix++ = SET_LEN(len, i);
                }
            }
            if (httpClientConn(conn)) {
                mprGetRandomBytes(dataMask, sizeof(dataMask), 0);
                for (i = 0; i < 4; i++) {
                    *prefix++ = dataMask[i];
                }
                fp = packet->content->start;
                ep = packet->content->end;
                for (i = 0; fp < ep; fp++) {
                    *fp = *fp ^ dataMask[i++ & 0x3];
                }
            }
            *prefix = '\0';
            mprAdjustBufEnd(packet->prefix, prefix - packet->prefix->start);
            httpTracePacket(conn, "tx.websockets.packet", "body", packet, 
                "wsSeqno:%d,wsTypeName:\"%s\",wsType:%d,wsLast:%d,wsLength:%zd",
                ws->txSeq++, codetxt[packet->type], packet->type, packet->last, httpGetPacketLength(packet));
        }
        httpPutPacketToNext(q, packet);
    }
}


PUBLIC cchar *httpGetWebSocketCloseReason(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->closeReason;
}


PUBLIC void *httpGetWebSocketData(HttpConn *conn)
{
    return (conn->rx && conn->rx->webSocket) ? conn->rx->webSocket->data : NULL;
}


PUBLIC ssize httpGetWebSocketMessageLength(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->messageLength;
}


PUBLIC char *httpGetWebSocketProtocol(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->subProtocol;
}


PUBLIC ssize httpGetWebSocketState(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->state;
}


PUBLIC bool httpWebSocketOrderlyClosed(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->closeStatus != WS_STATUS_COMMS_ERROR;
}


PUBLIC void httpSetWebSocketData(HttpConn *conn, void *data)
{
    if (conn->rx && conn->rx->webSocket) {
        conn->rx->webSocket->data = data;
    }
}


PUBLIC void httpSetWebSocketProtocols(HttpConn *conn, cchar *protocols)
{
    assert(conn);
    assert(protocols && *protocols);
    conn->protocols = sclone(protocols);
}


PUBLIC void httpSetWebSocketPreserveFrames(HttpConn *conn, bool on)
{
    HttpWebSocket   *ws;

    if ((ws = conn->rx->webSocket) != 0) {
        ws->preserveFrames = on;
    }
}


/*
    Test if a string is a valid unicode string.
    The return state may be UTF8_ACCEPT if all codepoints validate and are complete.
    Return UTF8_REJECT if an invalid codepoint was found.
    Otherwise, return the state for a partial codepoint.
 */
static int validUTF8(HttpConn *conn, cchar *str, ssize len)
{
    uchar   *cp, c;
    uint    state, type;

    state = UTF8_ACCEPT;
    for (cp = (uchar*) str; cp < (uchar*) &str[len]; cp++) {
        c = *cp;
        type = utfTable[c];
        /*
            KEEP. codepoint = (*state != UTF8_ACCEPT) ? (byte & 0x3fu) | (*codep << 6) : (0xff >> type) & (byte);
         */
        state = utfTable[256 + (state * 16) + type];
        if (state == UTF8_REJECT) {
            traceError(conn, "Invalid UTF8 at offset %d", cp - (uchar*) str);
            break;
        }
    }
    return state;
}


/*
    Validate the UTF8 in a packet. Return false if an invalid codepoint is found.
    If the packet is not the last packet, we alloc incomplete codepoints.
    Set ws->partialUTF if the last codepoint was incomplete.
 */
static bool validateText(HttpConn *conn, HttpPacket *packet)
{
    HttpWebSocket   *ws;
    HttpRx          *rx;
    MprBuf          *content;
    int             state;
    bool            valid;

    rx = conn->rx;
    ws = rx->webSocket;

    /*
        Skip validation if ignoring errors or some frames have already been sent to the callback
     */
    if ((rx->route && rx->route->ignoreEncodingErrors) || ws->messageLength > 0) {
        return 1;
    }
    content = packet->content;
    state = validUTF8(conn, content->start, mprGetBufLength(content));
    ws->partialUTF = state != UTF8_ACCEPT;

    if (packet->last) {
        valid =  state == UTF8_ACCEPT;
    } else {
        valid = state != UTF8_REJECT;
    }
    if (!valid) {
        traceError(conn, "Text packet has invalid UTF8");
    }
    return valid;
}


static void webSockPing(HttpConn *conn)
{
    assert(conn);
    assert(conn->rx);
    /*
        Send a ping. Optimze by sending no data message with it.
     */
    httpSendBlock(conn, WS_MSG_PING, NULL, 0, HTTP_BUFFER);
}


static void webSockTimeout(HttpConn *conn)
{
    assert(conn);
    httpSendClose(conn, WS_STATUS_POLICY_VIOLATION, "Request timeout");
}


/*
    Upgrade a client socket to use Web Sockets. This is called by the client to request a web sockets upgrade.
 */
PUBLIC int httpUpgradeWebSocket(HttpConn *conn)
{
    HttpTx  *tx;
    char    num[16];

    tx = conn->tx;
    assert(httpClientConn(conn));

    httpSetStatus(conn, HTTP_CODE_SWITCHING);
    httpSetHeader(conn, "Upgrade", "websocket");
    httpSetHeader(conn, "Connection", "Upgrade");
    mprGetRandomBytes(num, sizeof(num), 0);
    tx->webSockKey = mprEncode64Block(num, sizeof(num));
    httpSetHeaderString(conn, "Sec-WebSocket-Key", tx->webSockKey);
    httpSetHeaderString(conn, "Sec-WebSocket-Protocol", conn->protocols ? conn->protocols : "chat");
    httpSetHeaderString(conn, "Sec-WebSocket-Version", "13");
    httpSetHeader(conn, "X-Request-Timeout", "%lld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
    httpSetHeader(conn, "X-Inactivity-Timeout", "%lld", conn->limits->inactivityTimeout / MPR_TICKS_PER_SEC);

    conn->upgraded = 1;
    conn->keepAliveCount = 0;
    conn->rx->remainingContent = MAXINT;
    return 0;
}


/*
    Client verification of the server WebSockets handshake response
 */
PUBLIC bool httpVerifyWebSocketsHandshake(HttpConn *conn)
{
    HttpRx          *rx;
    HttpTx          *tx;
    cchar           *key, *expected;

    rx = conn->rx;
    tx = conn->tx;
    assert(rx);
    assert(rx->webSocket);
    assert(conn->upgraded);
    assert(httpClientConn(conn));

    rx->webSocket->state = WS_STATE_CLOSED;

    if (rx->status != HTTP_CODE_SWITCHING) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake status %d", rx->status);
        return 0;
    }
    if (!smatch(httpGetHeader(conn, "connection"), "Upgrade")) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Connection header");
        return 0;
    }
    if (!smatch(httpGetHeader(conn, "upgrade"), "WebSocket")) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Upgrade header");
        return 0;
    }
    expected = mprGetSHABase64(sjoin(tx->webSockKey, WS_MAGIC, NULL));
    key = httpGetHeader(conn, "sec-websocket-accept");
    if (!smatch(key, expected)) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake key\n%s\n%s", key, expected);
        return 0;
    }
    rx->webSocket->state = WS_STATE_OPEN;
    return 1;
}


static void traceErrorProc(HttpConn *conn, cchar *fmt, ...)
{
    HttpWebSocket   *ws;
    va_list         args;

    ws = conn->rx->webSocket;
    va_start(args, fmt);
    ws->errorMsg = sfmtv(fmt, args);
    va_end(args);

    httpTrace(conn, "rx.websockets.error", "error", "msg:'%s'", ws->errorMsg);
}

#endif /* ME_HTTP_WEB_SOCKETS */
/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

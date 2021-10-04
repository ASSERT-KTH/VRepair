/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * driver.c --
 *
 *      Connection I/O for loadable socket drivers.
 */

#include "nsd.h"

/*
 * The following are valid driver state flags.
 */

#define DRIVER_STARTED           1u
#define DRIVER_STOPPED           2u
#define DRIVER_SHUTDOWN          4u
#define DRIVER_FAILED            8u


/*
 * Constants for SockState return and reason codes.
 */

typedef enum {
    SOCK_READY =               0,
    SOCK_MORE =                1,
    SOCK_SPOOL =               2,
    SOCK_ERROR =              -1,
    SOCK_CLOSE =              -2,
    SOCK_CLOSETIMEOUT =       -3,
    SOCK_READTIMEOUT =        -4,
    SOCK_WRITETIMEOUT =       -5,
    SOCK_READERROR =          -6,
    SOCK_WRITEERROR =         -7,
    SOCK_SHUTERROR =          -8,
    SOCK_BADREQUEST =         -9,
    SOCK_ENTITYTOOLARGE =     -10,
    SOCK_BADHEADER =          -11,
    SOCK_TOOMANYHEADERS =     -12
} SockState;

/*
 * Subset for spooler states
 */
typedef enum {
    SPOOLER_CLOSE =             SOCK_CLOSE,
    SPOOLER_OK =                SOCK_READY,
    SPOOLER_READERROR =         SOCK_READERROR,
    SPOOLER_WRITEERROR =        SOCK_WRITEERROR,
    SPOOLER_CLOSETIMEOUT =      SOCK_CLOSETIMEOUT
} SpoolerState;

typedef struct {
    SpoolerState spoolerState;
    SockState    sockState;
} SpoolerStateMap;
/*
 * ServerMap maintains Host header to server mappings.
 */
typedef struct ServerMap {
    NsServer *servPtr;
    char      location[1];
} ServerMap;

/*
 * The following maintains the spooler state mapping
 */
static const SpoolerStateMap spoolerStateMap[] = {
    {SPOOLER_CLOSE,        SOCK_CLOSE},
    {SPOOLER_READERROR,    SOCK_READERROR},
    {SPOOLER_WRITEERROR,   SOCK_WRITEERROR},
    {SPOOLER_CLOSETIMEOUT, SOCK_CLOSETIMEOUT},
    {SPOOLER_OK,           SOCK_READY}
};
/*
 * The following structure manages polling.  The PollIn macro is
 * used for the common case of checking for readability.
 */

typedef struct PollData {
    unsigned int nfds;          /* Number of fds being monitored. */
    unsigned int maxfds;        /* Max fds (will grow as needed). */
    struct pollfd *pfds;        /* Dynamic array of poll structs. */
    Ns_Time timeout;            /* Min timeout, if any, for next spin. */
} PollData;

#define PollIn(ppd, i)           (((ppd)->pfds[(i)].revents & POLLIN)  == POLLIN )
#define PollOut(ppd, i)          (((ppd)->pfds[(i)].revents & POLLOUT) == POLLOUT)
#define PollHup(ppd, i)          (((ppd)->pfds[(i)].revents & POLLHUP) == POLLHUP)

/*
 * Collected informationof writer threads for per pool rates, necessary for
 * per pool bandwidth management.
 */
typedef struct ConnPoolInfo {
    size_t    threadSlot;
    int       currentPoolRate;
    int       deltaPercentage;
} ConnPoolInfo;

/*
 * The following structure maintains writer socket
 */
typedef struct WriterSock {
    struct WriterSock   *nextPtr;
    struct Sock         *sockPtr;
    struct SpoolerQueue *queuePtr;
    struct Conn         *connPtr;
    SpoolerState         status;
    int                  err;
    int                  refCount;
    unsigned int         flags;
    Tcl_WideInt          nsent;
    size_t               size;
    NsWriterStreamState  doStream;
    int                  fd;
    char                *headerString;
    struct ConnPool     *poolPtr;

    union {
        struct {
            struct iovec      *bufs;                 /* incoming bufs to be sent */
            int                nbufs;
            int                bufIdx;
            struct iovec       sbufs[UIO_SMALLIOV];  /* scratch bufs for handling partial sends */
            int                nsbufs;
            int                sbufIdx;
            struct iovec       preallocated_bufs[UIO_SMALLIOV];
            struct FileMap     fmap;
        } mem;

        struct {
            size_t             maxsize;
            size_t             bufsize;
            off_t              bufoffset;
            size_t             toRead;
            unsigned char     *buf;
            Ns_FileVec        *bufs;
            int                nbufs;
            int                currentbuf;
            Ns_Mutex           fdlock;
        } file;
    } c;

    char              *clientData;
    Ns_Time            startTime;
    int                rateLimit;
    int                currentRate;
    ConnPoolInfo      *infoPtr;
    bool               keep;

} WriterSock;

/*
 * Async writer definitions
 */

typedef struct AsyncWriter {
    Ns_Mutex      lock;        /* Lock around writer queues */
    SpoolerQueue *firstPtr;    /* List of writer threads */
} AsyncWriter;

/*
 * AsyncWriteData is similar to WriterSock
 */
typedef struct AsyncWriteData {
    struct AsyncWriteData *nextPtr;
    char                  *data;
    int                    fd;
    Tcl_WideInt            nsent;
    size_t                 size;
    size_t                 bufsize;
    const char            *buf;
} AsyncWriteData;

static AsyncWriter *asyncWriter = NULL;

/*
 * Static functions defined in this file.
 */

static Ns_ThreadProc DriverThread;
static Ns_ThreadProc SpoolerThread;
static Ns_ThreadProc WriterThread;
static Ns_ThreadProc AsyncWriterThread;

static Tcl_ObjCmdProc WriterListObjCmd;
static Tcl_ObjCmdProc WriterSizeObjCmd;
static Tcl_ObjCmdProc WriterStreamingObjCmd;
static Tcl_ObjCmdProc WriterSubmitObjCmd;
static Tcl_ObjCmdProc WriterSubmitFileObjCmd;

static Tcl_ObjCmdProc AsyncLogfileWriteObjCmd;
static Tcl_ObjCmdProc AsyncLogfileOpenObjCmd;
static Tcl_ObjCmdProc AsyncLogfileCloseObjCmd;

static Ns_ReturnCode DriverWriterFromObj(Tcl_Interp *interp, Tcl_Obj *driverObj,
                                         Ns_Conn *conn, DrvWriter **wrPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

static NS_SOCKET DriverListen(Driver *drvPtr, const char *bindaddr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static NS_DRIVER_ACCEPT_STATUS DriverAccept(Sock *sockPtr, NS_SOCKET sock)
    NS_GNUC_NONNULL(1);
static bool    DriverKeep(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void    DriverClose(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode DriverInit(const char *server, const char *moduleName, const char *threadName,
                                const Ns_DriverInitData *init,
                                NsServer *servPtr, const char *path,
                                const char *bindaddrs,
                                const char *defserver, const char *host)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(6)
    NS_GNUC_NONNULL(7) NS_GNUC_NONNULL(9);
static bool DriverModuleInitialized(const char *module)
    NS_GNUC_NONNULL(1);

static void  SockSetServer(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static SockState SockAccept(Driver *drvPtr, NS_SOCKET sock, Sock **sockPtrPtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode SockQueue(Sock *sockPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);

static Sock *SockNew(Driver *drvPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
static void  SockRelease(Sock *sockPtr, SockState reason, int err)
    NS_GNUC_NONNULL(1);

static void  SockError(Sock *sockPtr, SockState reason, int err)
    NS_GNUC_NONNULL(1);
static void  SockSendResponse(Sock *sockPtr, int code, const char *errMsg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static void  SockTrigger(NS_SOCKET sock);
static void  SockTimeout(Sock *sockPtr, const Ns_Time *nowPtr, const Ns_Time *timeout)
    NS_GNUC_NONNULL(1);
static void  SockClose(Sock *sockPtr, int keep)
    NS_GNUC_NONNULL(1);
static SockState SockRead(Sock *sockPtr, int spooler, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);
static SockState SockParse(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void SockPoll(Sock *sockPtr, short type, PollData *pdata)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static int  SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc)
    NS_GNUC_NONNULL(2);
static void SpoolerQueueStop(SpoolerQueue *queuePtr, const Ns_Time *timeoutPtr, const char *name)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void PollCreate(PollData *pdata)
    NS_GNUC_NONNULL(1);
static void PollFree(PollData *pdata)
    NS_GNUC_NONNULL(1);
static void PollReset(PollData *pdata)
    NS_GNUC_NONNULL(1);
static NS_POLL_NFDS_TYPE PollSet(PollData *pdata, NS_SOCKET sock, short type, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);
static int PollWait(const PollData *pdata, int timeout)
    NS_GNUC_NONNULL(1);
static bool ChunkedDecode(Request *reqPtr, bool update)
    NS_GNUC_NONNULL(1);
static WriterSock *WriterSockRequire(const Conn *connPtr)
    NS_GNUC_NONNULL(1);
static void WriterSockRelease(WriterSock *wrSockPtr)
    NS_GNUC_NONNULL(1);
static SpoolerState WriterReadFromSpool(WriterSock *curPtr)
    NS_GNUC_NONNULL(1);
static SpoolerState WriterSend(WriterSock *curPtr, int *err)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode WriterSetupStreamingMode(Conn *connPtr, struct iovec *bufs, int nbufs, int *fdPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static void WriterSockFileVecCleanup(WriterSock *wrSockPtr)
    NS_GNUC_NONNULL(1);
static int WriterGetMemunitFromDict(Tcl_Interp *interp, Tcl_Obj *dictObj, Tcl_Obj *keyObj, Ns_ObjvValueRange *rangePtr,
                                    Tcl_WideInt *valuePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

static void AsyncWriterRelease(AsyncWriteData *wdPtr)
    NS_GNUC_NONNULL(1);

static void WriteWarningRaw(const char *msg, int fd, size_t wantWrite, ssize_t written)
    NS_GNUC_NONNULL(1);
static const char *GetSockStateName(SockState sockState);

static size_t EndOfHeader(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void RequestNew(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void RequestFree(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void LogBuffer(Ns_LogSeverity severity, const char *msg, const char *buffer, size_t len)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void ServerMapEntryAdd(Tcl_DString *dsPtr, const char *host,
                              NsServer *servPtr, Driver *drvPtr, bool addDefaultMapEntry)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static Driver *LookupDriver(Tcl_Interp *interp, const char* protocol, const char *driverName)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void WriterPerPoolRates(WriterSock *writePtr, Tcl_HashTable *pools)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ConnPoolInfo *WriterGetInfoPtr(WriterSock *curPtr, Tcl_HashTable *pools)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Global variables defined in this file.
 */
//NS_EXTERN Ns_LogSeverity Ns_LogAccessDebug;

Ns_LogSeverity Ns_LogTaskDebug;
Ns_LogSeverity Ns_LogRequestDebug;
Ns_LogSeverity Ns_LogConnchanDebug;
Ns_LogSeverity Ns_LogUrlspaceDebug;
Ns_LogSeverity Ns_LogAccessDebug;
Ns_LogSeverity Ns_LogTimeoutDebug;

NS_EXPORT Ns_LogSeverity Ns_LogAccessDebug;

bool NsWriterBandwidthManagement = NS_FALSE;

static Ns_LogSeverity   WriterDebug;        /* Severity at which to log verbose debugging. */
static Ns_LogSeverity   DriverDebug;        /* Severity at which to log verbose debugging. */
static Ns_Mutex         reqLock     = NULL; /* Lock for allocated Request structure pool */
static Ns_Mutex         writerlock  = NULL; /* Lock updating streaming information in the writer */
static Request         *firstReqPtr = NULL; /* Allocated request structures kept in a pool */
static Driver          *firstDrvPtr = NULL; /* First in list of all drivers */

#define Push(x, xs) ((x)->nextPtr = (xs), (xs) = (x))


/*
 *----------------------------------------------------------------------
 *
 * WriteWarningRaw --
 *
 *      Write a warning message to stderr. This function is for cases, where
 *      writing to Ns_Log can't be used (e.g. in the AsyncWriter, which is
 *      used for writing also to the system log).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Line to stderr.
 *
 *----------------------------------------------------------------------
 */
static void
WriteWarningRaw(const char *msg, int fd, size_t wantWrite, ssize_t written)
{
    fprintf(stderr, "%s: Warning: wanted to write %" PRIuz
            " bytes, wrote %ld to file descriptor %d\n",
            msg, wantWrite, (long)written, fd);
}


/*
 *----------------------------------------------------------------------
 *
 * GetSockStateName --
 *
 *      Return human readable names for StockState values.
 *
 * Results:
 *      string
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
GetSockStateName(SockState sockState)
{
    int sockStateInt = (int)sockState;
    static const char *sockStateStrings[] = {
        "SOCK_READY",
        "SOCK_MORE",
        "SOCK_SPOOL",
        "SOCK_ERROR",
        "SOCK_CLOSE",
        "SOCK_CLOSETIMEOUT",
        "SOCK_READTIMEOUT",
        "SOCK_WRITETIMEOUT",
        "SOCK_READERROR",
        "SOCK_WRITEERROR",
        "SOCK_SHUTERROR",
        "SOCK_BADREQUEST",
        "SOCK_ENTITYTOOLARGE",
        "SOCK_BADHEADER",
        "SOCK_TOOMANYHEADERS",
        NULL
    };

    if (sockStateInt < 0) {
        sockStateInt = (- sockStateInt) + 2;
    }
    assert(sockStateInt < Ns_NrElements(sockStateStrings));
    return sockStateStrings[sockStateInt];
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitDrivers --
 *
 *      Init drivers system.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsInitDrivers(void)
{
    DriverDebug = Ns_CreateLogSeverity("Debug(ns:driver)");
    WriterDebug = Ns_CreateLogSeverity("Debug(writer)");
    Ns_LogTaskDebug = Ns_CreateLogSeverity("Debug(task)");
    Ns_LogRequestDebug = Ns_CreateLogSeverity("Debug(request)");
    Ns_LogConnchanDebug = Ns_CreateLogSeverity("Debug(connchan)");
    Ns_LogUrlspaceDebug = Ns_CreateLogSeverity("Debug(urlspace)");
    Ns_LogAccessDebug = Ns_CreateLogSeverity("Debug(access)");
    Ns_LogTimeoutDebug = Ns_CreateLogSeverity("Debug(timeout)");
    Ns_MutexInit(&reqLock);
    Ns_MutexInit(&writerlock);
    Ns_MutexSetName2(&reqLock, "ns:driver", "requestpool");
    Ns_MutexSetName2(&writerlock, "ns:writer", "stream");
}


/*
 *----------------------------------------------------------------------
 *
 * DriverModuleInitialized --
 *
 *      Check if a driver with the specified name is already initialized.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
DriverModuleInitialized(const char *module)
{
    Driver *drvPtr;
    bool found = NS_FALSE;

    NS_NONNULL_ASSERT(module != NULL);

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {

        if (strcmp(drvPtr->moduleName, module) == 0) {
            found = NS_TRUE;
            Ns_Log(Notice, "Driver %s is already initialized", module);
            break;
        }
    }

    return found;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DriverInit --
 *
 *      Initialize a driver.
 *
 * Results:
 *      NS_OK if initialized, NS_ERROR if config or other error.
 *
 * Side effects:
 *      Listen socket will be opened later in NsStartDrivers.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_DriverInit(const char *server, const char *module, const Ns_DriverInitData *init)
{
    Ns_ReturnCode  status = NS_OK;
    NsServer      *servPtr = NULL;
    bool           alreadyInitialized = NS_FALSE;

    NS_NONNULL_ASSERT(module != NULL);
    NS_NONNULL_ASSERT(init != NULL);

    /*
     * If a server is provided, servPtr must be set.
     */
    if (server != NULL) {
        servPtr = NsGetServer(server);
        if (unlikely(servPtr == NULL)) {
            Ns_Log(Bug, "cannot lookup server structure for server: %s", module);
            status = NS_ERROR;
        }
    } else {
        alreadyInitialized = DriverModuleInitialized(module);
    }

    /*
     * Check versions of drivers.
     */
    if (status == NS_OK && init->version < NS_DRIVER_VERSION_4) {
        Ns_Log(Warning, "%s: driver version is too old (version %d), Version 4 is recommended",
               module, init->version);
    }
#ifdef HAVE_IPV6
    if (status == NS_OK && init->version < NS_DRIVER_VERSION_3) {
        Ns_Log(Error, "%s: driver version is too old (version %d) and does not support IPv6",
               module, init->version);
        status = NS_ERROR;
    }
#endif
    if (status == NS_OK && init->version < NS_DRIVER_VERSION_2) {
        Ns_Log(Error, "%s: version field of driver is invalid: %d",
               module, init->version);
        status = NS_ERROR;
    }

    if (!alreadyInitialized && status == NS_OK) {
        const char *path, *host, *address, *defserver;
        bool        noHostNameGiven;
        int         nrDrivers, nrBindaddrs = 0, result;
        Ns_Set     *set;
        Tcl_Obj    *bindaddrsObj, **objv;

        path = ((init->path != NULL) ? init->path : Ns_ConfigGetPath(server, module, (char *)0L));

        set = Ns_ConfigCreateSection(path);

        /*
         * Determine the "defaultserver" the "hostname" / "address" for
         * binding to and/or the HTTP location string.
         */
        defserver = Ns_ConfigGetValue(path, "defaultserver");

        address = Ns_ConfigGetValue(path, "address");
        host = Ns_ConfigGetValue(path, "hostname");
        noHostNameGiven = (host == NULL);

        /*
         * If the listen address was not specified, attempt to determine it
         * through a DNS lookup of the specified hostname or the server's
         * primary hostname.
         */
        if (address == NULL) {
            Tcl_DString  ds;

            Tcl_DStringInit(&ds);
            if (noHostNameGiven) {
                host = Ns_InfoHostname();
            }

            if (Ns_GetAllAddrByHost(&ds, host) == NS_TRUE) {
                address = ns_strdup(Tcl_DStringValue(&ds));
                if (path != NULL) {
                    Ns_SetUpdate(set, "address", address);
                }
                Ns_Log(Notice, "no address given, obtained address '%s' from host name %s", address, host);

            }
            Tcl_DStringFree(&ds);
        }

        if (address == NULL) {
            address = NS_IP_UNSPECIFIED;
            Ns_Log(Notice, "no address given, set address to unspecified address %s", address);
        }

        bindaddrsObj = Tcl_NewStringObj(address, -1);
        result = Tcl_ListObjGetElements(NULL, bindaddrsObj, &nrBindaddrs, &objv);
        if (result != TCL_OK || nrBindaddrs < 1 || nrBindaddrs >= MAX_LISTEN_ADDR_PER_DRIVER) {
            Ns_Fatal("%s: bindaddrs '%s' is not a valid Tcl list containing addresses (max %d)",
                     module, address, MAX_LISTEN_ADDR_PER_DRIVER);
        }
        Tcl_IncrRefCount(bindaddrsObj);

        /*
         * If the hostname was not specified and not determined by the lookup
         * above, set it to the first specified or derived IP address string.
         */
        if (host == NULL) {
            host = ns_strdup(Tcl_GetString(objv[0]));
        }

        if (noHostNameGiven && host != NULL && path != NULL) {
            Ns_SetUpdate(set, "hostname", host);
        }
        Tcl_DecrRefCount(bindaddrsObj);

        /*
         * Get configured number of driver threads.
         */
        nrDrivers = Ns_ConfigIntRange(path, "driverthreads", 1, 1, 64);
        if (nrDrivers > 1) {
#if !defined(SO_REUSEPORT)
            Ns_Log(Warning,
                   "server %s module %s requests %d driverthreads, but is not supported by the operating system",
                   server, module, nrDrivers);
            Ns_SetUpdate(set, "driverthreads", "1");
            nrDrivers = 1;
#endif
        }

        /*
         * The common parameters are determined, create the driver thread(s)
         */
        {
            size_t maxModuleNameLength = strlen(module) + (size_t)TCL_INTEGER_SPACE + 1u;
            char  *moduleName = ns_malloc(maxModuleNameLength);
            int    i;

            if (host == NULL) {
                host = Ns_InfoHostname();
            }

            for (i = 0; i < nrDrivers; i++) {
                snprintf(moduleName, maxModuleNameLength, "%s:%d", module, i);
                status = DriverInit(server, module, moduleName, init,
                                    servPtr, path,
                                    address, defserver, host);
                if (status != NS_OK) {
                    break;
                }
            }
            ns_free(moduleName);
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerMapEntryAdd --
 *
 *      Add an entry to the virtual server map. The entry consists of the
 *      value as provided by the host header field and location string,
 *      containing as well the protocol.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Potentially adding an entry to the virtual server map.
 *
 *----------------------------------------------------------------------
 */

static void
ServerMapEntryAdd(Tcl_DString *dsPtr, const char *host,
                  NsServer *servPtr, Driver *drvPtr, bool addDefaultMapEntry) {
    Tcl_HashEntry *hPtr;
    int            isNew;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(host != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(drvPtr != NULL);

    hPtr = Tcl_CreateHashEntry(&drvPtr->hosts, host, &isNew);
    if (isNew != 0) {
        ServerMap *mapPtr;

        (void) Ns_DStringVarAppend(dsPtr, drvPtr->protocol, "://", host, (char *)0L);
        mapPtr = ns_malloc(sizeof(ServerMap) + (size_t)dsPtr->length);
        mapPtr->servPtr  = servPtr;
        memcpy(mapPtr->location, dsPtr->string, (size_t)dsPtr->length + 1u);

        Tcl_SetHashValue(hPtr, mapPtr);
        Ns_Log(Notice, "%s: adding virtual host entry for host <%s> location: %s mapped to server: %s",
               drvPtr->threadName, host, mapPtr->location, servPtr->server);

        if (addDefaultMapEntry) {
            drvPtr->defMapPtr = mapPtr;
        }
        /*
         * Always reset the Tcl_DString
         */
        Ns_DStringSetLength(dsPtr, 0);
    } else {
        Ns_Log(Notice, "%s: ignore duplicate virtual host entry: %s",
               drvPtr->threadName, host);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsDriverMapVirtualServers --
 *
 *      Map "Host:" headers for drivers not bound to physical servers.  This
 *      function has to be called a time, when all servers are already defined
 *      such that NsGetServer(server) can succeed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Add an entry to the virtual server map via ServerMapEntryAdd()
 *
 *----------------------------------------------------------------------
 */
void NsDriverMapVirtualServers(void)
{
    Driver *drvPtr;

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        const Ns_Set *lset;
        size_t        j;
        Tcl_DString   ds, *dsPtr = &ds;
        const char   *path, *defserver, *moduleName;

        moduleName = drvPtr->moduleName;
        defserver  = drvPtr->defserver;

        /*
         * Check for a "/servers" section for this driver module.
         */
        path = Ns_ConfigGetPath(NULL, moduleName, "servers", (char *)0L);
        lset = Ns_ConfigGetSection(path);

        if (lset == NULL || Ns_SetSize(lset) == 0u) {
            /*
             * The driver module has no (or empty) ".../servers" section.
             * There is no mapping from host name to virtual server defined.
             */
            if (drvPtr->server == NULL) {

                /*
                 * We have a global driver module. If there is at least a
                 * default server configured, we can use this for the mapping
                 * to the default server.
                 */
                if (defserver != NULL) {
                    NsServer   *servPtr = NsGetServer(defserver);

                    Tcl_DStringInit(dsPtr);
                    ServerMapEntryAdd(dsPtr, Ns_InfoHostname(), servPtr, drvPtr, NS_TRUE);
                    Tcl_DStringFree(dsPtr);
                    Ns_Log(Notice, "Global driver has no mapping from host to server (section '%s' missing)",
                           moduleName);
                } else {
                    /*
                     * Global driver, which has no default server, and no servers section.
                     */
                    Ns_Fatal("%s: virtual servers configured,"
                             " but '%s' has no defaultserver defined", moduleName, path);
                }
            }

            continue;
        }

        /*
         * We have a ".../servers" section, the driver might be global or
         * local. It is not clear, why we need the server map for the local
         * driver, but for compatibility, we keep this.
         *
         */
        if (defserver == NULL) {
            if (drvPtr->server != NULL) {
                /*
                 * We have a local (server specific) driver.  Since the code
                 * below assumes that we have a "defserver" set, we take the
                 * actual server as defserver.
                 */
                defserver = drvPtr->server;
            } else {
                /*
                 * We have a global driver, but no defserver.
                 */
                Ns_Fatal("%s: virtual servers configured,"
                         " but '%s' has no defaultserver defined", moduleName, path);
            }
        }

        assert(defserver != NULL);

        drvPtr->defMapPtr = NULL;

        Ns_DStringInit(dsPtr);
        for (j = 0u; j < Ns_SetSize(lset); ++j) {
            const char *server  = Ns_SetKey(lset, j);
            const char *host    = Ns_SetValue(lset, j);
            NsServer   *servPtr;

            /*
             * Perform an explicit lookup of the server.
             */
            servPtr = NsGetServer(server);
            if (servPtr == NULL) {
                Ns_Log(Error, "%s: no such server: %s", moduleName, server);
            } else {
                char *writableHost, *hostName, *portStart;

                writableHost = ns_strdup(host);
                Ns_HttpParseHost(writableHost, &hostName, &portStart);

                if (portStart == NULL) {
                    Tcl_DString hostDString;

                    /*
                     * The provided host entry does NOT contain a port.
                     *
                     * Add the provided entry to the virtual server map only,
                     * when the configured port is the default port for the
                     * protocol.
                     */
                    if (drvPtr->port == drvPtr->defport) {
                        ServerMapEntryAdd(dsPtr, host, servPtr, drvPtr,
                                          STREQ(defserver, server));
                    }

                    /*
                     * Auto-add configured port: Add always an entry with the
                     * explicitly configured port of the driver.
                     */
                    Tcl_DStringInit(&hostDString);
                    Tcl_DStringAppend(&hostDString, host, -1);
                    (void) Ns_DStringPrintf(&hostDString, ":%hu", drvPtr->port);

                    ServerMapEntryAdd(dsPtr, hostDString.string, servPtr, drvPtr,
                                      STREQ(defserver, server));

                    Tcl_DStringFree(&hostDString);
                } else {
                    /*
                     * The provided host entry does contain a port.
                     *
                     * In case, the provided port is equal to the configured port
                     * of the driver, add an entry.
                     */
                    unsigned short providedPort = (unsigned short)strtol(portStart+1, NULL, 10);

                    if (providedPort == drvPtr->port) {
                        ServerMapEntryAdd(dsPtr, host, servPtr, drvPtr,
                                          STREQ(defserver, server));
                        /*
                         * In case, the provided port is equal to the default
                         * port of the driver, make sure that we have an entry
                         * without the port.
                         */
                        if (providedPort == drvPtr->defport) {
                            ServerMapEntryAdd(dsPtr, hostName, servPtr, drvPtr,
                                              STREQ(defserver, server));
                        }
                    } else {
                        Ns_Log(Warning, "%s: driver is listening on port %hu; "
                               "virtual host entry %s ignored",
                               moduleName, drvPtr->port, host);
                    }
                }
                ns_free(writableHost);
            }
        }
        Ns_DStringFree(dsPtr);

        if (drvPtr->defMapPtr == NULL) {
            fprintf(stderr, "--- Server Map: ---\n");
            Ns_SetPrint(lset);
            Ns_Fatal("%s: default server '%s' not defined in '%s'", moduleName, defserver, path);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DriverInit --
 *
 *      Helper function of Ns_DriverInit. This function actually allocates and
 *      initialized the driver structure.
 *
 * Results:
 *      NS_OK if initialized, NS_ERROR if config or other error.
 *
 * Side effects:
 *      Listen socket will be opened later in NsStartDrivers.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
DriverInit(const char *server, const char *moduleName, const char *threadName,
           const Ns_DriverInitData *init,
           NsServer *servPtr, const char *path,
           const char *bindaddrs, const char *defserver, const char *host)
{
    const char     *defproto;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    DrvSpooler     *spPtr;
    int             i;
    unsigned short  defport;

    NS_NONNULL_ASSERT(threadName != NULL);
    NS_NONNULL_ASSERT(init != NULL);
    NS_NONNULL_ASSERT(path != NULL);
    NS_NONNULL_ASSERT(bindaddrs != NULL);
    NS_NONNULL_ASSERT(host != NULL);

    /*
     * Set the protocol and port defaults.
     */
    if (init->protocol != NULL) {
        defproto = init->protocol;
        defport = init->defaultPort;
    } else {
        defproto = "unknown";
        defport = 0u;
    }
    Ns_Log(DriverDebug, "DriverInit server <%s> threadName %s proto %s port %hu",
           server, threadName, defproto, defport);

    /*
     * Allocate a new driver instance and set configurable parameters.
     */

    drvPtr = ns_calloc(1u, sizeof(Driver));

    Ns_MutexInit(&drvPtr->lock);
    Ns_MutexSetName2(&drvPtr->lock, "ns:drv", threadName);

    Ns_MutexInit(&drvPtr->spooler.lock);
    Ns_MutexSetName2(&drvPtr->spooler.lock, "ns:drv:spool", threadName);

    Ns_MutexInit(&drvPtr->writer.lock);
    Ns_MutexSetName2(&drvPtr->writer.lock, "ns:drv:writer", threadName);

    if (ns_sockpair(drvPtr->trigger) != 0) {
        Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
    }

    drvPtr->server         = server;
    drvPtr->type           = init->name;
    drvPtr->moduleName     = ns_strdup(moduleName);
    drvPtr->threadName     = ns_strdup(threadName);
    drvPtr->defserver      = defserver;
    drvPtr->listenProc     = init->listenProc;
    drvPtr->acceptProc     = init->acceptProc;
    drvPtr->recvProc       = init->recvProc;
    drvPtr->sendProc       = init->sendProc;
    drvPtr->sendFileProc   = init->sendFileProc;
    drvPtr->keepProc       = init->keepProc;
    drvPtr->requestProc    = init->requestProc;
    drvPtr->closeProc      = init->closeProc;
    drvPtr->clientInitProc = init->clientInitProc;
    drvPtr->arg            = init->arg;
    drvPtr->opts           = init->opts;
    drvPtr->servPtr        = servPtr;
    drvPtr->defport        = defport;

    drvPtr->bufsize        = (size_t)Ns_ConfigMemUnitRange(path, "bufsize", 16384, 1024, INT_MAX);
    drvPtr->maxinput       = Ns_ConfigMemUnitRange(path, "maxinput", 1024*1024, 1024, LLONG_MAX);
    drvPtr->maxupload      = Ns_ConfigMemUnitRange(path, "maxupload", 0, 0, (Tcl_WideInt)drvPtr->maxinput);
    drvPtr->readahead      = Ns_ConfigMemUnitRange(path, "readahead", (Tcl_WideInt)drvPtr->bufsize,
                                                   (Tcl_WideInt)drvPtr->bufsize, drvPtr->maxinput);

    drvPtr->maxline        = Ns_ConfigIntRange(path, "maxline",      8192, 256, INT_MAX);
    drvPtr->maxheaders     = Ns_ConfigIntRange(path, "maxheaders",    128,   8, INT_MAX);
    drvPtr->maxqueuesize   = Ns_ConfigIntRange(path, "maxqueuesize", 1024,   1, INT_MAX);

    Ns_ConfigTimeUnitRange(path, "sendwait",
                           "30s", 1, 0, INT_MAX, 0, &drvPtr->sendwait);
    Ns_ConfigTimeUnitRange(path, "recvwait",
                           "30s", 1, 0, INT_MAX, 0, &drvPtr->recvwait);
    Ns_ConfigTimeUnitRange(path, "closewait",
                           "2s", 0, 0, INT_MAX, 0, &drvPtr->closewait);
    Ns_ConfigTimeUnitRange(path, "keepwait",
                           "5s", 0, 0, INT_MAX, 0, &drvPtr->keepwait);

    drvPtr->backlog        = Ns_ConfigIntRange(path, "backlog",       256,   1, INT_MAX);
    drvPtr->driverthreads  = Ns_ConfigIntRange(path, "driverthreads",   1,   1, 32);
    drvPtr->reuseport      = Ns_ConfigBool(path,     "reuseport",       NS_FALSE);
    drvPtr->acceptsize     = Ns_ConfigIntRange(path, "acceptsize", drvPtr->backlog, 1, INT_MAX);

    drvPtr->keepmaxuploadsize   = (size_t)Ns_ConfigMemUnitRange(path, "keepalivemaxuploadsize",
                                                            0, 0, INT_MAX);
    drvPtr->keepmaxdownloadsize = (size_t)Ns_ConfigMemUnitRange(path, "keepalivemaxdownloadsize",
                                                            0, 0, INT_MAX);
    drvPtr->recvTimeout = drvPtr->recvwait;

    Tcl_InitHashTable(&drvPtr->hosts, TCL_STRING_KEYS);

    if (drvPtr->driverthreads > 1) {
#if !defined(SO_REUSEPORT)
        drvPtr->driverthreads = 1;
        drvPtr->reuseport = NS_FALSE;
#else
        /*
         * When driver threads > 1, "reuseport" has to be active.
         */
        drvPtr->reuseport = NS_TRUE;
#endif
    }
    if (drvPtr->reuseport) {
        /*
         * Reuseport was specified
         */
#if !defined(SO_REUSEPORT)
        Ns_Log(Warning,
               "parameter %s reuseport was specified, but is not supported by the operating system",
               path);
        drvPtr->reuseport = NS_FALSE;
#endif
    }

    drvPtr->uploadpath = ns_strdup(Ns_ConfigString(path, "uploadpath", nsconf.tmpDir));

    /*
     * If activated, "maxupload" has to be at least "readahead" bytes. Tell
     * the user in case the config values are overruled.
     */
    if ((drvPtr->maxupload > 0) &&
        (drvPtr->maxupload < drvPtr->readahead)) {
        Ns_Log(Warning,
               "parameter %s maxupload % " TCL_LL_MODIFIER
               "d invalid; can be either 0 or must be >= %" TCL_LL_MODIFIER
               "d (size of readahead)",
               path, drvPtr->maxupload, drvPtr->readahead);
        drvPtr->maxupload = drvPtr->readahead;
    }

    /*
     * Determine the port and then set the HTTP location string either
     * as specified in the config file or constructed from the
     * protocol, hostname and port.
     */

    drvPtr->protocol     = ns_strdup(defproto);
    drvPtr->address      = ns_strdup(bindaddrs);
    drvPtr->port         = (unsigned short)Ns_ConfigIntRange(path, "port", (int)defport, 0, 65535);
    drvPtr->location     = Ns_ConfigGetValue(path, "location");

    if (drvPtr->location != NULL && (strstr(drvPtr->location, "://") != NULL)) {
        drvPtr->location = ns_strdup(drvPtr->location);
    } else {
        Tcl_DString ds, *dsPtr = &ds;

        Ns_DStringInit(dsPtr);
        Ns_HttpLocationString(dsPtr, drvPtr->protocol, host, drvPtr->port, defport);
        drvPtr->location = Ns_DStringExport(dsPtr);
    }

    drvPtr->nextPtr = firstDrvPtr;
    firstDrvPtr = drvPtr;

    /*
     * Add driver specific extra headers.
     */
    drvPtr->extraHeaders = Ns_ConfigSet(path, "extraheaders");

    /*
     * Check if upload spooler are enabled
     */
    spPtr = &drvPtr->spooler;
    spPtr->threads = Ns_ConfigIntRange(path, "spoolerthreads", 0, 0, 32);

    if (spPtr->threads > 0) {
        Ns_Log(Notice, "%s: enable %d spooler thread(s) "
               "for uploads >= %" TCL_LL_MODIFIER "d bytes", threadName,
               spPtr->threads, drvPtr->readahead);

        for (i = 0; i < spPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1u, sizeof(SpoolerQueue));
            char          buffer[100];

            snprintf(buffer, sizeof(buffer), "ns:driver:spooler:%d", i);
            Ns_MutexSetName2(&queuePtr->lock, buffer, "queue");
            queuePtr->id = i;
            Push(queuePtr, spPtr->firstPtr);
        }
    } else {
        Ns_Log(Notice, "%s: enable %d spooler thread(s) ",
               threadName, spPtr->threads);
    }

    /*
     * Enable writer threads
     */

    wrPtr = &drvPtr->writer;
    wrPtr->threads = Ns_ConfigIntRange(path, "writerthreads", 0, 0, 32);

    if (wrPtr->threads > 0) {
        wrPtr->writersize = (size_t)Ns_ConfigMemUnitRange(path, "writersize",
                                                          1024*1024, 1024, INT_MAX);
        wrPtr->bufsize = (size_t)Ns_ConfigMemUnitRange(path, "writerbufsize",
                                                   8192, 512, INT_MAX);
        wrPtr->rateLimit = Ns_ConfigIntRange(path, "writerratelimit", 0, 0, INT_MAX);
        wrPtr->doStream = Ns_ConfigBool(path, "writerstreaming", NS_FALSE)
            ? NS_WRITER_STREAM_ACTIVE : NS_WRITER_STREAM_NONE;
        Ns_Log(Notice, "%s: enable %d writer thread(s) "
               "for downloads >= %" PRIdz " bytes, bufsize=%" PRIdz " bytes, HTML streaming %d",
               threadName, wrPtr->threads, wrPtr->writersize, wrPtr->bufsize, wrPtr->doStream);

        for (i = 0; i < wrPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1u, sizeof(SpoolerQueue));
            char          buffer[100];

            snprintf(buffer, sizeof(buffer), "ns:driver:writer:%d", i);
            Ns_MutexSetName2(&queuePtr->lock, buffer, "queue");
            queuePtr->id = i;
            Push(queuePtr, wrPtr->firstPtr);
        }
    } else {
        Ns_Log(Notice, "%s: enable %d writer thread(s) ",
               threadName, wrPtr->threads);
    }

    return NS_OK;
}



/*
 *----------------------------------------------------------------------
 *
 * NsStartDrivers --
 *
 *      Listen on all driver address/ports and start the DriverThread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See DriverThread.
 *
 *----------------------------------------------------------------------
 */
void
NsStartDrivers(void)
{
    Driver *drvPtr;

    /*
     * Signal and wait for each driver to start.
     */
    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {

        if (drvPtr->port == 0u) {
            /*
             * Don't start a driver having port zero.
             */
            continue;
        }

        Ns_ThreadCreate(DriverThread, drvPtr, 0, &drvPtr->thread);
        Ns_MutexLock(&drvPtr->lock);
        while ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            Ns_CondWait(&drvPtr->cond, &drvPtr->lock);
        }
        /*if ((drvPtr->flags & DRIVER_FAILED)) {
          status = NS_ERROR;
          }*/
        Ns_MutexUnlock(&drvPtr->lock);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsStopDrivers --
 *
 *      Trigger the DriverThread to begin shutdown.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      DriverThread will close listen sockets and then exit after all
 *      outstanding connections are complete and closed.
 *
 *----------------------------------------------------------------------
 */

void
NsStopDrivers(void)
{
    Driver *drvPtr;

    NsAsyncWriterQueueDisable(NS_TRUE);

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        Tcl_HashEntry  *hPtr;
        Tcl_HashSearch  search;

        if ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            continue;
        }

        Ns_MutexLock(&drvPtr->lock);
        Ns_Log(Notice, "[driver:%s]: stopping", drvPtr->threadName);
        drvPtr->flags |= DRIVER_SHUTDOWN;
        Ns_CondBroadcast(&drvPtr->cond);
        Ns_MutexUnlock(&drvPtr->lock);
        SockTrigger(drvPtr->trigger[1]);

        hPtr = Tcl_FirstHashEntry(&drvPtr->hosts, &search);
        while (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
    }
}


void
NsStopSpoolers(void)
{
    const Driver *drvPtr;

    Ns_Log(Notice, "driver: stopping writer and spooler threads");

    /*
     * Shutdown all spooler and writer threads
     */
    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        Ns_Time timeout;

        if ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            continue;
        }
        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, nsconf.shutdowntimeout.sec, nsconf.shutdowntimeout.usec);

        SpoolerQueueStop(drvPtr->writer.firstPtr, &timeout, "writer");
        SpoolerQueueStop(drvPtr->spooler.firstPtr, &timeout, "spooler");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DriverInfoObjCmd --
 *
 *      Return public info of all drivers.
 *      Subcommand of NsTclDriverObjCmd.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverInfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);
        Tcl_HashTable driverNames;     /* names of the driver modules without duplicates */

        Tcl_InitHashTable(&driverNames, TCL_STRING_KEYS);

        /*
         * Iterate over all modules, not necessarily all driver threads
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            int isNew = 0;

            (void)Tcl_CreateHashEntry(&driverNames, drvPtr->moduleName, &isNew);
            if (isNew == 1) {
                Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("module", 6));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->moduleName, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("type", 4));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->type, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("server", 6));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->server != NULL ?
                                                                           drvPtr->server : NS_EMPTY_STRING, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("location", 8));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->location, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("address", 7));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->address, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("protocol", 8));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->protocol, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("sendwait", 8));
                Tcl_ListObjAppendElement(interp, listObj, Ns_TclNewTimeObj(&drvPtr->sendwait));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("recvwait", 8));
                Tcl_ListObjAppendElement(interp, listObj, Ns_TclNewTimeObj(&drvPtr->sendwait));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("extraheaders", 12));
                if (drvPtr->extraHeaders != NULL) {
                    Tcl_DString ds;

                    Tcl_DStringInit(&ds);
                    Ns_DStringAppendSet(&ds, drvPtr->extraHeaders);
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
                    Tcl_DStringFree(&ds);
                } else {
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("", 0));
                }

                Tcl_ListObjAppendElement(interp, resultObj, listObj);
            }
        }
        Tcl_SetObjResult(interp, resultObj);
        Tcl_DeleteHashTable(&driverNames);
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * DriverStatsObjCmd --
 *
 *      Return statistics of all drivers.
 *      Subcommand of NsTclDriverObjCmd.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverStatsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {

        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);

        /*
         * Iterate over all drivers and collect results.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);


            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("thread", 6));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->threadName, -1));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("module", 6));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->moduleName, -1));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("received", 8));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.received));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("spooled", 7));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.spooled));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("partial", 7));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.partial));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("errors", 6));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.errors));

            Tcl_ListObjAppendElement(interp, resultObj, listObj);
        }
        Tcl_SetObjResult(interp, resultObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverThreadsObjCmd --
 *
 *      Return the names of driver threads
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverThreadsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);

        /*
         * Iterate over all drivers and collect results.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(drvPtr->threadName, -1));
        }
        Tcl_SetObjResult(interp, resultObj);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverNamesObjCmd --
 *
 *      Return the names of drivers.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverNamesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);
        Tcl_HashTable driverNames;     /* names of the drivers without duplicates */

        Tcl_InitHashTable(&driverNames, TCL_STRING_KEYS);

        /*
         * Iterate over all drivers and collect results.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            int            isNew;

            (void)Tcl_CreateHashEntry(&driverNames, drvPtr->moduleName, &isNew);
            if (isNew == 1) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(drvPtr->moduleName, -1));
            }
        }
        Tcl_SetObjResult(interp, resultObj);
        Tcl_DeleteHashTable(&driverNames);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclDriverObjCmd -
 *
 *      Give information about drivers. Currently, just the statistics.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclDriverObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"info",       DriverInfoObjCmd},
        {"names",      DriverNamesObjCmd},
        {"threads",    DriverThreadsObjCmd},
        {"stats",      DriverStatsObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsWakeupDriver --
 *
 *      Wake up the associated DriverThread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The poll waiting for this trigger will be interrupted.
 *
 *----------------------------------------------------------------------
 */
void
NsWakeupDriver(const Driver *drvPtr) {
    NS_NONNULL_ASSERT(drvPtr != NULL);
    SockTrigger(drvPtr->trigger[1]);
}


/*
 *----------------------------------------------------------------------
 *
 * NsWaitDriversShutdown --
 *
 *      Wait for exit of DriverThread.  This callback is invoked later
 *      by the timed shutdown thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Driver thread is joined and trigger pipe closed.
 *
 *----------------------------------------------------------------------
 */

void
NsWaitDriversShutdown(const Ns_Time *toPtr)
{
    Driver       *drvPtr;
    Ns_ReturnCode status = NS_OK;

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        if ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            continue;
        }
        Ns_MutexLock(&drvPtr->lock);
        while ((drvPtr->flags & DRIVER_STOPPED) == 0u && status == NS_OK) {
            status = Ns_CondTimedWait(&drvPtr->cond, &drvPtr->lock, toPtr);
        }
        Ns_MutexUnlock(&drvPtr->lock);
        if (status != NS_OK) {
            Ns_Log(Warning, "[driver:%s]: shutdown timeout", drvPtr->threadName);
        } else {
            Ns_Log(Notice, "[driver:%s]: stopped", drvPtr->threadName);
            Ns_ThreadJoin(&drvPtr->thread, NULL);
            drvPtr->thread = NULL;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetRequest --
 *
 *      Return the request buffer, reading it if necessary (i.e., if not an
 *      async read-ahead connection). This function is called at the start of
 *      connection processing.
 * Results:
 *      Pointer to Request structure or NULL on error.
 *
 * Side effects:
 *      May wait for content to arrive if necessary.
 *
 *----------------------------------------------------------------------
 */
Request *
NsGetRequest(Sock *sockPtr, const Ns_Time *nowPtr)
{
    Request *reqPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    /*
     * The underlying "Request" structure is allocated by RequestNew(), which
     * must be called for the "sockPtr" prior to calling this
     * function. "reqPtr" should be NULL just in error cases.
     */
    reqPtr = sockPtr->reqPtr;

    if (likely(reqPtr != NULL)) {

        if (likely(reqPtr->request.line != NULL)) {
            Ns_Log(DriverDebug, "NsGetRequest got the pre-parsed request <%s> from the driver",
                   reqPtr->request.line);

        } else if (sockPtr->drvPtr->requestProc == NULL) {
            /*
             * Non-HTTP driver can send the drvPtr->requestProc to perform
             * their own request handling.
             */
            SockState status;

            Ns_Log(DriverDebug, "NsGetRequest has to read+parse the request");
            /*
             * We have no parsed request so far. So, do it now.
             */
            do {
                Ns_Log(DriverDebug, "NsGetRequest calls SockRead");
                status = SockRead(sockPtr, 0, nowPtr);
            } while (status == SOCK_MORE);

            /*
             * If anything went wrong, clean the request provided by
             * SockRead() and flag the error by returning NULL.
             */
            if (status != SOCK_READY) {
                if (sockPtr->reqPtr != NULL) {
                    Ns_Log(DriverDebug, "NsGetRequest calls RequestFree");
                    RequestFree(sockPtr);
                }
                reqPtr = NULL;
            }
        } else {
            Ns_Log(DriverDebug, "NsGetRequest found driver specific request Proc, "
                   "probably from a non-HTTP driver");
        }
    } else {
        Ns_Log(DriverDebug, "NsGetRequest has reqPtr NULL");
    }

    return reqPtr;
}



/*
 *----------------------------------------------------------------------
 *
 * NsSockClose --
 *
 *      Return a connection to the DriverThread for closing or keepalive.
 *      "keep" might be NS_TRUE/NS_FALSE or -1 if undecided.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Socket may be reused by a keepalive connection.
 *
 *----------------------------------------------------------------------
 */

void
NsSockClose(Sock *sockPtr, int keep)
{
    Driver *drvPtr;
    bool    trigger = NS_FALSE;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    drvPtr = sockPtr->drvPtr;

    Ns_Log(DriverDebug, "NsSockClose sockPtr %p (%d) keep %d",
           (void *)sockPtr, ((Ns_Sock*)sockPtr)->sock, keep);

    SockClose(sockPtr, keep);
    /*
     * Free the request, unless it is from a non-HTTP driver (who might not
     * fill out the request structure).
     */
    if (sockPtr->reqPtr != NULL) {
        Ns_Log(DriverDebug, "NsSockClose calls RequestFree");
        RequestFree(sockPtr);
    }

    Ns_MutexLock(&drvPtr->lock);
    if (drvPtr->closePtr == NULL) {
        trigger = NS_TRUE;
    }
    sockPtr->nextPtr = drvPtr->closePtr;
    drvPtr->closePtr = sockPtr;
    Ns_MutexUnlock(&drvPtr->lock);

    if (trigger) {
        SockTrigger(drvPtr->trigger[1]);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DriverListen --
 *
 *      Open a listening socket for accepting connections.
 *
 * Results:
 *      File description of socket, or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
DriverListen(Driver *drvPtr, const char *bindaddr)
{
    NS_SOCKET sock;

    NS_NONNULL_ASSERT(drvPtr != NULL);
    NS_NONNULL_ASSERT(bindaddr != NULL);

    sock = (*drvPtr->listenProc)((Ns_Driver *) drvPtr,
                                 bindaddr,
                                 drvPtr->port,
                                 drvPtr->backlog,
                                 drvPtr->reuseport);
    if (sock == NS_INVALID_SOCKET) {
        Ns_Log(Error, "%s: failed to listen on [%s]:%d: %s",
               drvPtr->threadName, bindaddr, drvPtr->port,
               ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Notice,
#ifdef HAVE_IPV6
               "%s: listening on [%s]:%d",
#else
               "%s: listening on %s:%d",
#endif
               drvPtr->threadName, bindaddr, drvPtr->port);
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverAccept --
 *
 *      Accept a new socket. It will be in non-blocking mode.
 *
 * Results:
 *      _ACCEPT:       a socket was accepted, poll for data
 *      _ACCEPT_DATA:  a socket was accepted, data present, read immediately
 *                     if in async mode, defer reading to connection thread
 *      _ACCEPT_QUEUE: a socket was accepted, queue immediately
 *      _ACCEPT_ERROR: no socket was accepted
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static NS_DRIVER_ACCEPT_STATUS
DriverAccept(Sock *sockPtr, NS_SOCKET sock)
{
    socklen_t n = (socklen_t)sizeof(struct NS_SOCKADDR_STORAGE);

    NS_NONNULL_ASSERT(sockPtr != NULL);

    return (*sockPtr->drvPtr->acceptProc)((Ns_Sock *) sockPtr,
                                          sock,
                                          (struct sockaddr *) &(sockPtr->sa), &n);
}


/*
 *----------------------------------------------------------------------
 *
 * NsDriverRecv --
 *
 *      Read data from the socket into the given vector of buffers.
 *
 * Results:
 *      Number of bytes read, or -1 on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

ssize_t
NsDriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr)
{
    ssize_t       result;
    const Driver *drvPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    drvPtr = sockPtr->drvPtr;

    if (likely(drvPtr->recvProc != NULL)) {
        result = (*drvPtr->recvProc)((Ns_Sock *) sockPtr, bufs, nbufs, timeoutPtr, 0u);
    } else {
        Ns_Log(Warning, "driver: no recvProc registered for driver %s", drvPtr->threadName);
        result = -1;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsDriverSend --
 *
 *      Write a vector of buffers to the socket via the driver callback.
 *      May not send all of the data.
 *
 * Results:
 *      Number of bytes written or -1 on error.
 *      May return 0 (zero) when socket is not writable.
 *
 * Side effects:
 *      Depends on the driver.
 *
 *----------------------------------------------------------------------
 */

ssize_t
NsDriverSend(Sock *sockPtr, const struct iovec *bufs, int nbufs, unsigned int flags)
{
    ssize_t       sent = -1;
    const Driver *drvPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    drvPtr = sockPtr->drvPtr;

    NS_NONNULL_ASSERT(drvPtr != NULL);

    if (likely(drvPtr->sendProc != NULL)) {
        /*
         * TODO: The Ns_DriverSendProc signature should be modified
         * to omit the timeout argument. Same with recvProc().
         */
        sent = (*drvPtr->sendProc)((Ns_Sock *) sockPtr, bufs, nbufs, NULL, flags);
    } else {
        Ns_Log(Warning, "no sendProc registered for driver %s", drvPtr->threadName);
    }

    return sent;
}


/*
 *----------------------------------------------------------------------
 *
 * NsDriverSendFile --
 *
 *      Write a vector of file buffers to the socket via the driver
 *      callback.
 *
 * Results:
 *      Number of bytes written, -1 on error.
 *      May not send all the data.
 *
 * Side effects:
 *      May block on disk read.
 *
 *----------------------------------------------------------------------
 */

ssize_t
NsDriverSendFile(Sock *sockPtr, Ns_FileVec *bufs, int nbufs, unsigned int flags)
{
    ssize_t       sent = -1;
    const Driver *drvPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);

    drvPtr = sockPtr->drvPtr;

    NS_NONNULL_ASSERT(drvPtr != NULL);

    if (drvPtr->sendFileProc != NULL) {
        /*
         * TODO: The Ns_DriverSendFileProc signature should be modified
         * to omit the timeout argument.
         */
        sent = (*drvPtr->sendFileProc)((Ns_Sock *)sockPtr, bufs, nbufs, NULL, flags);
    } else {
        sent = Ns_SockSendFileBufs((Ns_Sock *)sockPtr, bufs, nbufs, flags);
    }

    return sent;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverKeep --
 *
 *      Can the given socket be kept open in the hopes that another
 *      request will arrive before the keepwait timeout expires?
 *
 * Results:
 *      NS_TRUE if the socket is OK for keepalive, NS_FALSE if this is not possible.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static bool
DriverKeep(Sock *sockPtr)
{
    Ns_DriverKeepProc *keepProc;
    bool              result;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    keepProc = sockPtr->drvPtr->keepProc;
    if (keepProc == NULL)  {
        result = NS_FALSE;
    } else {
        result = (keepProc)((Ns_Sock *) sockPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverClose --
 *
 *      Close the given socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static void
DriverClose(Sock *sockPtr)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);
    (*sockPtr->drvPtr->closeProc)((Ns_Sock *) sockPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * DriverThread --
 *
 *      Main listening socket driver thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted on the configured listen sockets,
 *      placed on the run queue to be serviced, and gracefully
 *      closed when done.  Async sockets have the entire request read
 *      here before queuing as well.
 *
 *----------------------------------------------------------------------
 */

static void
DriverThread(void *arg)
{
    Driver        *drvPtr = (Driver*)arg;
    Ns_Time        now, diff;
    char           charBuffer[1], drain[1024];
    int            pollTimeout, accepted, nrBindaddrs = 0;
    bool           stopping;
    unsigned int   flags;
    Sock          *sockPtr, *closePtr, *nextPtr, *waitPtr, *readPtr;
    PollData       pdata;

    Ns_ThreadSetName("-driver:%s-", drvPtr->threadName);
    Ns_Log(Notice, "starting");

    flags = DRIVER_STARTED;

    {
        Tcl_Obj *bindaddrsObj, **objv;
        int      j = 0, result;

        bindaddrsObj = Tcl_NewStringObj(drvPtr->address, -1);
        Tcl_IncrRefCount(bindaddrsObj);

        result = Tcl_ListObjGetElements(NULL, bindaddrsObj, &nrBindaddrs, &objv);
        /*
         * "result" was ok during startup, it has still to be ok.
         */
        assert(result == TCL_OK);

        if (result == TCL_OK) {
            int i;

            /*
             * Bind all provided addresses.
             */
            for (i = 0; i < nrBindaddrs; i++) {

                drvPtr->listenfd[j] = DriverListen(drvPtr, Tcl_GetString(objv[i]));
                if (drvPtr->listenfd[j] != NS_INVALID_SOCKET) {
                    j ++;
                }
            }
            if (j > 0 && j < nrBindaddrs) {
                Ns_Log(Warning, "could only bind to %d out of %d addresses", j, nrBindaddrs);
            }
        }

        /*
         * "j" refers to the number of successful listen() operations.
         */
        nrBindaddrs = j;
        Tcl_DecrRefCount(bindaddrsObj);
    }

    if (nrBindaddrs > 0) {
        SpoolerQueueStart(drvPtr->spooler.firstPtr, SpoolerThread);
        SpoolerQueueStart(drvPtr->writer.firstPtr, WriterThread);
    } else {
        Ns_Log(Warning, "could no bind any of the following addresses, stopping this driver: %s", drvPtr->address);
        flags |= (DRIVER_FAILED | DRIVER_SHUTDOWN);
    }

    Ns_MutexLock(&drvPtr->lock);
    drvPtr->flags |= flags;
    Ns_CondBroadcast(&drvPtr->cond);
    Ns_MutexUnlock(&drvPtr->lock);

    /*
     * Loop forever until signaled to shut down and all
     * connections are complete and gracefully closed.
     */

    PollCreate(&pdata);
    Ns_GetTime(&now);
    closePtr = waitPtr = readPtr = NULL;
    stopping = ((flags & DRIVER_SHUTDOWN) != 0u);

    if (!stopping) {
        Ns_Log(Notice, "driver: accepting connections");
    }

    while (!stopping) {
        int n;

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, drvPtr->trigger[0], (short)POLLIN, NULL);

        if (likely(waitPtr == NULL)) {

            for (n = 0; n < nrBindaddrs; n++) {
                drvPtr->pidx[n] = PollSet(&pdata, drvPtr->listenfd[n],
                                          (short)POLLIN, NULL);
            }
        }

        /*
         * If there are any closing or read-ahead sockets, set the bits
         * and determine the minimum relative timeout.
         *
         * TODO: the various poll timeouts should probably be configurable.
         */

        if (readPtr == NULL && closePtr == NULL) {
            pollTimeout = 10 * 1000;
        } else {

            for (sockPtr = readPtr; sockPtr != NULL; sockPtr = sockPtr->nextPtr) {
                SockPoll(sockPtr, (short)POLLIN, &pdata);
            }
            for (sockPtr = closePtr; sockPtr != NULL; sockPtr = sockPtr->nextPtr) {
                SockPoll(sockPtr, (short)POLLIN, &pdata);
            }

            if (Ns_DiffTime(&pdata.timeout, &now, &diff) > 0)  {
                /*
                 * The resolution of "pollTimeout" is ms, therefore, we round
                 * up. If we would round down (e.g. 500 microseconds to 0 ms),
                 * the time comparison later would determine that it is too
                 * early.
                 */
                pollTimeout = (int)Ns_TimeToMilliseconds(&diff) + 1;

            } else {
                pollTimeout = 0;
            }
        }

        n = PollWait(&pdata, pollTimeout);

        Ns_Log(DriverDebug, "=== PollWait returned %d, trigger[0] %d", n, PollIn(&pdata, 0));

        if (PollIn(&pdata, 0) && unlikely(ns_recv(drvPtr->trigger[0], charBuffer, 1u, 0) != 1)) {
            const char *errstr = ns_sockstrerror(ns_sockerrno);

            Ns_Fatal("driver: trigger ns_recv() failed: %s", errstr);
        }
        /*
         * Check whether we should re-animate some connection threads,
         * when e.g. the number of current threads dropped below the
         * minimal value.  Perform this test on timeouts (n == 0;
         * just for safety reasons) or on explicit wakeup calls.
         */
        if ((n == 0) || PollIn(&pdata, 0)) {
            NsServer *servPtr = drvPtr->servPtr;

            if (servPtr != NULL) {
                /*
                 * Check if we have to reanimate the current server.
                 */
                NsEnsureRunningConnectionThreads(servPtr, NULL);

            } else {
                Ns_Set *servers = Ns_ConfigCreateSection("ns/servers");
                size_t  j;

                /*
                 * Reanimation check on all servers.
                 */
                for (j = 0u; j < Ns_SetSize(servers); ++j) {
                    const char *server  = Ns_SetKey(servers, j);

                    servPtr = NsGetServer(server);
                    if (servPtr != NULL) {
                        NsEnsureRunningConnectionThreads(servPtr, NULL);
                    }
                }
            }
        }

        /*
         * Update the current time and drain and/or release any
         * closing sockets.
         */
        Ns_GetTime(&now);

        if (closePtr != NULL) {
            sockPtr  = closePtr;
            closePtr = NULL;
            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                    /*
                     * Peer has closed the connection
                     */
                    SockRelease(sockPtr, SOCK_CLOSE, 0);
                } else if (likely(PollIn(&pdata, sockPtr->pidx))) {
                    /*
                     * Got some data
                     */
                    ssize_t received = ns_recv(sockPtr->sock, drain, sizeof(drain), 0);
                    if (received <= 0) {
                        Ns_Log(DriverDebug, "poll closewait pollin; sockrelease SOCK_READERROR (sock %d)",
                               sockPtr->sock);
                        SockRelease(sockPtr, SOCK_READERROR, 0);
                    } else {
                        Push(sockPtr, closePtr);
                    }
                } else if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    /* no PollHup, no PollIn, maybe timeout */
                    Ns_Log(DriverDebug, "poll closewait timeout; sockrelease SOCK_CLOSETIMEOUT (sock %d)",
                           sockPtr->sock);
                    SockRelease(sockPtr, SOCK_CLOSETIMEOUT, 0);
                } else {
                    /* too early, keep waiting */
                    Push(sockPtr, closePtr);
                }
                sockPtr = nextPtr;
            }
        }

        /*
         * Attempt read-ahead of any new connections.
         */

        sockPtr = readPtr;
        readPtr = NULL;

        while (likely(sockPtr != NULL)) {
            nextPtr = sockPtr->nextPtr;

            if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                /*
                 * Peer has closed the connection
                 */
                SockRelease(sockPtr, SOCK_CLOSE, 0);

            } else if (unlikely(!PollIn(&pdata, sockPtr->pidx))
                       && ((sockPtr->reqPtr == NULL) || (sockPtr->reqPtr->leftover == 0u))) {
                /*
                 * Got no data for this sockPtr.
                 */
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                } else {
                    Push(sockPtr, readPtr);
                }

            } else {
                /*
                 * Got some data for this sockPtr.
                 * If enabled, perform read-ahead now.
                 */
                assert(drvPtr == sockPtr->drvPtr);

                if (likely((drvPtr->opts & NS_DRIVER_ASYNC) != 0u)) {
                    SockState s = SockRead(sockPtr, 0, &now);

                    /*
                     * Queue for connection processing if ready.
                     */

                    switch (s) {
                    case SOCK_SPOOL:
                        drvPtr->stats.spooled++;
                        if (SockSpoolerQueue(drvPtr, sockPtr) == 0) {
                            Push(sockPtr, readPtr);
                        }
                        break;

                    case SOCK_MORE:
                        drvPtr->stats.partial++;
                        SockTimeout(sockPtr, &now, &drvPtr->recvwait);
                        Push(sockPtr, readPtr);
                        break;

                    case SOCK_READY:
                        if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                            Push(sockPtr, waitPtr);
                        }
                        break;

                        /*
                         * Already handled or normal cases
                         */
                    case SOCK_ENTITYTOOLARGE:  NS_FALL_THROUGH; /* fall through */
                    case SOCK_BADREQUEST:      NS_FALL_THROUGH; /* fall through */
                    case SOCK_BADHEADER:       NS_FALL_THROUGH; /* fall through */
                    case SOCK_TOOMANYHEADERS:  NS_FALL_THROUGH; /* fall through */
                    case SOCK_CLOSE:
                        SockRelease(sockPtr, s, errno);
                        break;

                        /*
                         * Exceptions
                         */
                    case SOCK_READERROR:    NS_FALL_THROUGH; /* fall through */
                    case SOCK_CLOSETIMEOUT: NS_FALL_THROUGH; /* fall through */
                    case SOCK_ERROR:        NS_FALL_THROUGH; /* fall through */
                    case SOCK_READTIMEOUT:  NS_FALL_THROUGH; /* fall through */
                    case SOCK_SHUTERROR:    NS_FALL_THROUGH; /* fall through */
                    case SOCK_WRITEERROR:   NS_FALL_THROUGH; /* fall through */
                    case SOCK_WRITETIMEOUT:
                        drvPtr->stats.errors++;
                        Ns_Log(Warning,
                               "sockread returned unexpected result %s (err %s); close socket (%d)",
                               GetSockStateName(s),
                               ((errno != 0) ? strerror(errno) : NS_EMPTY_STRING),
                               sockPtr->sock);
                        SockRelease(sockPtr, s, errno);
                        break;
                    }
                } else {
                    /*
                     * Potentially blocking driver, NS_DRIVER_ASYNC is not defined
                     */
                    if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                        drvPtr->stats.errors++;
                        Ns_Log(Notice, "read-ahead has some data, no async sock read ===== diff time %ld",
                               Ns_DiffTime(&sockPtr->timeout, &now, &diff));
                        sockPtr->keep = NS_FALSE;
                        SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                    } else {
                        if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                            Push(sockPtr, waitPtr);
                        }
                    }
                }
            }
            sockPtr = nextPtr;
        }

        /*
         * Attempt to queue any pending connection after reversing the
         * list to ensure oldest connections are tried first.
         */
        if (waitPtr != NULL) {
            sockPtr = NULL;
            while ((nextPtr = waitPtr) != NULL) {
                waitPtr = nextPtr->nextPtr;
                Push(nextPtr, sockPtr);
            }

            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                    Push(sockPtr, waitPtr);
                }
                sockPtr = nextPtr;
            }
        }

        /*
         * If no connections are waiting, attempt to accept more.
         */

        if (waitPtr == NULL) {
            /*
             * If configured, try to accept more than one request, under heavy load
             * this helps to process more requests
             */
            SockState s;
            bool      acceptMore = NS_TRUE;

            accepted = 0;
            while (acceptMore &&
                   accepted < drvPtr->acceptsize
                   && drvPtr->queuesize < drvPtr->maxqueuesize ) {
                bool gotRequests = NS_FALSE;

                /*
                 * Check for input data on all bind addresses. Stop checking,
                 * when one round of checking on all addresses fails.
                 */
                for (n = 0; n < nrBindaddrs; n++) {

                    if (
                        PollIn(&pdata, drvPtr->pidx[n])
                        && (s = SockAccept(drvPtr, pdata.pfds[drvPtr->pidx[n]].fd, &sockPtr, &now)) != SOCK_ERROR) {

                        switch (s) {
                        case SOCK_SPOOL:
                            drvPtr->stats.spooled++;
                            if (SockSpoolerQueue(drvPtr, sockPtr) == 0) {
                                Push(sockPtr, readPtr);
                            }
                            break;

                        case SOCK_MORE:
                            drvPtr->stats.partial++;
                            SockTimeout(sockPtr, &now, &drvPtr->recvwait);
                            Push(sockPtr, readPtr);
                            break;

                        case SOCK_READY:
                            if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                                Push(sockPtr, waitPtr);
                            }
                            break;

                        case SOCK_BADHEADER:      NS_FALL_THROUGH; /* fall through */
                        case SOCK_BADREQUEST:     NS_FALL_THROUGH; /* fall through */
                        case SOCK_CLOSE:          NS_FALL_THROUGH; /* fall through */
                        case SOCK_CLOSETIMEOUT:   NS_FALL_THROUGH; /* fall through */
                        case SOCK_ENTITYTOOLARGE: NS_FALL_THROUGH; /* fall through */
                        case SOCK_ERROR:          NS_FALL_THROUGH; /* fall through */
                        case SOCK_READERROR:      NS_FALL_THROUGH; /* fall through */
                        case SOCK_READTIMEOUT:    NS_FALL_THROUGH; /* fall through */
                        case SOCK_SHUTERROR:      NS_FALL_THROUGH; /* fall through */
                        case SOCK_TOOMANYHEADERS: NS_FALL_THROUGH; /* fall through */
                        case SOCK_WRITEERROR:     NS_FALL_THROUGH; /* fall through */
                        case SOCK_WRITETIMEOUT:
                            Ns_Fatal("driver: SockAccept returned: %s", GetSockStateName(s));
                        }
                        accepted++;
                        gotRequests = NS_TRUE;
#ifdef __APPLE__
                        /*
                         * On Darwin, the first accept() succeeds typically, but it is
                         * useless to try, since this leads always to an EAGAIN
                         */
                        acceptMore = NS_FALSE;
                        break;
#endif
                    }
                }
                if (!gotRequests) {
                    acceptMore = NS_FALSE;
                }
            }
            if (accepted > 1) {
                Ns_Log(Notice, "... sockAccept accepted %d connections", accepted);
            }
        }

        /*
         * Check for shut down and get the list of any closing or
         * keep-alive sockets.
         */

        Ns_MutexLock(&drvPtr->lock);
        sockPtr          = drvPtr->closePtr;
        drvPtr->closePtr = NULL;
        flags            = drvPtr->flags;
        Ns_MutexUnlock(&drvPtr->lock);

        stopping = ((flags & DRIVER_SHUTDOWN) != 0u);

        /*
         * Update the timeout for each closing socket and add to the
         * close list if some data has been read from the socket
         * (i.e., it's not a closing keep-alive connection).
         */
        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            if (sockPtr->keep) {

                assert(drvPtr == sockPtr->drvPtr);

                Ns_Log(DriverDebug, "setting keepwait %ld.%6ld for socket %d",
                       drvPtr->keepwait.sec, drvPtr->keepwait.usec,
                       sockPtr->sock);

                SockTimeout(sockPtr, &now, &drvPtr->keepwait);
                Push(sockPtr, readPtr);
            } else {

                /*
                 * Purely packet oriented drivers set on close the fd to
                 * NS_INVALID_SOCKET. Since we cannot "shutdown" an UDP-socket
                 * for writing, we bypass this call.
                 */
                assert(drvPtr == sockPtr->drvPtr);

                if (sockPtr->sock == NS_INVALID_SOCKET) {
                    SockRelease(sockPtr, SOCK_CLOSE, errno);

                    Ns_Log(DriverDebug, "DRIVER SockRelease: errno %d drvPtr->closewait %ld.%6ld",
                           errno, drvPtr->closewait.sec, drvPtr->closewait.usec);

                } else if (shutdown(sockPtr->sock, SHUT_WR) != 0) {
                    SockRelease(sockPtr, SOCK_SHUTERROR, errno);

                } else {
                    Ns_Log(DriverDebug, "setting closewait %ld.%6ld for socket %d",
                           drvPtr->closewait.sec,  drvPtr->closewait.usec, sockPtr->sock);
                    SockTimeout(sockPtr, &now, &drvPtr->closewait);
                    Push(sockPtr, closePtr);
                }
            }
            sockPtr = nextPtr;
        }

        /*
         * Close the active drivers if shutdown is pending.
         */

        if (stopping) {
            for (n = 0; n < nrBindaddrs; n++) {
                ns_sockclose(drvPtr->listenfd[n]);
                drvPtr->listenfd[n] = NS_INVALID_SOCKET;
            }
        }
    }

    PollFree(&pdata);

    Ns_Log(Notice, "exiting");
    Ns_MutexLock(&drvPtr->lock);
    drvPtr->flags |= DRIVER_STOPPED;
    Ns_CondBroadcast(&drvPtr->cond);
    Ns_MutexUnlock(&drvPtr->lock);

}

static void
PollCreate(PollData *pdata)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    memset(pdata, 0, sizeof(PollData));
}

static void
PollFree(PollData *pdata)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    ns_free(pdata->pfds);
    memset(pdata, 0, sizeof(PollData));
}

static void
PollReset(PollData *pdata)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    pdata->nfds = 0u;
    pdata->timeout.sec = TIME_T_MAX;
    pdata->timeout.usec = 0;
}

static NS_POLL_NFDS_TYPE
PollSet(PollData *pdata, NS_SOCKET sock, short type, const Ns_Time *timeoutPtr)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    /*
     * Grow the pfds array if necessary.
     */

    if (unlikely(pdata->nfds >= pdata->maxfds)) {
        pdata->maxfds += 100u;
        pdata->pfds = ns_realloc(pdata->pfds, pdata->maxfds * sizeof(struct pollfd));
    }

    /*
     * Set the next pollfd struct with this socket.
     */

    pdata->pfds[pdata->nfds].fd = sock;
    pdata->pfds[pdata->nfds].events = type;
    pdata->pfds[pdata->nfds].revents = 0;

    /*
     * Check for new minimum timeout.
     */

    if (timeoutPtr != NULL && Ns_DiffTime(timeoutPtr, &pdata->timeout, NULL) < 0) {
        pdata->timeout = *timeoutPtr;
    }

    return pdata->nfds++;
}

static int
PollWait(const PollData *pdata, int timeout)
{
    int n;

    NS_NONNULL_ASSERT(pdata != NULL);

    do {
        n = ns_poll(pdata->pfds, pdata->nfds, timeout);
    } while (n < 0  && errno == NS_EINTR);

    if (n < 0) {
        Ns_Fatal("PollWait: ns_poll() failed: %s", ns_sockstrerror(ns_sockerrno));
    }
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * RequestNew
 *
 *      Prepares for reading from the socket, allocates a "Request"
 *      struct for the given socket. It might be reused from the pool
 *      or freshly allocated. Counterpart of RequestFree().
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static void
RequestNew(Sock *sockPtr)
{
    Request *reqPtr;
    bool     reuseRequest = NS_TRUE;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    /*
     * Try to get a request from the pool of allocated Requests.
     */
    Ns_MutexLock(&reqLock);
    reqPtr = firstReqPtr;
    if (likely(reqPtr != NULL)) {
        firstReqPtr = reqPtr->nextPtr;
    } else {
        reuseRequest = NS_FALSE;
    }
    Ns_MutexUnlock(&reqLock);

    if (reuseRequest) {
        Ns_Log(DriverDebug, "RequestNew reuses a Request");
    }

    /*
     * In case we failed, allocate a new Request.
     */
    if (reqPtr == NULL) {
        Ns_Log(DriverDebug, "RequestNew gets a fresh Request");
        reqPtr = ns_calloc(1u, sizeof(Request));
        Tcl_DStringInit(&reqPtr->buffer);
        reqPtr->headers = Ns_SetCreate(NULL);
    }
    sockPtr->reqPtr = reqPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * RequestFree --
 *
 *      Free/clean a socket request structure.  This routine is called
 *      at the end of connection processing or on a socket which
 *      times out during async read-ahead. Counterpart of RequestNew().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
RequestFree(Sock *sockPtr)
{
    Request *reqPtr;
    bool     keep;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    reqPtr = sockPtr->reqPtr;
    assert(reqPtr != NULL);

    Ns_Log(DriverDebug, "=== RequestFree cleans %p (avail %" PRIuz " keep %d length %" PRIuz " contentLength %" PRIuz ")",
           (void *)reqPtr, reqPtr->avail, sockPtr->keep, reqPtr->length, reqPtr->contentLength);

    keep = (sockPtr->keep) && (reqPtr->avail > reqPtr->contentLength);
    if (keep) {
        size_t      leftover = reqPtr->avail - reqPtr->contentLength;
        const char *offset   = reqPtr->buffer.string + ((size_t)reqPtr->buffer.length - leftover);

        Ns_Log(DriverDebug, "setting leftover to %" PRIuz " bytes", leftover);
        /*
         * Here it is safe to move the data in the buffer, although the
         * reqPtr->content might point to it, since we re-init the content. In
         * case the terminating null character was written to the end of the
         * previous buffer, we have to restore the first character.
         */
        memmove(reqPtr->buffer.string, offset, leftover);
        if (reqPtr->savedChar != '\0') {
            reqPtr->buffer.string[0] = reqPtr->savedChar;
        }
        Tcl_DStringSetLength(&reqPtr->buffer, (int)leftover);
        LogBuffer(DriverDebug, "KEEP BUFFER", reqPtr->buffer.string, leftover);
        reqPtr->leftover = leftover;
    } else {
        /*
         * Clean large buffers in order to avoid memory growth on huge
         * uploads (when maxupload is huge)
         */
        /*fprintf(stderr, "=== reuse buffer size %d avail %d dynamic %d\n",
                reqPtr->buffer.length, reqPtr->buffer.spaceAvl,
                reqPtr->buffer.string == reqPtr->buffer.staticSpace);*/
        if (Tcl_DStringLength(&reqPtr->buffer) > 65536) {
            Tcl_DStringFree(&reqPtr->buffer);
        } else {
            /*
             * Reuse buffer, but set length to 0.
             */
            Tcl_DStringSetLength(&reqPtr->buffer, 0);
        }
        reqPtr->leftover = 0u;
    }

    reqPtr->next           = NULL;
    reqPtr->content        = NULL;
    reqPtr->length         = 0u;
    reqPtr->contentLength  = 0u;

    reqPtr->expectedLength = 0u;
    reqPtr->chunkStartOff  = 0u;
    reqPtr->chunkWriteOff  = 0u;

    reqPtr->roff           = 0u;
    reqPtr->woff           = 0u;
    reqPtr->coff           = 0u;
    reqPtr->avail          = 0u;
    reqPtr->savedChar      = '\0';

    Ns_SetTrunc(reqPtr->headers, 0u);

    if (reqPtr->auth != NULL) {
        Ns_SetFree(reqPtr->auth);
        reqPtr->auth = NULL;
    }

    if (reqPtr->request.line != NULL) {
        Ns_Log(DriverDebug, "RequestFree calls Ns_ResetRequest on %p", (void*)&reqPtr->request);
        Ns_ResetRequest(&reqPtr->request);
    } else {
        Ns_Log(DriverDebug, "RequestFree does not call Ns_ResetRequest on %p", (void*)&reqPtr->request);
    }

    if (!keep) {
        /*
         * Push the reqPtr to the pool for reuse in other connections.
         */
        sockPtr->reqPtr = NULL;

        Ns_MutexLock(&reqLock);
        reqPtr->nextPtr = firstReqPtr;
        firstReqPtr = reqPtr;
        Ns_MutexUnlock(&reqLock);
    } else {
        /*
         * Keep the partly cleaned up reqPtr associated with the connection.
         */
        Ns_Log(DriverDebug, "=== KEEP request structure in sockPtr (don't push into the pool)");
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SockQueue --
 *
 *      Puts socket into connection queue
 *
 * Results:
 *      NS_OK if queued,
 *      NS_ERROR if socket closed because of error
 *      NS_TIMEOUT if queue is full
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
SockQueue(Sock *sockPtr, const Ns_Time *timePtr)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    /*
     *  Verify the conditions. Request struct must exist already.
     */
    assert(sockPtr->reqPtr != NULL);

    SockSetServer(sockPtr);
    assert(sockPtr->servPtr != NULL);

    /*
     *  Actual queueing, if not ready spool to the waiting list.
     */
    if (!NsQueueConn(sockPtr, timePtr)) {
        result = NS_TIMEOUT;
    } else {
        result = NS_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SockPoll --
 *
 *      Arrange for given Sock to be monitored.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sock fd will be monitored for readability on next spin of
 *      DriverThread.
 *
 *----------------------------------------------------------------------
 */

static void
SockPoll(Sock *sockPtr, short type, PollData *pdata)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(pdata != NULL);

    sockPtr->pidx = PollSet(pdata, sockPtr->sock, type, &sockPtr->timeout);
}

/*
 *----------------------------------------------------------------------
 *
 * SockTimeout --
 *
 *      Update socket with timeout
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Socket timeout will have nowPtr + timeout value
 *
 *----------------------------------------------------------------------
 */

static void
SockTimeout(Sock *sockPtr, const Ns_Time *nowPtr, const Ns_Time *timeout)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);
    sockPtr->timeout = *nowPtr;
    Ns_IncrTime(&sockPtr->timeout, timeout->sec, timeout->usec);
}



/*
 *----------------------------------------------------------------------
 *
 * SockAccept --
 *
 *      Accept and initialize a new Sock in sockPtrPtr.
 *
 * Results:
 *      SOCK_READY, SOCK_MORE, SOCK_SPOOL,
 *      SOCK_ERROR + NULL sockPtr.
 *
 * Side effects:
 *      Read-ahead may be attempted on new socket.
 *
 *----------------------------------------------------------------------
 */

static SockState
SockAccept(Driver *drvPtr, NS_SOCKET sock, Sock **sockPtrPtr, const Ns_Time *nowPtr)
{
    Sock    *sockPtr;
    SockState sockStatus;
    NS_DRIVER_ACCEPT_STATUS status;

    NS_NONNULL_ASSERT(drvPtr != NULL);

    sockPtr = SockNew(drvPtr);

    /*
     * Accept the new connection.
     */

    status = DriverAccept(sockPtr, sock);

    if (unlikely(status == NS_DRIVER_ACCEPT_ERROR)) {
        sockStatus = SOCK_ERROR;

        /*
         * We reach the place frequently, especially on Linux, when we try to
         * accept multiple connection in one sweep. Usually, the errno is
         * EAGAIN.
         */

        Ns_MutexLock(&drvPtr->lock);
        sockPtr->nextPtr = drvPtr->sockPtr;
        drvPtr->sockPtr = sockPtr;
        Ns_MutexUnlock(&drvPtr->lock);
        sockPtr = NULL;

    } else {
        sockPtr->acceptTime = *nowPtr;
        drvPtr->queuesize++;

        if (status == NS_DRIVER_ACCEPT_DATA) {

            /*
             * If there is already data present then read it without
             * polling if we're in async mode.
             */

            if ((drvPtr->opts & NS_DRIVER_ASYNC) != 0u) {
                sockStatus = SockRead(sockPtr, 0, nowPtr);
                if ((int)sockStatus < 0) {
                    Ns_Log(DriverDebug, "SockRead returned error %s",
                           GetSockStateName(sockStatus));

                    SockRelease(sockPtr, sockStatus, errno);
                    sockStatus = SOCK_ERROR;
                    sockPtr = NULL;
                }
            } else {

                /*
                 * Queue this socket without reading, NsGetRequest() in the
                 * connection thread will perform actual reading of the
                 * request.
                 */
                sockStatus = SOCK_READY;
            }
        } else if (status == NS_DRIVER_ACCEPT_QUEUE) {

            /*
             *  We need to call RequestNew() to make sure socket has request
             *  structure allocated, otherwise NsGetRequest() will call
             *  SockRead() which is not what this driver wants.
             */
            if (sockPtr->reqPtr == NULL) {
                RequestNew(sockPtr);
            }
            sockStatus = SOCK_READY;
        } else {
            sockStatus = SOCK_MORE;
        }
    }

    *sockPtrPtr = sockPtr;

    return sockStatus;
}


/*
 *----------------------------------------------------------------------
 *
 * SockNew --
 *
 *      Allocate and/or initialize a Sock structure. Counterpart of
 *      SockRelease().
 *
 * Results:
 *      SockPtr
 *
 * Side effects:
 *      Potentially new memory is allocated.
 *
 *----------------------------------------------------------------------
 */

static Sock *
SockNew(Driver *drvPtr)
{
    Sock *sockPtr;

    NS_NONNULL_ASSERT(drvPtr != NULL);

    Ns_MutexLock(&drvPtr->lock);
    sockPtr = drvPtr->sockPtr;
    if (likely(sockPtr != NULL)) {
        drvPtr->sockPtr = sockPtr->nextPtr;
        sockPtr->keep   = NS_FALSE;
    }
    Ns_MutexUnlock(&drvPtr->lock);

    if (sockPtr == NULL) {
        size_t sockSize = sizeof(Sock) + (nsconf.nextSlsId * sizeof(Ns_Callback *));
        sockPtr = ns_calloc(1u, sockSize);
        sockPtr->drvPtr = drvPtr;
    } else {
        sockPtr->tfd    = 0;
        sockPtr->taddr  = NULL;
        sockPtr->flags  = 0u;
        sockPtr->arg    = NULL;
        sockPtr->recvSockState = NS_SOCK_NONE;
    }
    return sockPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * SockRelease --
 *
 *      Close a socket and release the connection structure for
 *      re-use.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
SockRelease(Sock *sockPtr, SockState reason, int err)
{
    Driver *drvPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    Ns_Log(DriverDebug, "SockRelease reason %s err %d (sock %d)",
           GetSockStateName(reason), err, sockPtr->sock);

    drvPtr = sockPtr->drvPtr;
    assert(drvPtr != NULL);

    SockError(sockPtr, reason, err);

    if (sockPtr->sock != NS_INVALID_SOCKET) {
        SockClose(sockPtr, (int)NS_FALSE);
    } else {
        Ns_Log(DriverDebug, "SockRelease bypasses SockClose, since we have an invalid socket");
    }
    NsSlsCleanup(sockPtr);

    drvPtr->queuesize--;

    if (sockPtr->reqPtr != NULL) {
        Ns_Log(DriverDebug, "SockRelease calls RequestFree");
        RequestFree(sockPtr);
    }

    Ns_MutexLock(&drvPtr->lock);
    sockPtr->nextPtr = drvPtr->sockPtr;
    drvPtr->sockPtr  = sockPtr;
    Ns_MutexUnlock(&drvPtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * SockError --
 *
 *      Log error message for given socket
 *      re-use.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
SockError(Sock *sockPtr, SockState reason, int err)
{
    const char *errMsg = NULL;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    switch (reason) {
    case SOCK_READY:
    case SOCK_SPOOL:
    case SOCK_MORE:
    case SOCK_CLOSE:
    case SOCK_CLOSETIMEOUT:
        /* This is normal, never log. */
        break;

    case SOCK_READTIMEOUT:
        /*
         * For this case, whether this is acceptable or not
         * depends upon whether this sock was a keep-alive
         * that we were allowing to 'linger'.
         */
        if (!sockPtr->keep) {
            errMsg = "Timeout during read";
        }
        break;

    case SOCK_WRITETIMEOUT:
        errMsg = "Timeout during write";
        break;

    case SOCK_READERROR:
        errMsg = "Unable to read request";
        break;

    case SOCK_WRITEERROR:
        errMsg = "Unable to write request";
        break;

    case SOCK_SHUTERROR:
        errMsg = "Unable to shutdown socket";
        break;

    case SOCK_BADREQUEST:
        errMsg = "Bad Request";
        SockSendResponse(sockPtr, 400, errMsg);
        break;

    case SOCK_TOOMANYHEADERS:
        errMsg = "Too Many Request Headers";
        SockSendResponse(sockPtr, 414, errMsg);
        break;

    case SOCK_BADHEADER:
        errMsg = "Invalid Request Header";
        SockSendResponse(sockPtr, 400, errMsg);
        break;

    case SOCK_ENTITYTOOLARGE:
        errMsg = "Request Entity Too Large";
        SockSendResponse(sockPtr, 413, errMsg);
        break;

    case SOCK_ERROR:
        errMsg = "Unknown Error";
        SockSendResponse(sockPtr, 400, errMsg);
        break;
    }

    if (errMsg != NULL) {
        char ipString[NS_IPADDR_SIZE];

        Ns_Log(DriverDebug, "SockError: %s (%d: %s), sock: %d, peer: [%s]:%d, request: %.99s",
               errMsg,
               err, (err != 0) ? strerror(err) : NS_EMPTY_STRING,
               sockPtr->sock,
               ns_inet_ntop((struct sockaddr *)&(sockPtr->sa), ipString, sizeof(ipString)),
               Ns_SockaddrGetPort((struct sockaddr *)&(sockPtr->sa)),
               (sockPtr->reqPtr != NULL) ? sockPtr->reqPtr->buffer.string : NS_EMPTY_STRING);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SockSendResponse --
 *
 *      Send an HTTP response directly to the client using the
 *      driver callback.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May not sent the complete response to the client
 *      if encountering non-writable connection socket.
 *
 *----------------------------------------------------------------------
 */

static void
SockSendResponse(Sock *sockPtr, int code, const char *errMsg)
{
    struct iovec iov[3];
    char         header[32];
    ssize_t      sent, tosend;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(errMsg != NULL);

    snprintf(header, sizeof(header), "HTTP/1.0 %d ", code);
    iov[0].iov_base = header;
    iov[0].iov_len  = strlen(header);
    iov[1].iov_base = (void *)errMsg;
    iov[1].iov_len  = strlen(errMsg);
    iov[2].iov_base = (void *)"\r\n\r\n";
    iov[2].iov_len  = 4u;
    tosend = (ssize_t)(iov[0].iov_len + iov[1].iov_len + iov[2].iov_len);
    sent = NsDriverSend(sockPtr, iov, 3, 0u);
    if (sent < tosend) {
        Ns_Log(Warning, "Driver: partial write while sending response;"
               " %" PRIdz " < %" PRIdz, sent, tosend);
    }

    /*
     * In case we have a request structure, complain the system log about
     * the bad request.
     */
    if (sockPtr->reqPtr != NULL) {
        Request     *reqPtr = sockPtr->reqPtr;
        const char  *requestLine = (reqPtr->request.line != NULL) ? reqPtr->request.line : NS_EMPTY_STRING;

        (void)ns_inet_ntop((struct sockaddr *)&(sockPtr->sa), sockPtr->reqPtr->peer, NS_IPADDR_SIZE);

        /*
         * Check, if bad request looks like a TLS handshake. If yes, there is
         * no need to print out the received buffer.
         */
        if (requestLine[0] == (char)0x16 && requestLine[1] >= 3 && requestLine[2] == 1) {
            Ns_Log(Warning, "invalid request %d (%s) from peer %s: received TLS handshake on a non-TLS connection",
                   code, errMsg, reqPtr->peer);
        } else {
            Tcl_DString  dsReqLine;

            Tcl_DStringInit(&dsReqLine);
            Ns_Log(Warning, "invalid request: %d (%s) from peer %s request '%s' offsets: read %" PRIuz
                   " write %" PRIuz " content %" PRIuz " avail %" PRIuz,
                   code, errMsg,
                   reqPtr->peer,
                   Ns_DStringAppendPrintable(&dsReqLine, NS_FALSE, requestLine, strlen(requestLine)),
                   reqPtr->roff,
                   reqPtr->woff,
                   reqPtr->coff,
                   reqPtr->avail);
            Tcl_DStringFree(&dsReqLine);

            LogBuffer(Warning, "REQ BUFFER", reqPtr->buffer.string, (size_t)reqPtr->buffer.length);
        }
    } else {
        Ns_Log(Warning, "invalid request: %d (%s) - no request information available",
               code, errMsg);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * SockTrigger --
 *
 *      Wakeup DriversThread from blocking ns_poll().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      DriversThread will wake up.
 *
 *----------------------------------------------------------------------
 */

static void
SockTrigger(NS_SOCKET sock)
{
    if (send(sock, NS_EMPTY_STRING, 1, 0) != 1) {
        const char *errstr = ns_sockstrerror(ns_sockerrno);

        Ns_Log(Error, "driver: trigger send() failed: %s", errstr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SockClose --
 *
 *      Closes connection socket, does all cleanups. The input parameter
 *      "keep" might be NS_TRUE/NS_FALSE or -1 if undecided.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static void
SockClose(Sock *sockPtr, int keep)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);

    if (keep != 0) {
        bool driverKeep = DriverKeep(sockPtr);
        keep = (int)driverKeep;
    }
    if (keep == (int)NS_FALSE) {
        DriverClose(sockPtr);
    }
    Ns_MutexLock(&sockPtr->drvPtr->lock);
    sockPtr->keep = (bool)keep;
    Ns_MutexUnlock(&sockPtr->drvPtr->lock);

    /*
     * Unconditionally remove temporary file, connection thread
     * should take care about very large uploads.
     */

    if (sockPtr->tfile != NULL) {
        unlink(sockPtr->tfile);
        ns_free(sockPtr->tfile);
        sockPtr->tfile = NULL;

        if (sockPtr->tfd > 0) {
            /*
             * Close and reset fd. The fd should be > 0 unless we are in error
             * conditions.
             */
            (void) ns_close(sockPtr->tfd);
        }
        sockPtr->tfd = 0;

    } else if (sockPtr->tfd > 0) {
        /*
         * This must be a fd allocated via Ns_GetTemp();
         */
        Ns_ReleaseTemp(sockPtr->tfd);
        sockPtr->tfd = 0;
    }

#ifndef _WIN32
    /*
     * Un-map temp file used for spooled content.
     */
    if (sockPtr->taddr != NULL) {
        munmap(sockPtr->taddr, (size_t)sockPtr->tsize);
        sockPtr->taddr = NULL;
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ChunkedDecode --
 *
 *      Reads the content form the incoming request buffer and tries
 *      to decode chunked encoding parts. The function can be called
 *      repeatedly and with incomplete input and overwrites the buffer
 *      with the decoded data optionally. The decoded data is always
 *      shorter than the encoded one.
 *
 * Results:
 *      NS_TRUE when chunk was complete, NS_FALSE otherwise
 *
 * Side effects:
 *      updates the buffer if update is true (and adjusts reqPtr->chunkWriteOff)
 *      updates always reqPtr->chunkStartOff to allow incremental operations
 *
 *----------------------------------------------------------------------
 */
static bool
ChunkedDecode(Request *reqPtr, bool update)
{
    const Tcl_DString *bufPtr;
    const char        *end, *chunkStart;
    bool              success = NS_TRUE;

    NS_NONNULL_ASSERT(reqPtr != NULL);

    bufPtr = &reqPtr->buffer;
    end = bufPtr->string + bufPtr->length;
    chunkStart = bufPtr->string + reqPtr->chunkStartOff;

    while (reqPtr->chunkStartOff <  (size_t)bufPtr->length) {
        char   *p = strstr(chunkStart, "\r\n");
        size_t  chunk_length;

        if (p == NULL) {
            Ns_Log(DriverDebug, "ChunkedDecode: chunk did not find end-of-line");
            success = NS_FALSE;
            break;
        }

        *p = '\0';
        chunk_length = (size_t)strtol(chunkStart, NULL, 16);
        *p = '\r';

        if (p + 2 + chunk_length > end) {
            Ns_Log(DriverDebug, "ChunkedDecode: chunk length past end of buffer");
            success = NS_FALSE;
            break;
        }
        if (update) {
            char *writeBuffer = bufPtr->string + reqPtr->chunkWriteOff;

            memmove(writeBuffer, p + 2, chunk_length);
            reqPtr->chunkWriteOff += chunk_length;
            *(writeBuffer + chunk_length) = '\0';
        }
        reqPtr->chunkStartOff += (size_t)(p - chunkStart) + 4u + chunk_length;
        chunkStart = bufPtr->string + reqPtr->chunkStartOff;
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * SockRead --
 *
 *      Read content from the given Sock, processing the input as
 *      necessary.  This is the core callback routine designed to
 *      either be called repeatedly within the DriverThread during
 *      an async read-ahead or in a blocking loop in NsGetRequest()
 *      at the start of connection processing.
 *
 * Results:
 *      SOCK_READY: Request is ready for processing.
 *      SOCK_MORE:  More input is required.
 *      SOCK_ERROR: Client drop or timeout.
 *      SOCK_SPOOL: Pass input handling to spooler
 *      SOCK_CLOSE: peer closed connection
 *      SOCK_BADREQUEST
 *      SOCK_BADHEADER
 *      SOCK_TOOMANYHEADERS
 *
 * Side effects:
 *      The Request structure will be built up for use by the
 *      connection thread.  Also, before returning SOCK_READY,
 *      the next byte to read mark and bytes available are set
 *      to the beginning of the content, just beyond the headers.
 *
 *      Contents may be spooled into temp file and mmap-ed
 *
 *----------------------------------------------------------------------
 */

static SockState
SockRead(Sock *sockPtr, int spooler, const Ns_Time *timePtr)
{
    const Driver       *drvPtr;
    Request      *reqPtr;
    Tcl_DString  *bufPtr;
    struct iovec  buf;
    char          tbuf[16384];
    size_t        buflen, nread;
    ssize_t       n;
    SockState     resultState;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    drvPtr = sockPtr->drvPtr;

    tbuf[0] = '\0';

    /*
     * In case of "keepwait", the accept time is not meaningful and
     * reset to 0. In such cases, update "acceptTime" to the actual
     * begin of a request. This part is intended for async drivers.
     */
    if (sockPtr->acceptTime.sec == 0) {
        assert(timePtr != NULL);
        sockPtr->acceptTime = *timePtr;
    }

    /*
     * Initialize request structure if needed.
     */
    if (sockPtr->reqPtr == NULL) {
        RequestNew(sockPtr);
    }

    /*
     * On the first read, attempt to read-ahead "bufsize" bytes.
     * Otherwise, read only the number of bytes left in the
     * content.
     */

    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;
    if (reqPtr->length == 0u) {
        nread = drvPtr->bufsize;
    } else {
        nread = reqPtr->length - reqPtr->avail;
    }

    /*
     * Grow the buffer to include space for the next bytes.
     */

    buflen = (size_t)bufPtr->length;
    n = (ssize_t)(buflen + nread);
    if (unlikely(n > drvPtr->maxinput)) {
        n = (ssize_t)drvPtr->maxinput;
        nread = (size_t)n - buflen;
        if (nread == 0u) {
            Ns_Log(DriverDebug, "SockRead: maxinput reached %" TCL_LL_MODIFIER "d",
                   drvPtr->maxinput);
            return SOCK_ERROR;
        }
    }

    /*
     * Use temp file for content larger than "readahead" bytes.
     */
#ifndef _WIN32
    if (reqPtr->coff > 0u                     /* We are in the content part (after the header) */
        && !reqPtr->chunkStartOff             /* Never spool chunked encoded data since we decode in memory */
        && reqPtr->length > (size_t)drvPtr->readahead /* We need more data */
        && sockPtr->tfd <= 0                  /* We have no spool fd */
        ) {
        const DrvSpooler *spPtr = &drvPtr->spooler;

        Ns_Log(DriverDebug, "SockRead: require tmp file for content spooling (length %" PRIuz" > readahead "
               "%" TCL_LL_MODIFIER "d)",
               reqPtr->length, drvPtr->readahead);

        /*
         * In driver mode send this Sock to the spooler thread if
         * it is running
         */

        if (spooler == 0 && spPtr->threads > 0) {
            return SOCK_SPOOL;
        }

        /*
         * If "maxupload" is specified and content size exceeds the configured
         * values, spool uploads into normal temp file (not deleted).  We do
         * not want to map such large files into memory.
         */
        if (drvPtr->maxupload > 0
            && reqPtr->length > (size_t)drvPtr->maxupload
            ) {
            size_t tfileLength = strlen(drvPtr->uploadpath) + 16u;

            sockPtr->tfile = ns_malloc(tfileLength);
            snprintf(sockPtr->tfile, tfileLength, "%s/%d.XXXXXX", drvPtr->uploadpath, sockPtr->sock);
            sockPtr->tfd = ns_mkstemp(sockPtr->tfile);
            if (sockPtr->tfd == NS_INVALID_FD) {
                Ns_Log(Error, "SockRead: cannot create spool file with template '%s': %s",
                       sockPtr->tfile, strerror(errno));
            }
        } else {
            /*
             * Get a temporary fd. These FDs are used for mmapping.
             */

            sockPtr->tfd = Ns_GetTemp();
        }

        if (unlikely(sockPtr->tfd == NS_INVALID_FD)) {
            Ns_Log(DriverDebug, "SockRead: spool fd invalid");
            return SOCK_ERROR;
        }

        n = (ssize_t)((size_t)bufPtr->length - reqPtr->coff);
        assert(n >= 0);
        if (ns_write(sockPtr->tfd, bufPtr->string + reqPtr->coff, (size_t)n) != n) {
            return SOCK_WRITEERROR;
        }
        Tcl_DStringSetLength(bufPtr, 0);
    }
#endif
    if (sockPtr->tfd > 0) {
        buf.iov_base = tbuf;
        buf.iov_len = MIN(nread, sizeof(tbuf));
    } else {
        Tcl_DStringSetLength(bufPtr, (int)(buflen + nread));
        buf.iov_base = bufPtr->string + reqPtr->woff;
        buf.iov_len = nread;
    }

    if (reqPtr->leftover > 0u) {
        /*
         * There is some leftover in the buffer, don't read but take the
         * leftover instead as input.
         */
        n = (ssize_t)reqPtr->leftover;
        reqPtr->leftover = 0u;
        buflen = 0u;
        Ns_Log(DriverDebug, "SockRead receive from leftover %" PRIdz " bytes", n);
    } else {
        /*
         * Receive actually some data from the driver.
         */
        n = NsDriverRecv(sockPtr, &buf, 1, NULL);
        Ns_Log(DriverDebug, "SockRead receive from network %" PRIdz " bytes sockState %.2x",
               n, (int)sockPtr->recvSockState);
    }

    {
        Ns_SockState nsSockState = sockPtr->recvSockState;
    /*
     * The nsSockState has one of the following values, when provided:
     *
     *      NS_SOCK_READ, NS_SOCK_DONE, NS_SOCK_AGAIN, NS_SOCK_EXCEPTION,
     *      NS_SOCK_TIMEOUT
     */
        switch (nsSockState) {
        case NS_SOCK_TIMEOUT:  NS_FALL_THROUGH; /* fall through */
        case NS_SOCK_EXCEPTION:
            return SOCK_READERROR;
        case NS_SOCK_AGAIN:
            Tcl_DStringSetLength(bufPtr, (int)buflen);
            return SOCK_MORE;
        case NS_SOCK_DONE:
            return SOCK_CLOSE;
        case NS_SOCK_READ:
            break;

        case NS_SOCK_CANCEL:  NS_FALL_THROUGH; /* fall through */
        case NS_SOCK_EXIT:    NS_FALL_THROUGH; /* fall through */
        case NS_SOCK_INIT:    NS_FALL_THROUGH; /* fall through */
        case NS_SOCK_WRITE:
            Ns_Log(Warning, "SockRead received unexpected state %.2x from driver", nsSockState);
            return SOCK_READERROR;

        case NS_SOCK_NONE:
            /*
             * Old style state management based on "n" and "errno", which is
             * more fragile. We keep there for old-style drivers.
             */
            if (n < 0) {
                Tcl_DStringSetLength(bufPtr, (int)buflen);
                /*
                 * The driver returns -1 when the peer closed the connection, but
                 * clears the errno such we can distinguish from error conditions.
                 */
                if (errno == 0) {
                    return SOCK_CLOSE;
                }
                return SOCK_READERROR;
            }

            if (n == 0) {
                Tcl_DStringSetLength(bufPtr, (int)buflen);
                return SOCK_MORE;
            }
            break;

        }
    }

    if (sockPtr->tfd > 0) {
        if (ns_write(sockPtr->tfd, tbuf, (size_t)n) != n) {
            return SOCK_WRITEERROR;
        }
    } else {
        Tcl_DStringSetLength(bufPtr, (int)(buflen + (size_t)n));
    }

    reqPtr->woff  += (size_t)n;
    reqPtr->avail += (size_t)n;

    /*
     * This driver needs raw buffer, it is binary or non-HTTP request
     */

    if ((drvPtr->opts & NS_DRIVER_NOPARSE) != 0u) {
        return SOCK_READY;
    }

    resultState = SockParse(sockPtr);

    return resultState;
}


/*----------------------------------------------------------------------
 *
 * LogBuffer --
 *
 *      Debug function to output buffer content when the provided severity is
 *      enabled. The function prints just visible characters and space as is
 *      and prints the hex code otherwise.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Writes to error.log
 *
 *----------------------------------------------------------------------
 */
static void
LogBuffer(Ns_LogSeverity severity, const char *msg, const char *buffer, size_t len)
{
    Tcl_DString ds;

    NS_NONNULL_ASSERT(msg != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (Ns_LogSeverityEnabled(severity)) {

        Tcl_DStringInit(&ds);
        Tcl_DStringAppend(&ds, msg, -1);
        Tcl_DStringAppend(&ds, ": ", 2);
        (void)Ns_DStringAppendPrintable(&ds, NS_FALSE, buffer, len);

        Ns_Log(severity, "%s", ds.string);
        Tcl_DStringFree(&ds);
    }
}


/*----------------------------------------------------------------------
 *
 * EndOfHeader --
 *
 *      Function to be called (once), when end of header is reached. At this
 *      time, all request header lines were parsed already correctly.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Update various reqPtr fields and signal certain facts and error
 *      conditions via sockPtr->flags. In error conditions, sockPtr->keep is
 *      set to NS_FALSE.
 *
 *----------------------------------------------------------------------
 */
static size_t
EndOfHeader(Sock *sockPtr)
{
    Request      *reqPtr;
    const char   *s;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    reqPtr = sockPtr->reqPtr;
    assert(reqPtr != NULL);

    reqPtr->chunkStartOff = 0u;

    /*
     * Check for "expect: 100-continue" and clear flag in case we have
     * pipelining.
     */
    sockPtr->flags &= ~(NS_CONN_CONTINUE);
    s = Ns_SetIGet(reqPtr->headers, "expect");
    if (s != NULL) {
        if (*s == '1' && *(s+1) == '0' && *(s+2) == '0' && *(s+3) == '-') {
            char *dup = ns_strdup(s+4);

            Ns_StrToLower(dup);
            if (STREQ(dup, "continue")) {
                sockPtr->flags |= NS_CONN_CONTINUE;
            }
            ns_free(dup);
        }
    }

    /*
     * Handle content-length, which might be provided or not.
     * Clear length specific error flags.
     */
    sockPtr->flags &= ~(NS_CONN_ENTITYTOOLARGE);
    s = Ns_SetIGet(reqPtr->headers, "content-length");
    if (s == NULL) {
        s = Ns_SetIGet(reqPtr->headers, "Transfer-Encoding");

        if (s != NULL) {
            /* Lower case is in the standard, capitalized by macOS */
            if (STREQ(s, "chunked") || STREQ(s, "Chunked")) {
                Tcl_WideInt expected;

                reqPtr->chunkStartOff = reqPtr->roff;
                reqPtr->chunkWriteOff = reqPtr->chunkStartOff;
                reqPtr->contentLength = 0u;

                /*
                 * We need reqPtr->expectedLength for safely terminating read loop.
                 */
                s = Ns_SetIGet(reqPtr->headers, "X-Expected-Entity-Length");

                if ((s != NULL)
                    && (Ns_StrToWideInt(s, &expected) == NS_OK)
                    && (expected > 0) ) {
                    reqPtr->expectedLength = (size_t)expected;
                }
                s = NULL;
            }
        }
    }

    /*
     * In case a valid and meaningful was provided, the string with the
     * content length ("s") is not NULL.
     */
    if (s != NULL) {
        Tcl_WideInt length;

        if ((Ns_StrToWideInt(s, &length) == NS_OK) && (length > 0)) {
            reqPtr->length = (size_t)length;
            /*
             * Handle too large input requests.
             */
            if (reqPtr->length > (size_t)sockPtr->drvPtr->maxinput) {

                Ns_Log(Warning, "SockParse: request too large, length=%"
                       PRIdz ", maxinput=%" TCL_LL_MODIFIER "d",
                       reqPtr->length, sockPtr->drvPtr->maxinput);

                sockPtr->keep = NS_FALSE;
                sockPtr->flags |= NS_CONN_ENTITYTOOLARGE;

            }
            reqPtr->contentLength = (size_t)length;
        }
    }

    /*
     * Compression format handling: parse information from request headers
     * indicating allowed compression formats for quick access.
     *
     * Clear compression accepted flag
     */
    sockPtr->flags &= ~(NS_CONN_ZIPACCEPTED|NS_CONN_BROTLIACCEPTED);

    s = Ns_SetIGet(reqPtr->headers, "Accept-Encoding");
    if (s != NULL) {
        bool gzipAccept, brotliAccept;

        /*
         * Get allowed compression formats from "accept-encoding" headers.
         */
        NsParseAcceptEncoding(reqPtr->request.version, s, &gzipAccept, &brotliAccept);
        if (gzipAccept || brotliAccept) {
            /*
             * Don't allow compression formats for Range requests.
             */
            s = Ns_SetIGet(reqPtr->headers, "Range");
            if (s == NULL) {
                if (gzipAccept) {
                    sockPtr->flags |= NS_CONN_ZIPACCEPTED;
                }
                if (brotliAccept) {
                    sockPtr->flags |= NS_CONN_BROTLIACCEPTED;
                }
            }
        }
    }

    /*
     * Set up request length for spooling and further read operations
     */
    if (reqPtr->contentLength != 0u) {
        /*
         * Content-Length was provided, use it
         */
        reqPtr->length = reqPtr->contentLength;
    }

    return reqPtr->roff;
}


/*----------------------------------------------------------------------
 *
 * SockParse --
 *
 *      Construct the given conn by parsing input buffer until end of
 *      headers.  Return SOCK_READY when finished parsing.
 *
 * Results:
 *      SOCK_READY:  Conn is ready for processing.
 *      SOCK_MORE:   More input is required.
 *      SOCK_ERROR:  Malformed request.
 *      SOCK_BADREQUEST
 *      SOCK_BADHEADER
 *      SOCK_TOOMANYHEADERS
 *
 * Side effects:
 *      An Ns_Request and/or Ns_Set may be allocated.
 *      Ns_Conn buffer management offsets updated.
 *
 *----------------------------------------------------------------------
 */

static SockState
SockParse(Sock *sockPtr)
{
    const Tcl_DString  *bufPtr;
    const Driver       *drvPtr;
    Request            *reqPtr;
    char                save;
    SockState           result;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    drvPtr = sockPtr->drvPtr;

    NsUpdateProgress((Ns_Sock *) sockPtr);

    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;

    /*
     * Scan lines (header) until start of content (body-part)
     */

    while (reqPtr->coff == 0u) {
        char *s, *e;
        size_t cnt;

        /*
         * Find the next header line.
         */
        s = bufPtr->string + reqPtr->roff;
        e = memchr(s, INTCHAR('\n'), reqPtr->avail);

        if (unlikely(e == NULL)) {
            /*
             * Input not yet newline terminated - request more data.
             */
            return SOCK_MORE;
        }

        /*
         * Check for max single line overflows.
         *
         * Previous versions if the driver returned here directly an
         * error code, which was handled via HTTP error message
         * provided via SockError(). However, the SockError() handling
         * closes the connection immediately. This has the
         * consequence, that the HTTP client might never see the error
         * message, since the request was not yet fully transmitted,
         * but it will see a "broken pipe: 13" message instead. We
         * read now the full request and return the message via
         * ConnRunRequest().
         */

        if (unlikely((e - s) > drvPtr->maxline)) {
            sockPtr->keep = NS_FALSE;
            if (reqPtr->request.line == NULL) {
                Ns_Log(DriverDebug, "SockParse: maxline reached of %d bytes",
                       drvPtr->maxline);
                sockPtr->flags = NS_CONN_REQUESTURITOOLONG;
                Ns_Log(Warning, "request line is too long (%d bytes)", (int)(e - s));
            } else {
                sockPtr->flags = NS_CONN_LINETOOLONG;
                Ns_Log(Warning, "request header line is too long (%d bytes)", (int)(e - s));
            }
        }

        /*
         * Update next read pointer to end of this line.
         */
        cnt = (size_t)(e - s) + 1u;
        reqPtr->roff  += cnt;
        reqPtr->avail -= cnt;

        /*
         * Adjust end pointer to the last content character before the line
         * terminator.
         */
        if (likely(e > s) && likely(*(e-1) == '\r')) {
            --e;
        }

        /*
         * Check for end of headers in case we have not done it yet.
         */
        if (unlikely(e == s) && (reqPtr->coff == 0u)) {
            /*
             * We are at end of headers.
             */
            reqPtr->coff = EndOfHeader(sockPtr);

            /*
             * In cases the client sent "expect: 100-continue", report back that
             * everything is fine with the headers.
             */
            if ((sockPtr->flags & NS_CONN_CONTINUE) != 0u) {

                Ns_Log(Ns_LogRequestDebug, "honoring 100-continue");

                /*
                 * In case, the request entity (body) was too large, we can
                 * return immediately the error message, when the client has
                 * flagged this via "Expect:". Otherwise we have to read the
                 * full request (although it is too large) to drain the
                 * channel. Otherwise, the server might close the connection
                 * *before* it has received full request with its body from
                 * the client. We just keep the flag and let
                 * Ns_ConnRunRequest() handle the error message.
                 */
                if ((sockPtr->flags & NS_CONN_ENTITYTOOLARGE) != 0u) {
                    Ns_Log(Ns_LogRequestDebug, "100-continue: entity too large");

                    return SOCK_ENTITYTOOLARGE;

                    /*
                     * We have no other error message flagged (future ones
                     * have to be handled here).
                     */
                } else {
                    struct iovec iov[1];
                    ssize_t      sent;

                    /*
                     * Reply with "100 continue".
                     */
                    Ns_Log(Ns_LogRequestDebug, "100-continue: reply CONTINUE");

                    iov[0].iov_base = (char *)"HTTP/1.1 100 Continue\r\n\r\n";
                    iov[0].iov_len = strlen(iov[0].iov_base);

                    sent = Ns_SockSendBufs((Ns_Sock *)sockPtr, iov, 1,
                                           NULL, 0u);
                    if (sent != (ssize_t)iov[0].iov_len) {
                        Ns_Log(Warning, "could not deliver response: 100 Continue");
                        /*
                         * Should we bail out here?
                         */
                    }
                }
            }
        } else {
            /*
             * We have the request-line or a header line to process.
             */
            save = *e;
            *e = '\0';

            if (unlikely(reqPtr->request.line == NULL)) {
                /*
                 * There is no request-line set. The received line must the
                 * the request-line.
                 */
                Ns_Log(DriverDebug, "SockParse (%d): parse request line <%s>", sockPtr->sock, s);

                if (Ns_ParseRequest(&reqPtr->request, s) == NS_ERROR) {
                    /*
                     * Invalid request.
                     */
                    return SOCK_BADREQUEST;
                }

                /*
                 * HTTP 0.9 did not have a HTTP-version number or request headers
                 * and no empty line terminating the request header.
                 */
                if (unlikely(reqPtr->request.version < 1.0)) {
                    /*
                     * Pre-HTTP/1.0 request.
                     */
                    reqPtr->coff = reqPtr->roff;
                    Ns_Log(Notice, "pre-HTTP/1.0 request <%s>", reqPtr->request.line);
                }

            } else if (Ns_ParseHeader(reqPtr->headers, s, Preserve) != NS_OK) {
                /*
                 * Invalid header.
                 */
                return SOCK_BADHEADER;

            } else {
                /*
                 * Check for max number of headers
                 */
                if (unlikely(Ns_SetSize(reqPtr->headers) > (size_t)drvPtr->maxheaders)) {
                    Ns_Log(DriverDebug, "SockParse (%d): maxheaders reached of %d bytes",
                           sockPtr->sock, drvPtr->maxheaders);
                    return SOCK_TOOMANYHEADERS;
                }
            }

            *e = save;
        }
    }

    if (unlikely(reqPtr->request.line == NULL)) {
        /*
         * We are at end of headers, but we have not parsed a request line
         * (maybe just two linefeeds).
         */
        return SOCK_BADREQUEST;
    }


    /*
     * We are in the request body.
     */
    assert(reqPtr->coff > 0u);
    assert(reqPtr->request.line != NULL);

    /*
     * Check if all content has arrived.
     */
    Ns_Log(Dev, "=== length < avail (length %" PRIuz
           ", avail %" PRIuz ") tfd %d tfile %p chunkStartOff %" PRIuz,
           reqPtr->length, reqPtr->avail, sockPtr->tfd,
           (void *)sockPtr->tfile, reqPtr->chunkStartOff);

    if (reqPtr->chunkStartOff != 0u) {
        /*
         * Chunked encoding was provided.
         */
        bool   complete;
        size_t currentContentLength;

        complete = ChunkedDecode(reqPtr, NS_TRUE);
        currentContentLength = reqPtr->chunkWriteOff - reqPtr->coff;

        /*
         * A chunk might be complete, but it might not be the last
         * chunk from the client. The best thing would be to be able
         * to read until EOF here. In cases, where the (optional)
         * "expectedLength" was provided by the client, we terminate
         * depending on that information
         */
        if ((!complete)
            || (reqPtr->expectedLength != 0u && currentContentLength < reqPtr->expectedLength)) {
            /*
             * ChunkedDecode wants more data.
             */
            return SOCK_MORE;
        }
        /*
         * ChunkedDecode has enough data.
         */
        reqPtr->length = (size_t)currentContentLength;
    }

    if (reqPtr->avail < reqPtr->length) {
        Ns_Log(DriverDebug, "SockRead wait for more input");
        /*
         * Wait for more input.
         */
        return SOCK_MORE;
    }

    Ns_Log(Dev, "=== all required data is available (avail %" PRIuz", length %" PRIuz ", "
           "readahead %" TCL_LL_MODIFIER "d maxupload %" TCL_LL_MODIFIER "d) tfd %d",
           reqPtr->avail, reqPtr->length, drvPtr->readahead, drvPtr->maxupload,
           sockPtr->tfd);

    /*
     * We have all required data in the receive buffer or in a temporary file.
     *
     * - Uploads > "readahead": these are put into temporary files.
     *
     * - Uploads > "maxupload": these are put into temporary files
     *   without mmapping, no content parsing will be performed in memory.
     */
    result = SOCK_READY;

    if (sockPtr->tfile != NULL) {
        reqPtr->content = NULL;
        reqPtr->next = NULL;
        reqPtr->avail = 0u;
        Ns_Log(DriverDebug, "content spooled to file: size %" PRIdz ", file %s",
               reqPtr->length, sockPtr->tfile);
        /*
         * Nothing more to do, return via SOCK_READY;
         */
    } else {

        /*
         * Uploads < "maxupload" are spooled to files and mmapped in order to
         * provide the usual interface via [ns_conn content].
         */
        if (sockPtr->tfd > 0) {
#ifdef _WIN32
            /*
             * For _WIN32, tfd should never be set, since tfd-spooling is not
             * implemented for windows.
             */
            assert(0);
#else
            int prot = PROT_READ | PROT_WRITE;
            /*
             * Add a byte to make sure, the string termination with \0 below falls
             * always into the mmapped area. On some older OSes this might lead to
             * crashes when we hitting page boundaries.
             */
            ssize_t rc = ns_write(sockPtr->tfd, "\0", 1);
            if (rc == -1) {
                Ns_Log(Error, "socket: could not append terminating 0-byte");
            }
            sockPtr->tsize = reqPtr->length + 1;
            sockPtr->taddr = mmap(0, sockPtr->tsize, prot, MAP_PRIVATE,
                                  sockPtr->tfd, 0);
            if (sockPtr->taddr == MAP_FAILED) {
                sockPtr->taddr = NULL;
                result = SOCK_ERROR;

            } else {
                reqPtr->content = sockPtr->taddr;
                Ns_Log(Debug, "content spooled to mmapped file: readahead=%"
                       TCL_LL_MODIFIER "d, filesize=%" PRIdz,
                       drvPtr->readahead, sockPtr->tsize);
            }
#endif
        } else {
            /*
             * Set the content the begin of the remaining buffer (content offset).
             * This happens as well when reqPtr->contentLength is 0, but it is
             * needed for chunked input processing.
             */
            reqPtr->content = bufPtr->string + reqPtr->coff;
        }
        reqPtr->next = reqPtr->content;

        /*
         * Add a terminating null character. The content might be from the receive
         * buffer (Tcl_DString) or from the mmapped file. Non-mmapped files are handled
         * above.
         */
        if (reqPtr->length > 0u) {
            Ns_Log(DriverDebug, "SockRead adds null terminating character at content[%" PRIuz "]", reqPtr->length);

            reqPtr->savedChar = reqPtr->content[reqPtr->length];
            reqPtr->content[reqPtr->length] = '\0';
            if (sockPtr->taddr == NULL) {
                LogBuffer(DriverDebug, "UPDATED BUFFER", sockPtr->reqPtr->buffer.string, (size_t)reqPtr->buffer.length);
            }
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SockSetServer --
 *
 *      Set virtual server from driver context or Host header.
 *
 * Results:
 *      void.
 *
 * Side effects:
 *
 *      Updates sockPtr->servPtr. In case an invalid server set, or the
 *      required host field in HTTP/1.1 is missing the HTTP-method is set to
 *      the constant "BAD".
 *
 *----------------------------------------------------------------------
 */

static void
SockSetServer(Sock *sockPtr)
{
    char    *host;
    Request *reqPtr;
    bool     bad_request = NS_FALSE;
    Driver  *drvPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    reqPtr = sockPtr->reqPtr;
    assert(reqPtr != NULL);

    drvPtr = sockPtr->drvPtr;
    assert(drvPtr != NULL);

    sockPtr->servPtr  = drvPtr->servPtr;
    sockPtr->location = drvPtr->location;

    host = Ns_SetIGet(reqPtr->headers, "Host");
    Ns_Log(DriverDebug, "SockSetServer host '%s' request line '%s'", host, reqPtr->request.line);

    if (unlikely((host == NULL) && (reqPtr->request.version >= 1.1))) {
        /*
         * HTTP/1.1 requires host header
         */
        Ns_Log(Notice, "request header field \"Host\" is missing in HTTP/1.1 request: \"%s\"\n",
               reqPtr->request.line);
        bad_request = NS_TRUE;
    }

    if (sockPtr->servPtr == NULL) {
        const ServerMap *mapPtr = NULL;

        if (host != NULL) {
            const Tcl_HashEntry *hPtr;
            size_t               hostLength = strlen(host);

            /*
             * Remove trailing dot of host header field, since RFC 2976 allows
             * fully qualified "absolute" DNS names in host fields (see e.g. §3.2.2).
             */
            if (host[hostLength] == '.') {
                host[hostLength] = '\0';
            }
            /*
             * Convert provided host header field to lower case before hash
             * lookup.
             */
            Ns_StrToLower(host);

            hPtr = Tcl_FindHashEntry(&drvPtr->hosts, host);
            Ns_Log(DriverDebug, "SockSetServer driver '%s' host '%s' => %p",
                   drvPtr->moduleName, host, (void*)hPtr);

            if (hPtr != NULL) {
                /*
                 * Request with provided host header field could be resolved
                 * against a certain server.
                 */
                mapPtr = Tcl_GetHashValue(hPtr);

            } else {
                /*
                 * Host header field content is not found in the mapping table.
                 */
                Ns_Log(DriverDebug,
                       "cannot locate host header content '%s' in virtual hosts "
                       "table of driver '%s', fall back to default '%s'",
                       host, drvPtr->moduleName,
                       drvPtr->defMapPtr->location);

                if (Ns_LogSeverityEnabled(DriverDebug)) {
                    Tcl_HashEntry  *hPtr2;
                    Tcl_HashSearch  search;

                    hPtr2 = Tcl_FirstHashEntry(&drvPtr->hosts, &search);
                    while (hPtr2 != NULL) {
                        Ns_Log(Notice, "... host entry: '%s'\n",
                               (char *)Tcl_GetHashKey(&drvPtr->hosts, hPtr2));
                        hPtr2 = Tcl_NextHashEntry(&search);
                    }
                }
            }
        }
        if (mapPtr == NULL) {
            /*
             * Could not lookup the virtual host, Get the default mapping from the driver.
             */
            mapPtr = drvPtr->defMapPtr;
        }
        if (mapPtr != NULL) {
            sockPtr->servPtr  = mapPtr->servPtr;
            sockPtr->location = mapPtr->location;
        }
        if (sockPtr->servPtr == NULL) {
            Ns_Log(Warning, "cannot determine server for request: \"%s\" (host \"%s\")\n",
                   reqPtr->request.line, host);
            bad_request = NS_TRUE;
        }
    }

    if (unlikely(bad_request)) {
        Ns_Log(DriverDebug, "SockSetServer sets method to BAD");
        ns_free((char *)reqPtr->request.method);
        reqPtr->request.method = ns_strdup("BAD");
    }

}

/*
 *======================================================================
 *  Spooler Thread: Receive asynchronously from the client socket
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * SpoolerThread --
 *
 *      Spooling socket driver thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted on the configured listen sockets,
 *      placed on the run queue to be serviced, and gracefully
 *      closed when done.  Async sockets have the entire request read
 *      here before queuing as well.
 *
 *----------------------------------------------------------------------
 */

static void
SpoolerThread(void *arg)
{
    SpoolerQueue  *queuePtr = (SpoolerQueue*)arg;
    char           charBuffer[1];
    int            pollTimeout;
    bool           stopping;
    Sock          *sockPtr, *nextPtr, *waitPtr, *readPtr;
    Ns_Time        now, diff;
    const Driver  *drvPtr;
    PollData       pdata;

    Ns_ThreadSetName("-spooler%d-", queuePtr->id);
    queuePtr->threadName = Ns_ThreadGetName();

    /*
     * Loop forever until signaled to shut down and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "spooler%d: accepting connections", queuePtr->id);

    PollCreate(&pdata);
    Ns_GetTime(&now);
    waitPtr = readPtr = NULL;
    stopping = NS_FALSE;

    while (!stopping) {

        /*
         * If there are any read sockets, set the bits
         * and determine the minimum relative timeout.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, queuePtr->pipe[0], (short)POLLIN, NULL);

        if (readPtr == NULL) {
            pollTimeout = 30 * 1000;
        } else {
            sockPtr = readPtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, (short)POLLIN, &pdata);
                sockPtr = sockPtr->nextPtr;
            }
            pollTimeout = -1;
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */

        /*n =*/ (void) PollWait(&pdata, pollTimeout);

        if (PollIn(&pdata, 0) && unlikely(ns_recv(queuePtr->pipe[0], charBuffer, 1u, 0) != 1)) {
            Ns_Fatal("spooler: trigger ns_recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }

        /*
         * Attempt read-ahead of any new connections.
         */

        Ns_GetTime(&now);
        sockPtr = readPtr;
        readPtr = NULL;

        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            drvPtr  = sockPtr->drvPtr;
            if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                /*
                 * Peer has closed the connection
                 */
                SockRelease(sockPtr, SOCK_CLOSE, 0);

            } else if (!PollIn(&pdata, sockPtr->pidx)) {
                /*
                 * Got no data
                 */
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                    queuePtr->queuesize--;
                } else {
                    Push(sockPtr, readPtr);
                }
            } else {
                /*
                 * Got some data
                 */
                SockState n = SockRead(sockPtr, 1, &now);
                switch (n) {
                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, &drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;

                case SOCK_READY:
                    assert(sockPtr->reqPtr != NULL);
                    SockSetServer(sockPtr);
                    Push(sockPtr, waitPtr);
                    break;

                case SOCK_BADHEADER:      NS_FALL_THROUGH; /* fall through */
                case SOCK_BADREQUEST:     NS_FALL_THROUGH; /* fall through */
                case SOCK_CLOSE:          NS_FALL_THROUGH; /* fall through */
                case SOCK_CLOSETIMEOUT:   NS_FALL_THROUGH; /* fall through */
                case SOCK_ENTITYTOOLARGE: NS_FALL_THROUGH; /* fall through */
                case SOCK_ERROR:          NS_FALL_THROUGH; /* fall through */
                case SOCK_READERROR:      NS_FALL_THROUGH; /* fall through */
                case SOCK_READTIMEOUT:    NS_FALL_THROUGH; /* fall through */
                case SOCK_SHUTERROR:      NS_FALL_THROUGH; /* fall through */
                case SOCK_SPOOL:          NS_FALL_THROUGH; /* fall through */
                case SOCK_TOOMANYHEADERS: NS_FALL_THROUGH; /* fall through */
                case SOCK_WRITEERROR:     NS_FALL_THROUGH; /* fall through */
                case SOCK_WRITETIMEOUT:
                    SockRelease(sockPtr, n, errno);
                    queuePtr->queuesize--;
                    break;
                }
            }
            sockPtr = nextPtr;
        }

        /*
         * Attempt to queue any pending connection
         * after reversing the list to ensure oldest
         * connections are tried first.
         */

        if (waitPtr != NULL) {
            sockPtr = NULL;
            while ((nextPtr = waitPtr) != NULL) {
                waitPtr = nextPtr->nextPtr;
                Push(nextPtr, sockPtr);
            }
            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                if (!NsQueueConn(sockPtr, &now)) {
                    Push(sockPtr, waitPtr);
                } else {
                    queuePtr->queuesize--;
                }
                sockPtr = nextPtr;
            }
        }

        /*
         * Add more connections from the spooler queue
         */

        Ns_MutexLock(&queuePtr->lock);
        if (waitPtr == NULL) {
            sockPtr = (Sock*)queuePtr->sockPtr;
            queuePtr->sockPtr = NULL;
            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                drvPtr  = sockPtr->drvPtr;
                SockTimeout(sockPtr, &now, &drvPtr->recvwait);
                Push(sockPtr, readPtr);
                queuePtr->queuesize++;
                sockPtr = nextPtr;
            }
        }

        /*
         * Check for shutdown
         */

        stopping = queuePtr->shutdown;
        Ns_MutexUnlock(&queuePtr->lock);
    }
    PollFree(&pdata);

    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = NS_TRUE;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

static void
SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc)
{
    NS_NONNULL_ASSERT(proc != NULL);

    while (queuePtr != NULL) {
        if (ns_sockpair(queuePtr->pipe) != 0) {
            Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(proc, queuePtr, 0, &queuePtr->thread);
        queuePtr = queuePtr->nextPtr;
    }
}

static void
SpoolerQueueStop(SpoolerQueue *queuePtr, const Ns_Time *timeoutPtr, const char *name)
{

    NS_NONNULL_ASSERT(timeoutPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    while (queuePtr != NULL) {
        Ns_ReturnCode status;

        Ns_MutexLock(&queuePtr->lock);
        if (!queuePtr->stopped && !queuePtr->shutdown) {
            Ns_Log(Debug, "%s%d: triggering shutdown", name, queuePtr->id);
            queuePtr->shutdown = NS_TRUE;
            SockTrigger(queuePtr->pipe[1]);
        }
        status = NS_OK;
        while (!queuePtr->stopped && status == NS_OK) {
            status = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, timeoutPtr);
        }
        if (status != NS_OK) {
            Ns_Log(Warning, "%s%d: timeout waiting for shutdown", name, queuePtr->id);
        } else {
            /*Ns_Log(Notice, "%s%d: shutdown complete", name, queuePtr->id);*/
            if (queuePtr->thread != NULL) {
                Ns_ThreadJoin(&queuePtr->thread, NULL);
                queuePtr->thread = NULL;
            } else {
                Ns_Log(Notice, "%s%d: shutdown: thread already gone", name, queuePtr->id);
            }
            ns_sockclose(queuePtr->pipe[0]);
            ns_sockclose(queuePtr->pipe[1]);
        }
        Ns_MutexUnlock(&queuePtr->lock);
        queuePtr = queuePtr->nextPtr;
    }
}

static int
SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr)
{
    bool          trigger = NS_FALSE;
    SpoolerQueue *queuePtr;

    NS_NONNULL_ASSERT(drvPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);
    /*
     * Get the next spooler thread from the list, all spooler requests are
     * rotated between all spooler threads
     */

    Ns_MutexLock(&drvPtr->spooler.lock);
    if (drvPtr->spooler.curPtr == NULL) {
        drvPtr->spooler.curPtr = drvPtr->spooler.firstPtr;
    }
    queuePtr = drvPtr->spooler.curPtr;
    drvPtr->spooler.curPtr = drvPtr->spooler.curPtr->nextPtr;
    Ns_MutexUnlock(&drvPtr->spooler.lock);

    Ns_Log(Debug, "Spooler: %d: started fd=%d: %" PRIdz " bytes",
           queuePtr->id, sockPtr->sock, sockPtr->reqPtr->length);

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = NS_TRUE;
    }
    Push(sockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);

    /*
     * Wake up spooler thread
     */

    if (trigger) {
        SockTrigger(queuePtr->pipe[1]);
    }

    return 1;
}

/*
 *======================================================================
 *  Writer Thread: Write asynchronously to the client socket
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * NsWriterLock, NsWriterUnlock --
 *
 *       Provide an API for locking and unlocking context information
 *       for streaming asynchronous writer jobs.  The locks are just
 *       needed for managing linkage between "connPtr" and a writer
 *       entry. The lock operations are rather infrequent and the
 *       lock duration is very short, such that at a single global
 *       appears sufficient.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Change Mutex state.
 *
 *----------------------------------------------------------------------
 */
void NsWriterLock(void) {
    Ns_MutexLock(&writerlock);
}

void NsWriterUnlock(void) {
    Ns_MutexUnlock(&writerlock);
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSockFileVecCleanup --
 *
 *      Cleanup function for FileVec array in WriterSock structure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Closing potentially file descriptors, freeing Ns_FileVec memory.
 *
 *----------------------------------------------------------------------
 */
static void
WriterSockFileVecCleanup(WriterSock *wrSockPtr) {

    NS_NONNULL_ASSERT(wrSockPtr != NULL);

    if ( wrSockPtr->c.file.nbufs > 0) {
        int i;

        Ns_Log(DriverDebug, "WriterSockRelease nbufs %d", wrSockPtr->c.file.nbufs);

        for (i = 0; i < wrSockPtr->c.file.nbufs; i++) {
            /*
             * The fd of c.file.currentbuf is always the same as
             * wrSockPtr->fd and therefore already closed at this point.
             */
            if ( (i != wrSockPtr->c.file.currentbuf)
                 && (wrSockPtr->c.file.bufs[i].fd != NS_INVALID_FD) ) {

                Ns_Log(DriverDebug, "WriterSockRelease must close fd %d",
                       wrSockPtr->c.file.bufs[i].fd);
                ns_close(wrSockPtr->c.file.bufs[i].fd);
            }
        }
        ns_free(wrSockPtr->c.file.bufs);
    }
    ns_free(wrSockPtr->c.file.buf);
}


/*
 *----------------------------------------------------------------------
 *
 * WriterSockRequire, WriterSockRelease --
 *
 *      Management functions for WriterSocks. WriterSockRequire() and
 *      WriterSockRelease() are responsible for obtaining and
 *      freeing "WriterSock" structures. When shuch a structure is finally
 *      released, it is removed from the queue, the socket is
 *      closed and the memory is freed.
 *
 * Results:
 *      WriterSockRequire() returns a WriterSock from a connection,
 *      the other functions return nothing.
 *
 * Side effects:
 *      Updating reference counters, closing socket, freeing memory.
 *
 *----------------------------------------------------------------------
 */

static WriterSock *
WriterSockRequire(const Conn *connPtr) {
    WriterSock *wrSockPtr;

    NS_NONNULL_ASSERT(connPtr != NULL);

    NsWriterLock();
    wrSockPtr = (WriterSock *)connPtr->strWriter;
    if (wrSockPtr != NULL) {
        wrSockPtr->refCount ++;
    }
    NsWriterUnlock();
    return wrSockPtr;
}

static void
WriterSockRelease(WriterSock *wrSockPtr) {
    SpoolerQueue *queuePtr;

    NS_NONNULL_ASSERT(wrSockPtr != NULL);

    wrSockPtr->refCount --;

    Ns_Log(DriverDebug, "WriterSockRelease %p refCount %d keep %d",
           (void *)wrSockPtr, wrSockPtr->refCount, wrSockPtr->keep);

    if (wrSockPtr->refCount > 0) {
        return;
    }

    Ns_Log(DriverDebug,
           "Writer: closed sock %d, file fd %d, error %d/%d, "
           "sent=%" TCL_LL_MODIFIER "d, flags=%X",
           wrSockPtr->sockPtr->sock, wrSockPtr->fd,
           wrSockPtr->status, wrSockPtr->err,
           wrSockPtr->nsent, wrSockPtr->flags);

    NsPoolAddBytesSent(wrSockPtr->poolPtr, wrSockPtr->nsent);

    if (wrSockPtr->doStream != NS_WRITER_STREAM_NONE) {
        Conn *connPtr;

        NsWriterLock();
        connPtr = wrSockPtr->connPtr;
        if (connPtr != NULL && connPtr->strWriter != NULL) {
            connPtr->strWriter = NULL;
        }
        NsWriterUnlock();

        /*
         * In case, writer streams are activated for this wrSockPtr, make sure
         * to release the tmp file. See thread Naviserver Open Files on the
         * sourceforge mailing list (starting July 2019).
         */
        if (wrSockPtr->doStream == NS_WRITER_STREAM_FINISH) {
            Ns_ReleaseTemp(wrSockPtr->fd);
        }
    }

    /*
     * Remove the entry from the queue and decrement counter
     */
    queuePtr = wrSockPtr->queuePtr;
    if (queuePtr->curPtr == wrSockPtr) {
        queuePtr->curPtr = wrSockPtr->nextPtr;
        queuePtr->queuesize--;
    } else {
        WriterSock *curPtr, *lastPtr = queuePtr->curPtr;
        for (curPtr = (lastPtr != NULL) ? lastPtr->nextPtr : NULL;
             curPtr != NULL;
             lastPtr = curPtr, curPtr = curPtr->nextPtr
             ) {
            if (curPtr == wrSockPtr) {
                lastPtr->nextPtr = wrSockPtr->nextPtr;
                queuePtr->queuesize--;
                break;
            }
        }
    }

    if ((wrSockPtr->err != 0) || (wrSockPtr->status != SPOOLER_OK)) {
        int i;
        /*
         * Lookup the matching sockState from the spooler state. The array has
         * just 5 elements, on average, just 2 comparisons are needed (since
         * OK is at the end).
         */
        for (i = 0; i < Ns_NrElements(spoolerStateMap); i++) {
            if (spoolerStateMap[i].spoolerState == wrSockPtr->status) {
                SockError(wrSockPtr->sockPtr, spoolerStateMap[i].sockState, wrSockPtr->err);
                break;
            }
        }
        NsSockClose(wrSockPtr->sockPtr, (int)NS_FALSE);
    } else {
        NsSockClose(wrSockPtr->sockPtr, (int)wrSockPtr->keep);
    }
    if (wrSockPtr->clientData != NULL) {
        ns_free(wrSockPtr->clientData);
    }
    if (wrSockPtr->fd != NS_INVALID_FD) {
        if (wrSockPtr->doStream != NS_WRITER_STREAM_FINISH) {
            (void) ns_close(wrSockPtr->fd);
        }
        WriterSockFileVecCleanup(wrSockPtr);

    } else if (wrSockPtr->c.mem.bufs != NULL) {
        if (wrSockPtr->c.mem.fmap.addr != NULL) {
            NsMemUmap(&wrSockPtr->c.mem.fmap);

        } else {
            int i;
            for (i = 0; i < wrSockPtr->c.mem.nbufs; i++) {
                ns_free((char *)wrSockPtr->c.mem.bufs[i].iov_base);
            }
        }
        if (wrSockPtr->c.mem.bufs != wrSockPtr->c.mem.preallocated_bufs) {
            ns_free(wrSockPtr->c.mem.bufs);
        }
    }
    if (wrSockPtr->headerString != NULL) {
        ns_free(wrSockPtr->headerString);
    }

    ns_free(wrSockPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * WriterReadFromSpool --
 *
 *      Utility function of the WriterThread to read blocks from a
 *      file into the output buffer of the writer. It handles
 *      left overs from previous send attempts and takes care for
 *      locking in case simultaneous reading and writing from the
 *      same file.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Fills up curPtr->c.file.buf and updates counters/sizes.
 *
 *----------------------------------------------------------------------
 */

static SpoolerState
WriterReadFromSpool(WriterSock *curPtr) {
    NsWriterStreamState doStream;
    SpoolerState        status = SPOOLER_OK;
    size_t              maxsize, toRead;
    unsigned char      *bufPtr;

    NS_NONNULL_ASSERT(curPtr != NULL);

    doStream = curPtr->doStream;
    if (doStream != NS_WRITER_STREAM_NONE) {
        Ns_MutexLock(&curPtr->c.file.fdlock);
        toRead = curPtr->c.file.toRead;
        Ns_MutexUnlock(&curPtr->c.file.fdlock);
    } else {
        toRead = curPtr->c.file.toRead;

        Ns_Log(DriverDebug, "### WriterReadFromSpool [%d]: fd %d tosend %lu files %d",
                curPtr->c.file.currentbuf, curPtr->fd, toRead, curPtr->c.file.nbufs);
    }

    maxsize = curPtr->c.file.maxsize;
    bufPtr  = curPtr->c.file.buf;

    /*
     * When bufsize > 0 we have a leftover from previous send. In such
     * cases, move the leftover to the front, and fill the reminder of
     * the buffer with new data from curPtr->c.
     */

    if (curPtr->c.file.bufsize > 0u) {
        Ns_Log(DriverDebug,
               "### WriterReadFromSpool %p %.6x leftover %" PRIdz " offset %ld",
               (void *)curPtr,
               curPtr->flags,
               curPtr->c.file.bufsize,
               (long)curPtr->c.file.bufoffset);
        if (likely(curPtr->c.file.bufoffset > 0)) {
            memmove(curPtr->c.file.buf,
                    curPtr->c.file.buf + curPtr->c.file.bufoffset,
                    curPtr->c.file.bufsize);
        }
        bufPtr = curPtr->c.file.buf + curPtr->c.file.bufsize;
        maxsize -= curPtr->c.file.bufsize;
    }
    if (toRead > maxsize) {
        toRead = maxsize;
    }

    /*
     * Read content from the file into the buffer.
     */
    if (toRead > 0u) {
        ssize_t n;

        if (doStream != NS_WRITER_STREAM_NONE) {
            /*
             * In streaming mode, the connection thread writes to the
             * spool file and the writer thread reads from the same
             * file.  Therefore, we have to re-adjust the current
             * read/writer position, which might be changed by the
             * other thread. These positions have to be locked, since
             * seeking might be subject to race conditions. Here we
             * set the read pointer to the position after the last
             * send operation.
             */
            Ns_MutexLock(&curPtr->c.file.fdlock);
            (void) ns_lseek(curPtr->fd, (off_t)curPtr->nsent, SEEK_SET);
        }

        if (curPtr->c.file.nbufs == 0) {
            /*
             * Working on a single fd.
             */
            n = ns_read(curPtr->fd, bufPtr, toRead);

        } else {
            /*
             * Working on a Ns_FileVec.
             */
            int    currentbuf = curPtr->c.file.currentbuf;
            size_t wantRead = curPtr->c.file.bufs[currentbuf].length;
            size_t segSize = (wantRead > toRead ? toRead : wantRead);

            n = ns_read(curPtr->fd, bufPtr, segSize);

            Ns_Log(DriverDebug, "### WriterReadFromSpool [%d] (nbufs %d): read from fd %d want %lu got %ld (remain %lu)",
                   currentbuf, curPtr->c.file.nbufs, curPtr->fd,  segSize, n, wantRead);

            if (n > 0) {
                /*
                 * Reduce the remaining length in the Ns_FileVec for the
                 * next iteration.
                 */
                curPtr->c.file.bufs[currentbuf].length -= (size_t)n;

                if ((size_t)n < wantRead) {
                    /*
                     * Partial read on a segment.
                     */
                    Ns_Log(DriverDebug, "### WriterReadFromSpool [%d] (nbufs %d): partial read on fd %d (got %ld)",
                           currentbuf, curPtr->c.file.nbufs,
                           curPtr->fd, n);

                } else if (currentbuf < curPtr->c.file.nbufs - 1 /* && (n == wantRead) */) {
                    /*
                     * All read from this segment, setup next read.
                     */
                    ns_close(curPtr->fd);
                    curPtr->c.file.bufs[currentbuf].fd = NS_INVALID_FD;

                    curPtr->c.file.currentbuf ++;
                    curPtr->fd = curPtr->c.file.bufs[curPtr->c.file.currentbuf].fd;

                    Ns_Log(DriverDebug, "### WriterReadFromSpool switch to [%d] fd %d",
                           curPtr->c.file.currentbuf, curPtr->fd);
                }
            }
        }

        if (n <= 0) {
            status = SPOOLER_READERROR;
        } else {
            /*
             * curPtr->c.file.toRead is still protected by
             * curPtr->c.file.fdlock when needed (in streaming mode).
             */
            curPtr->c.file.toRead -= (size_t)n;
            curPtr->c.file.bufsize += (size_t)n;
        }

        if (doStream != NS_WRITER_STREAM_NONE) {
            Ns_MutexUnlock(&curPtr->c.file.fdlock);
        }
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSend --
 *
 *      Utility function of the WriterThread to send content to the client. It
 *      handles partial write operations from the lower level driver
 *      infrastructure.
 *
 * Results:
 *      either NS_OK or SOCK_ERROR;
 *
 * Side effects:
 *      Sends data, might reshuffle iovec.
 *
 *----------------------------------------------------------------------
 */

static SpoolerState
WriterSend(WriterSock *curPtr, int *err) {
    const struct iovec *bufs;
    struct iovec        vbuf;
    int                 nbufs;
    SpoolerState        status = SPOOLER_OK;
    size_t              toWrite;
    ssize_t             n;

    NS_NONNULL_ASSERT(curPtr != NULL);
    NS_NONNULL_ASSERT(err != NULL);

    /*
     * Prepare send operation
     */
    if (curPtr->fd != NS_INVALID_FD) {
        /*
         * We have a valid file descriptor, send data from file.
         *
         * Prepare sending a single buffer with curPtr->c.file.bufsize bytes
         * from the curPtr->c.file.buf to the client.
         */
        vbuf.iov_len = curPtr->c.file.bufsize;
        vbuf.iov_base = (void *)curPtr->c.file.buf;
        bufs = &vbuf;
        nbufs = 1;
        toWrite = curPtr->c.file.bufsize;
    } else {
        int i;

        /*
         * Prepare sending multiple memory buffers.  Get length of remaining
         * buffers.
         */
        toWrite = 0u;
        for (i = 0; i < curPtr->c.mem.nsbufs; i ++) {
            toWrite += curPtr->c.mem.sbufs[i].iov_len;
        }
        Ns_Log(DriverDebug,
               "### Writer wants to send remainder nbufs %d len %" PRIdz,
               curPtr->c.mem.nsbufs, toWrite);

        /*
         * Add buffers from the source and fill structure up to max
         */
        while (curPtr->c.mem.bufIdx  < curPtr->c.mem.nbufs &&
               curPtr->c.mem.sbufIdx < UIO_SMALLIOV) {
            const struct iovec *vPtr = &curPtr->c.mem.bufs[curPtr->c.mem.bufIdx];

            if (vPtr->iov_len > 0u && vPtr->iov_base != NULL) {

                Ns_Log(DriverDebug,
                       "### Writer copies source %d to scratch %d len %" PRIiovlen,
                       curPtr->c.mem.bufIdx, curPtr->c.mem.sbufIdx, vPtr->iov_len);

                toWrite += Ns_SetVec(curPtr->c.mem.sbufs, curPtr->c.mem.sbufIdx++,
                                     vPtr->iov_base, vPtr->iov_len);
                curPtr->c.mem.nsbufs++;
            }
            curPtr->c.mem.bufIdx++;
        }

        bufs  = curPtr->c.mem.sbufs;
        nbufs = curPtr->c.mem.nsbufs;
        Ns_Log(DriverDebug, "### Writer wants to send %d bufs size %" PRIdz,
               nbufs, toWrite);
    }

    /*
     * Perform the actual send operation.
     */
    n = NsDriverSend(curPtr->sockPtr, bufs, nbufs, 0u);

    if (n == -1) {
        *err = ns_sockerrno;
        status = SPOOLER_WRITEERROR;
    } else {
        /*
         * We have sent zero or more bytes.
         */
        if (curPtr->doStream != NS_WRITER_STREAM_NONE) {
            Ns_MutexLock(&curPtr->c.file.fdlock);
            curPtr->size -= (size_t)n;
            Ns_MutexUnlock(&curPtr->c.file.fdlock);
        } else {
            curPtr->size -= (size_t)n;
        }
        curPtr->nsent += n;
        curPtr->sockPtr->timeout.sec = 0;

        if (curPtr->fd != NS_INVALID_FD) {
            /*
             * File-descriptor based send operation. Reduce the (remainig)
             * buffer size the amount of data sent and adjust the buffer
             * offset. For partial send operations, this will lead to a
             * remaining buffer size > 0.
             */
            curPtr->c.file.bufsize -= (size_t)n;
            curPtr->c.file.bufoffset = (off_t)n;

        } else {
            if (n < (ssize_t)toWrite) {
                /*
                 * We have a partial transmit from the iovec
                 * structure. We have to compact it to fill content in
                 * the next round.
                 */
                curPtr->c.mem.sbufIdx = Ns_ResetVec(curPtr->c.mem.sbufs, curPtr->c.mem.nsbufs, (size_t)n);
                curPtr->c.mem.nsbufs -= curPtr->c.mem.sbufIdx;

                memmove(curPtr->c.mem.sbufs, curPtr->c.mem.sbufs + curPtr->c.mem.sbufIdx,
                        /* move the iovecs to the start of the scratch buffers */
                        sizeof(struct iovec) * (size_t)curPtr->c.mem.nsbufs);
            }
        }
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterGetInfoPtr --
 *
 *      Helper function to obtain ConnPoolInfo structure for a WriterSock.
 *
 *      The connInfoPtr is allocated only once per pool and cached in the
 *      WriterSock. Only the first time, a writer thread "sees" a pool, it
 *      allocates the structure for it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Can allocate memory
 *
 *----------------------------------------------------------------------
 */
static ConnPoolInfo *
WriterGetInfoPtr(WriterSock *curPtr, Tcl_HashTable *pools)
{
    NS_NONNULL_ASSERT(curPtr != NULL);
    NS_NONNULL_ASSERT(pools != NULL);

    if (curPtr->infoPtr == NULL) {
        int            isNew;
        Tcl_HashEntry  *hPtr;

        hPtr = Tcl_CreateHashEntry(pools, (void*)curPtr->poolPtr, &isNew);
        if (isNew == 1) {
            /*
             * This is a pool that we have not seen yet.
             */
            curPtr->infoPtr = ns_malloc(sizeof(ConnPoolInfo));
            curPtr->infoPtr->currentPoolRate = 0;
            curPtr->infoPtr->threadSlot =
                NsPoolAllocateThreadSlot(curPtr->poolPtr, Ns_ThreadId());
            Tcl_SetHashValue(hPtr, curPtr->infoPtr);
            Ns_Log(DriverDebug, "poollimit: pool '%s' allocate infoPtr with slot %lu poolLimit %d",
                   curPtr->poolPtr->pool,
                   curPtr->infoPtr->threadSlot,
                   curPtr->poolPtr->rate.poolLimit);
        } else {
            curPtr->infoPtr = (ConnPoolInfo *)Tcl_GetHashValue(hPtr);
        }
    }

    return curPtr->infoPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterPerPoolRates --
 *
 *      Compute current bandwidths per pool and writer.
 *
 *      Since we have potentially multiple writer threads running, all these
 *      might have writer threads of the same pool. In order to minimize
 *      locking, we compute first writer thread specific subresults and combine
 *      these later with with the results of the other threads.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted and their SockPtr is set to NULL
 *      such that closing actual connection does not close the socket.
 *
 *----------------------------------------------------------------------
 */

static void
WriterPerPoolRates(WriterSock *writePtr, Tcl_HashTable *pools)
{
    WriterSock     *curPtr;
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

    NS_NONNULL_ASSERT(writePtr != NULL);
    NS_NONNULL_ASSERT(pools != NULL);

    /*
     * First reset pool total rate.  We keep the bandwidth managed pools in a
     * thread-local memory. Before, we accumulate the data, we reset it.
     */
    hPtr = Tcl_FirstHashEntry(pools, &search);
    while (hPtr != NULL) {
        ConnPoolInfo *infoPtr = (ConnPoolInfo *)Tcl_GetHashValue(hPtr);
        infoPtr->currentPoolRate = 0;
        hPtr = Tcl_NextHashEntry(&search);
    }

    /*
     * Sum the actual rates per bandwidth limited pool for all active writer
     * jobs.
     */
    for (curPtr = writePtr; curPtr != NULL; curPtr = curPtr->nextPtr) {
        /*
         * Does the writer come form a badwidth limited pool?
         */
        if (curPtr->poolPtr->rate.poolLimit > 0 && curPtr->currentRate > 0) {
            /*
             * Add the actual rate to the writer specific pool rate.
             */
            ConnPoolInfo *infoPtr = WriterGetInfoPtr(curPtr, pools);

            infoPtr->currentPoolRate += curPtr->currentRate;
            Ns_Log(DriverDebug, "poollimit pool '%s' added rate poolLimit %d poolRate %d",
                   curPtr->poolPtr->pool,
                   curPtr->poolPtr->rate.poolLimit,
                   infoPtr->currentPoolRate);
        }
    }

    /*
     * Now iterate over the pools used by this thread and sum the specific
     * pool rates from all writer threads.
     */
    hPtr = Tcl_FirstHashEntry(pools, &search);
    while (hPtr != NULL) {
        ConnPool     *poolPtr = (ConnPool *)Tcl_GetHashKey(pools, hPtr);
        int           totalPoolRate, writerThreadCount, threadDeltaRate;
        ConnPoolInfo *infoPtr;

        /*
         * Compute the following indicators:
         *   - totalPoolRate: accumulated pool rates from all writer threads.
         *
         *   - threadDeltaRate: how much of the available bandwidth can i used
         *     the current thread. We assume that the distribution of writers
         *     between all writer threads is even, so we can split the
         *     available rate by the number of writer threads working on this
         *     pool.
         *
         *  - deltaPercentage: adjust in a single iteration just a fraction
         *    (e.g. 10 percent) of the potential change. This function is
         *    called often enough to justify delayed adjustments.
         */
        infoPtr = (ConnPoolInfo *)Tcl_GetHashValue(hPtr);
        totalPoolRate = NsPoolTotalRate(poolPtr,
                                        infoPtr->threadSlot,
                                        infoPtr->currentPoolRate,
                                        &writerThreadCount);

        /*
         * If nothing is going on, allow a thread the full rate.
         */
        if (infoPtr->currentPoolRate == 0) {
            threadDeltaRate = (poolPtr->rate.poolLimit - totalPoolRate);
        } else {
            threadDeltaRate = (poolPtr->rate.poolLimit - totalPoolRate) / writerThreadCount;
        }
        infoPtr->deltaPercentage = threadDeltaRate / 10;
        if (infoPtr->deltaPercentage < -50) {
            infoPtr->deltaPercentage = -50;
        }

        if (totalPoolRate > 0) {
            Ns_Log(Notice, "... pool '%s' thread's pool rate %d total pool rate %d limit %d "
                   "(#%d writer threads) -> computed rate %d (%d%%) ",
                   NsPoolName(poolPtr->pool),
                   infoPtr->currentPoolRate,
                   totalPoolRate,
                   poolPtr->rate.poolLimit,
                   writerThreadCount,
                   threadDeltaRate,
                   infoPtr->deltaPercentage
                   );
        }

        hPtr = Tcl_NextHashEntry(&search);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * WriterThread --
 *
 *      Thread that writes files to clients.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted and their SockPtr is set to NULL
 *      such that closing actual connection does not close the socket.
 *
 *----------------------------------------------------------------------
 */

static void
WriterThread(void *arg)
{
    SpoolerQueue   *queuePtr = (SpoolerQueue*)arg;
    int             err, pollTimeout;
    bool            stopping;
    Ns_Time         now;
    Sock           *sockPtr;
    const Driver   *drvPtr;
    WriterSock     *curPtr, *nextPtr, *writePtr;
    PollData        pdata;
    Tcl_HashTable   pools;     /* used for accumulating bandwidth per pool */

    Ns_ThreadSetName("-writer%d-", queuePtr->id);
    queuePtr->threadName = Ns_ThreadGetName();

    Tcl_InitHashTable(&pools, TCL_ONE_WORD_KEYS);

    /*
     * Loop forever until signaled to shut down and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "writer%d: accepting connections", queuePtr->id);

    PollCreate(&pdata);
    writePtr = NULL;
    stopping = NS_FALSE;

    while (!stopping) {
        char charBuffer[1];

        /*
         * If there are any write sockets, set the bits.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, queuePtr->pipe[0], (short)POLLIN, NULL);

        if (writePtr == NULL) {
            pollTimeout = 30 * 1000;
        } else {

            /*
             * If per-pool bandwidth management is requested, compute the base
             * data for the adjustment. If there is no bandwidth management
             * requested, there is no slowdow.
             */
            if (NsWriterBandwidthManagement) {
                WriterPerPoolRates(writePtr, &pools);
            }

            /*
             * There are writers active. Determine on which writers we poll
             * and compute the maximal poll wait time.
             */
            pollTimeout = 1000;
            for (curPtr = writePtr; curPtr != NULL; curPtr = curPtr->nextPtr) {
                int sleepTimeMs = 0;

                Ns_Log(DriverDebug, "### Writer poll collect %p size %" PRIdz
                       " streaming %d rateLimit %d",
                       (void *)curPtr, curPtr->size, curPtr->doStream, curPtr->rateLimit);

                if (curPtr->rateLimit > 0
                    && curPtr->nsent > 0
                    && curPtr->currentRate > 0
                    ) {
                    int  currentMs, targetTimeMs;

                    /*
                     * Perform per-pool rate management, when
                     *  - a poolLimit is provided,
                     *  - we have performance data of thee pool, and
                     *  - changes are possible (as flagged by deltaPercentage).
                     */
                    if (NsWriterBandwidthManagement
                        && curPtr->poolPtr->rate.poolLimit > 0
                        && curPtr->infoPtr != NULL
                        && curPtr->infoPtr->deltaPercentage != 0
                        ) {
                        /*
                         * Only adjust data for busy writer jobs, which
                         * are close to their limits.
                         */
                        bool onLimit = (curPtr->currentRate*100 / curPtr->rateLimit) > 90;

                        Ns_Log(DriverDebug, "we allowed %d we use %d on limit %d (%d) , we can do %d%%",
                               curPtr->rateLimit,  curPtr->currentRate,
                               (int)onLimit, curPtr->currentRate*100/curPtr->rateLimit,
                               curPtr->infoPtr->deltaPercentage);
                        if (onLimit) {
                            /*
                             * Compute new rate limit based on
                             * positive/negative delta percentage.
                             */
                            int newRate = curPtr->currentRate +
                                (curPtr->currentRate * curPtr->infoPtr->deltaPercentage / 100);
                            /*
                             * Sanity checks:
                             *  - never allow more than poolLimit
                             *  - never kill connections completely (e.g. minRate 5KB/s)
                             */
                            if (newRate > curPtr->poolPtr->rate.poolLimit) {
                                newRate = curPtr->poolPtr->rate.poolLimit;
                            } else if (newRate < 5) {
                                newRate = 5;
                            }
                            Ns_Log(Notice, "... pool '%s' new rate limit changed from %d to %d KB/s (delta %d%%)",
                                   curPtr->poolPtr->pool, curPtr->rateLimit, newRate,
                                   curPtr->infoPtr->deltaPercentage);
                            curPtr->rateLimit = newRate;
                        }
                    }

                    /*
                     * Adjust rate to the rate limit.
                     */
                    currentMs    = (int)(curPtr->nsent/(Tcl_WideInt)curPtr->currentRate);
                    targetTimeMs = (int)(curPtr->nsent/(Tcl_WideInt)curPtr->rateLimit);
                    sleepTimeMs = 1 + targetTimeMs - currentMs;
                    Ns_Log(WriterDebug, "### Writer(%d)"
                           " byte sent %" TCL_LL_MODIFIER "d msecs %d rate %d KB/s"
                           " targetRate %d KB/s sleep %d",
                           curPtr->sockPtr->sock,
                           curPtr->nsent, currentMs,
                           curPtr->currentRate,
                           curPtr->rateLimit,
                           sleepTimeMs);
                }

                if (likely(curPtr->size > 0u)) {
                    if (sleepTimeMs <= 0) {
                        SockPoll(curPtr->sockPtr, (short)POLLOUT, &pdata);
                        pollTimeout = -1;
                    } else {
                        pollTimeout = MIN(sleepTimeMs, pollTimeout);
                    }
                } else if (unlikely(curPtr->doStream == NS_WRITER_STREAM_FINISH)) {
                    pollTimeout = -1;
                }
            }
        }
        Ns_Log(DriverDebug, "### Writer final pollTimeout %d", pollTimeout);

        /*
         * Select and drain the trigger pipe if necessary.
         */
        (void) PollWait(&pdata, pollTimeout);

        if (PollIn(&pdata, 0) && unlikely(ns_recv(queuePtr->pipe[0], charBuffer, 1u, 0) != 1)) {
            Ns_Fatal("writer: trigger ns_recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }

        /*
         * Write to all available sockets
         */
        Ns_GetTime(&now);
        curPtr = writePtr;
        writePtr = NULL;

        while (curPtr != NULL) {
            NsWriterStreamState doStream;
            SpoolerState        spoolerState = SPOOLER_OK;

            nextPtr = curPtr->nextPtr;
            sockPtr = curPtr->sockPtr;
            err = 0;

            /*
             * The truth value of doStream does not change through
             * concurrency.
             */
            doStream = curPtr->doStream;

            if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                Ns_Log(DriverDebug, "### Writer %p reached POLLHUP fd %d", (void *)curPtr, sockPtr->sock);
                spoolerState = SPOOLER_CLOSE;
                err = 0;
                curPtr->infoPtr = WriterGetInfoPtr(curPtr, &pools);
                curPtr->infoPtr->currentPoolRate += curPtr->currentRate;


            } else if (likely(PollOut(&pdata, sockPtr->pidx)) || (doStream == NS_WRITER_STREAM_FINISH)) {
                /*
                 * The socket is writable, we can compute the rate, when
                 * something was sent already and some kind of rate limiting
                 * is in place ... and we have sent enough data to make a good
                 * estimate (just after the 2nd send, so more than driver
                 * buffer size.
                 */
                Ns_Log(DriverDebug, "Socket of pool '%s' is writable, writer limit %d nsent %ld",
                       curPtr->poolPtr->pool, curPtr->rateLimit, (long)curPtr->nsent);

                if (curPtr->rateLimit > 0
                    && (size_t)curPtr->nsent > curPtr->sockPtr->drvPtr->bufsize
                    )  {
                    Ns_Time diff;
                    long    currentMs;

                    Ns_DiffTime(&now, &curPtr->startTime, &diff);
                    currentMs = Ns_TimeToMilliseconds(&diff);
                    if (currentMs > 0) {
                        curPtr->currentRate = (int)((curPtr->nsent)/(Tcl_WideInt)currentMs);
                        Ns_Log(DriverDebug,
                               "Socket of pool '%s' is writable, currentMs %ld has updated current rate %d",
                               curPtr->poolPtr->pool, currentMs,curPtr->currentRate);
                    }
                }
                Ns_Log(DriverDebug,
                       "### Writer %p can write to client fd %d (trigger %d) streaming %.6x"
                       " size %" PRIdz " nsent %" TCL_LL_MODIFIER "d bufsize %" PRIdz,
                       (void *)curPtr, sockPtr->sock, PollIn(&pdata, 0), doStream,
                       curPtr->size, curPtr->nsent, curPtr->c.file.bufsize);
                if (unlikely(curPtr->size < 1u)) {
                    /*
                     * Size < 1 means that everything was sent.
                     */
                    if (doStream != NS_WRITER_STREAM_ACTIVE) {
                        if (doStream == NS_WRITER_STREAM_FINISH) {
                            Ns_ReleaseTemp(curPtr->fd);
                        }
                        spoolerState = SPOOLER_CLOSE;
                    }
                } else {
                    /*
                     * If size > 0, there is still something to send.
                     * If we are spooling from a file, read some data
                     * from the (spool) file and place it into curPtr->c.file.buf.
                     */
                    if (curPtr->fd != NS_INVALID_FD) {
                        spoolerState = WriterReadFromSpool(curPtr);
                    }

                    if (spoolerState == SPOOLER_OK) {
                        spoolerState = WriterSend(curPtr, &err);
                    }
                }
            } else {

                /*
                 *  Mark when first timeout occurred or check if it is already
                 *  for too long and we need to stop this socket
                 */
                if (sockPtr->timeout.sec == 0) {
                    Ns_Log(DriverDebug, "Writer %p fd %d setting sendwait %ld.%6ld",
                           (void *)curPtr, sockPtr->sock,
                           curPtr->sockPtr->drvPtr->sendwait.sec,
                           curPtr->sockPtr->drvPtr->sendwait.usec);
                    SockTimeout(sockPtr, &now, &curPtr->sockPtr->drvPtr->sendwait);
                } else if (Ns_DiffTime(&sockPtr->timeout, &now, NULL) <= 0) {
                    Ns_Log(DriverDebug, "Writer %p fd %d timeout", (void *)curPtr, sockPtr->sock);
                    err          = ETIMEDOUT;
                    spoolerState = SPOOLER_CLOSETIMEOUT;
                }
            }

            /*
             * Check result status and close the socket in case of
             * timeout or completion
             */

            Ns_MutexLock(&queuePtr->lock);
            if (spoolerState == SPOOLER_OK) {
                if (curPtr->size > 0u || doStream == NS_WRITER_STREAM_ACTIVE) {
                    Ns_Log(DriverDebug,
                           "Writer %p continue OK (size %" PRIdz ") => PUSH",
                           (void *)curPtr, curPtr->size);
                    Push(curPtr, writePtr);
                } else {
                    Ns_Log(DriverDebug,
                           "Writer %p done OK (size %" PRIdz ") => RELEASE",
                           (void *)curPtr, curPtr->size);
                    WriterSockRelease(curPtr);
                }
            } else {
                /*
                 * spoolerState might be SPOOLER_CLOSE or SPOOLER_*TIMEOUT, or SPOOLER_*ERROR
                 */
                Ns_Log(DriverDebug,
                       "Writer %p fd %d release, not OK (status %d) => RELEASE",
                       (void *)curPtr, curPtr->sockPtr->sock, (int)spoolerState);
                curPtr->status = spoolerState;
                curPtr->err    = err;
                WriterSockRelease(curPtr);
            }
            Ns_MutexUnlock(&queuePtr->lock);
            curPtr = nextPtr;
        }

        /*
         * Add more sockets to the writer queue
         */

        if (queuePtr->sockPtr != NULL) {
            Ns_MutexLock(&queuePtr->lock);
            if (queuePtr->sockPtr != NULL) {
                curPtr = queuePtr->sockPtr;
                queuePtr->sockPtr = NULL;
                while (curPtr != NULL) {
                    nextPtr = curPtr->nextPtr;
                    sockPtr = curPtr->sockPtr;
                    drvPtr  = sockPtr->drvPtr;
                    SockTimeout(sockPtr, &now, &drvPtr->sendwait);
                    Push(curPtr, writePtr);
                    queuePtr->queuesize++;
                    curPtr = nextPtr;
                }
                queuePtr->curPtr = writePtr;
            }
            Ns_MutexUnlock(&queuePtr->lock);
        }

        /*
         * Check for shutdown
         */

        stopping = queuePtr->shutdown;
    }
    PollFree(&pdata);

    {
        /*
         * Free ConnPoolInfo
         */
        Tcl_HashSearch  search;
        Tcl_HashEntry  *hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {
            ConnPoolInfo *infoPtr = (ConnPoolInfo *)Tcl_GetHashValue(hPtr);
            ns_free(infoPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        /*
         * Delete the hash table for pools.
         */
        Tcl_DeleteHashTable(&pools);
    }


    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = NS_TRUE;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsWriterFinish --
 *
 *      Finish a streaming writer job (typically called at the close
 *      of a connection). A streaming writer job is fed typically by a
 *      sequence of ns_write operations. After such an operation, the
 *      WriterThread has to keep the writer job alive.
 *      NsWriterFinish() tells the WriterThread that no more
 *      other writer jobs will come from this connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Change the state of the writer job and trigger the queue.
 *
 *----------------------------------------------------------------------
 */
void
NsWriterFinish(NsWriterSock *wrSockPtr) {
    WriterSock *writerSockPtr = (WriterSock *)wrSockPtr;

    NS_NONNULL_ASSERT(wrSockPtr != NULL);

    Ns_Log(DriverDebug, "NsWriterFinish: %p", (void *)writerSockPtr);
    writerSockPtr->doStream = NS_WRITER_STREAM_FINISH;
    SockTrigger(writerSockPtr->queuePtr->pipe[1]);
}


/*
 *----------------------------------------------------------------------
 *
 * WriterSetupStreamingMode --
 *
 *      In streaming mode, setup a temporary fd which is used as input and
 *      output. Streaming i/o will append to the file, while the write will
 *      read from it.
 *
 * Results:
 *      Ns_ReturnCode (NS_OK, NS_ERROR, NS_FILTER_BREAK). In the last case
 *      signals that all processing was already performed and the caller can
 *      stop handling more data. On success, the function returns an fd as
 *      last argument.
 *
 * Side effects:
 *      Potentially allocating temp file and updating connPtr members.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
WriterSetupStreamingMode(Conn *connPtr, struct iovec *bufs, int nbufs, int *fdPtr)
{
    bool           first;
    size_t         wrote = 0u;
    WriterSock    *wrSockPtr1;
    Ns_ReturnCode  status = NS_OK;

    NS_NONNULL_ASSERT(connPtr != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);
    NS_NONNULL_ASSERT(fdPtr != NULL);

    Ns_Log(DriverDebug, "NsWriterQueue: streaming writer job");

    if (connPtr->fd == 0) {
        /*
         * Create a new temporary spool file and provide the fd to the
         * connection thread via connPtr.
         */
        first = NS_TRUE;
        wrSockPtr1 = NULL;

        *fdPtr = Ns_GetTemp();
        connPtr->fd = *fdPtr;

        Ns_Log(DriverDebug, "NsWriterQueue: new tmp file has fd %d", *fdPtr);

    } else {
        /*
         * Reuse previously created spool file.
         */
        first = NS_FALSE;
        wrSockPtr1 = WriterSockRequire(connPtr);

        if (wrSockPtr1 == NULL) {
            Ns_Log(Notice,
                   "NsWriterQueue: writer job was already canceled (fd %d); maybe user dropped connection",
                   connPtr->fd);
            return NS_ERROR;

        } else {
            /*
             * lock only, when first == NS_FALSE.
             */
            Ns_MutexLock(&wrSockPtr1->c.file.fdlock);
            (void)ns_lseek(connPtr->fd, 0, SEEK_END);
        }
    }

    /*
     * For the time being, handle just "string data" in streaming
     * output (iovec bufs). Write the content to the spool file.
     */
    {
        int i;

        for (i = 0; i < nbufs; i++) {
            ssize_t j = ns_write(connPtr->fd, bufs[i].iov_base, bufs[i].iov_len);

            if (j > 0) {
                wrote += (size_t)j;
                Ns_Log(Debug, "NsWriterQueue: fd %d [%d] spooled %" PRIdz " of %" PRIiovlen " OK %d",
                       connPtr->fd, i, j, bufs[i].iov_len, (j == (ssize_t)bufs[i].iov_len));
            } else {
                Ns_Log(Warning, "NsWriterQueue: spool to fd %d write operation failed",
                       connPtr->fd);
            }
        }
    }

    if (first) {
        //bufs = NULL;
        connPtr->nContentSent = wrote;
#ifndef _WIN32
        /*
         * sock_set_blocking can't be used under windows, since sockets
         * are under windows no file descriptors.
         */
        (void)ns_sock_set_blocking(connPtr->fd, NS_FALSE);
#endif
        /*
         * Fall through to register stream writer with temp file
         */
    } else {
        WriterSock *writerSockPtr;

        /*
         * This is a later streaming operation, where the writer job
         * (strWriter) was previously established.
         */
        assert(wrSockPtr1 != NULL);
        /*
         * Update the controlling variables (size and toread) in the connPtr,
         * and the length info for the access log, and trigger the writer to
         * notify it about the change.
         */

        writerSockPtr = (WriterSock *)connPtr->strWriter;
        writerSockPtr->size += wrote;
        writerSockPtr->c.file.toRead += wrote;
        Ns_MutexUnlock(&wrSockPtr1->c.file.fdlock);

        connPtr->nContentSent += wrote;
        if (likely(wrSockPtr1->queuePtr != NULL)) {
            SockTrigger(wrSockPtr1->queuePtr->pipe[1]);
        }
        WriterSockRelease(wrSockPtr1);
        status = NS_FILTER_BREAK;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsWriterQueue --
 *
 *      Submit a new job to the writer queue.
 *
 * Results:
 *
 *      NS_ERROR means that the Writer thread refuses to accept this
 *      job and that the client (the connection thread) has to handle
 *      this data.  NS_OK means that the Writer thread cares for
 *      transmitting the content to the client.
 *
 * Side effects:
 *      Potentially adding a job to the writer queue.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsWriterQueue(Ns_Conn *conn, size_t nsend,
              Tcl_Channel chan, FILE *fp, int fd,
              struct iovec *bufs, int nbufs,
              const Ns_FileVec *filebufs, int nfilebufs,
              bool everysize)
{
    Conn          *connPtr;
    WriterSock    *wrSockPtr;
    SpoolerQueue  *queuePtr;
    DrvWriter     *wrPtr;
    bool           trigger = NS_FALSE;
    size_t         headerSize;
    Ns_ReturnCode  status = NS_OK;
    Ns_FileVec    *fbufs = NULL;
    int            nfbufs = 0;

    NS_NONNULL_ASSERT(conn != NULL);
    connPtr = (Conn *)conn;

    if (unlikely(connPtr->sockPtr == NULL)) {
        Ns_Log(Warning,
               "NsWriterQueue: called without sockPtr size %" PRIdz " bufs %d flags %.6x stream %.6x chan %p fd %d",
               nsend, nbufs, connPtr->flags, connPtr->flags & NS_CONN_STREAM,
               (void *)chan, fd);
        status = NS_ERROR;
        wrPtr = NULL;
    } else {

        wrPtr = &connPtr->sockPtr->drvPtr->writer;

        Ns_Log(DriverDebug,
               "NsWriterQueue: size %" PRIdz " bufs %p (%d) flags %.6x stream %.6x chan %p fd %d thread %d",
               nsend, (void *)bufs, nbufs, connPtr->flags, connPtr->flags & NS_CONN_STREAM,
               (void *)chan, fd, wrPtr->threads);

        if (unlikely(wrPtr->threads == 0)) {
            Ns_Log(DriverDebug, "NsWriterQueue: no writer threads configured");
            status = NS_ERROR;

        } else if (nsend < (size_t)wrPtr->writersize && !everysize && connPtr->fd == 0) {
            Ns_Log(DriverDebug, "NsWriterQueue: file is too small(%" PRIdz " < %" PRIdz ")",
                   nsend, wrPtr->writersize);
            status = NS_ERROR;
        }
    }
    if (status != NS_OK) {
        return status;
    }

    assert(wrPtr != NULL);

    /*
     * In streaming mode, setup a temporary fd which is used as input and
     * output. Streaming i/o will append to the file, while the write will
     * read from it.
     */
    if (((connPtr->flags & NS_CONN_STREAM) != 0u) || connPtr->fd > 0) {

        if (wrPtr->doStream == NS_WRITER_STREAM_NONE) {
            status = NS_ERROR;
        } else if (unlikely(fp != NULL || fd != NS_INVALID_FD)) {
            Ns_Log(DriverDebug, "NsWriterQueue: does not stream from this source via writer");
            status = NS_ERROR;
        } else {
            status = WriterSetupStreamingMode(connPtr, bufs, nbufs, &fd);
        }

        if (unlikely(status != NS_OK)) {
            if (status == NS_FILTER_BREAK) {
                status = NS_OK;
            }
            return status;
        }

        /*
         * As a result of successful WriterSetupStreamingMode(), we have fd
         * set.
         */
        assert(fd != NS_INVALID_FD);

    } else {
        if (fp != NULL) {
            /*
             * The client provided an open file pointer and closes it
             */
            fd = ns_dup(fileno(fp));
        } else if (fd != NS_INVALID_FD) {
            /*
             * The client provided an open file descriptor and closes it
             */
            fd = ns_dup(fd);
        } else if (chan != NULL) {
            ClientData clientData;
            /*
             * The client provided an open Tcl channel and closes it
             */
            if (Tcl_GetChannelHandle(chan, TCL_READABLE, &clientData) != TCL_OK) {
                return NS_ERROR;
            }
            fd = ns_dup(PTR2INT(clientData));
        } else if (filebufs != NULL && nfilebufs > 0) {
            /*
             * The client provided Ns_FileVec with open files. The client is
             * responsible for closing it, like in all other cases.
             */
            size_t i;

            /*
             * This is the only case, where fbufs will be != NULL,
             * i.e. keeping a duplicate of the passed-in Ns_FileVec structure
             * for which the client is responsible.
             */
            fbufs = (Ns_FileVec *)ns_calloc((size_t)nfilebufs, sizeof(Ns_FileVec));
            nfbufs = nfilebufs;

            for (i = 0u; i < (size_t)nfilebufs; i++) {
                fbufs[i].fd = ns_dup(filebufs[i].fd);
                fbufs[i].length = filebufs[i].length;
                fbufs[i].offset = filebufs[i].offset;
            }
            /*
             * Place the fd of the first Ns_FileVec to fd.
             */
            fd = fbufs[0].fd;

            Ns_Log(DriverDebug, "NsWriterQueue: filevec mode, take first fd %d tosend %lu", fd, nsend);
        }
    }

    Ns_Log(DriverDebug, "NsWriterQueue: writer threads %d nsend %" PRIdz " writersize %" PRIdz,
           wrPtr->threads, nsend, wrPtr->writersize);

    assert(connPtr->poolPtr != NULL);
    connPtr->poolPtr->stats.spool++;

    wrSockPtr = (WriterSock *)ns_calloc(1u, sizeof(WriterSock));
    wrSockPtr->sockPtr = connPtr->sockPtr;
    wrSockPtr->poolPtr = connPtr->poolPtr;  /* just for being able to trace back the origin, e.g. list */
    wrSockPtr->sockPtr->timeout.sec = 0;
    wrSockPtr->flags = connPtr->flags;
    wrSockPtr->refCount = 1;
    /*
     * Take the rate limit from the connection.
     */
    wrSockPtr->rateLimit = connPtr->rateLimit;
    if (wrSockPtr->rateLimit == -1) {
        /*
         * The value was not specified via connection. Use either the pool
         * limit as a base for the computation or fall back to the driver
         * default value.
         */
        if (connPtr->poolPtr->rate.poolLimit > 0) {
            /*
             * Very optimistic start value, but values will float through via
             * bandwidth management.
             */
            wrSockPtr->rateLimit = connPtr->poolPtr->rate.poolLimit / 2;
        } else {
            wrSockPtr->rateLimit = wrPtr->rateLimit;
        }
    }
    Ns_Log(WriterDebug, "### Writer(%d): initial rate limit %d KB/s",
           wrSockPtr->sockPtr->sock, wrSockPtr->rateLimit);

    /*
     * Make sure we have proper content length header for
     * keep-alive/pipelining.
     */
    Ns_ConnSetLengthHeader(conn, nsend, (wrSockPtr->flags & NS_CONN_STREAM) != 0u);

    /*
     * Flush the headers
     */

    if ((conn->flags & NS_CONN_SENTHDRS) == 0u) {
        Tcl_DString    ds;

        Ns_DStringInit(&ds);
        Ns_Log(DriverDebug, "### Writer(%d): add header", fd);
        conn->flags |= NS_CONN_SENTHDRS;
        (void)Ns_CompleteHeaders(conn, nsend, 0u, &ds);

        headerSize = (size_t)Ns_DStringLength(&ds);
        if (headerSize > 0u) {
            wrSockPtr->headerString = ns_strdup(Tcl_DStringValue(&ds));
        }
        Ns_DStringFree(&ds);
    } else {
        headerSize = 0u;
    }

    if (fd != NS_INVALID_FD) {
        /* maybe add mmap support for files (fd != NS_INVALID_FD) */

        wrSockPtr->fd = fd;
        wrSockPtr->c.file.bufs = fbufs;
        wrSockPtr->c.file.nbufs = nfbufs;

        Ns_Log(DriverDebug, "### Writer(%d) tosend %" PRIdz " files %d bufsize %" PRIdz,
               fd, nsend, nfbufs, wrPtr->bufsize);

        if (unlikely(headerSize >= wrPtr->bufsize)) {
            /*
             * We have a header which is larger than bufsize; place it
             * as "leftover" and use the headerString as buffer for file
             * reads (rather rare case)
             */
            wrSockPtr->c.file.buf = (unsigned char *)wrSockPtr->headerString;
            wrSockPtr->c.file.maxsize = headerSize;
            wrSockPtr->c.file.bufsize = headerSize;
            wrSockPtr->headerString = NULL;
        } else if (headerSize > 0u) {
            /*
             * We have a header that fits into the bufsize; place it
             * as "leftover" at the end of the buffer.
             */
            wrSockPtr->c.file.buf = ns_malloc(wrPtr->bufsize);
            memcpy(wrSockPtr->c.file.buf, wrSockPtr->headerString, headerSize);
            wrSockPtr->c.file.bufsize = headerSize;
            wrSockPtr->c.file.maxsize = wrPtr->bufsize;
            ns_free(wrSockPtr->headerString);
            wrSockPtr->headerString = NULL;
        } else {
            assert(wrSockPtr->headerString == NULL);
            wrSockPtr->c.file.buf = ns_malloc(wrPtr->bufsize);
            wrSockPtr->c.file.maxsize = wrPtr->bufsize;
        }
        wrSockPtr->c.file.bufoffset = 0;
        wrSockPtr->c.file.toRead = nsend;

    } else if (bufs != NULL) {
        int   i, j, headerbufs = (headerSize > 0u ? 1 : 0);

        wrSockPtr->fd = NS_INVALID_FD;

        if (nbufs+headerbufs < UIO_SMALLIOV) {
            wrSockPtr->c.mem.bufs = wrSockPtr->c.mem.preallocated_bufs;
        } else {
            Ns_Log(DriverDebug, "NsWriterQueue: alloc %d iovecs", nbufs);
            wrSockPtr->c.mem.bufs = ns_calloc((size_t)nbufs + (size_t)headerbufs, sizeof(struct iovec));
        }
        wrSockPtr->c.mem.nbufs = nbufs+headerbufs;
        if (headerbufs != 0) {
            wrSockPtr->c.mem.bufs[0].iov_base = wrSockPtr->headerString;
            wrSockPtr->c.mem.bufs[0].iov_len  = headerSize;
        }

        if (connPtr->fmap.addr != NULL) {
            Ns_Log(DriverDebug, "NsWriterQueue: deliver fmapped %p", (void *)connPtr->fmap.addr);
            /*
             * Deliver an mmapped file, no need to copy content
             */
            for (i = 0, j=headerbufs; i < nbufs; i++, j++) {
                wrSockPtr->c.mem.bufs[j].iov_base = bufs[i].iov_base;
                wrSockPtr->c.mem.bufs[j].iov_len  = bufs[i].iov_len;
            }
            /*
             * Make a copy of the fmap structure and make clear that
             * we unmap in the writer thread.
             */
            wrSockPtr->c.mem.fmap = connPtr->fmap;
            connPtr->fmap.addr = NULL;
            /* header string will be freed via wrSockPtr->headerString */

        } else {
            /*
             * Deliver a content from iovec. The lifetime of the
             * source is unknown, we have to copy the c.
             */
            for (i = 0, j=headerbufs; i < nbufs; i++, j++) {
                wrSockPtr->c.mem.bufs[j].iov_base = ns_malloc(bufs[i].iov_len);
                wrSockPtr->c.mem.bufs[j].iov_len  = bufs[i].iov_len;
                memcpy(wrSockPtr->c.mem.bufs[j].iov_base, bufs[i].iov_base, bufs[i].iov_len);
            }
            /* header string will be freed a buf[0] */
            wrSockPtr->headerString = NULL;
        }

    } else {
        ns_free(wrSockPtr);
        return NS_ERROR;
    }

    /*
     * Add header size to total size.
     */
    nsend += headerSize;


    if (connPtr->clientData != NULL) {
        wrSockPtr->clientData = ns_strdup(connPtr->clientData);
    }
    wrSockPtr->startTime = *Ns_ConnStartTime(conn);

    /*
     * Setup streaming context before sending potentially headers.
     */

    if ((wrSockPtr->flags & NS_CONN_STREAM) != 0u) {
        wrSockPtr->doStream = NS_WRITER_STREAM_ACTIVE;
        assert(connPtr->strWriter == NULL);
        /*
         * Add a reference to the stream writer to the connection such
         * it can efficiently append to a stream when multiple output
         * operations happen. The backpointer (from the stream writer
         * to the connection is needed to clear the reference to the
         * writer in case the writer is deleted. No locks are needed,
         * since nobody can share this structure yet.
         */
        connPtr->strWriter = (NsWriterSock *)wrSockPtr;
        wrSockPtr->connPtr = connPtr;
    }

    /*
     * Tell connection, that writer handles the output (including
     * closing the connection to the client).
     */

    connPtr->flags |= NS_CONN_SENT_VIA_WRITER;
    wrSockPtr->keep = connPtr->keep > 0 ? NS_TRUE : NS_FALSE;
    wrSockPtr->size = nsend;
    Ns_Log(DriverDebug, "NsWriterQueue NS_CONN_SENT_VIA_WRITER connPtr %p",
           (void*)connPtr);

    if ((wrSockPtr->flags & NS_CONN_STREAM) == 0u) {
        Ns_Log(DriverDebug, "NsWriterQueue NS_CONN_SENT_VIA_WRITER connPtr %p clear sockPtr %p",
               (void*)connPtr, (void*)connPtr->sockPtr);
        connPtr->sockPtr = NULL;
        connPtr->flags |= NS_CONN_CLOSED;
        connPtr->nContentSent = nsend - headerSize;
    }

    /*
     * Get the next writer thread from the list, all writer requests are
     * rotated between all writer threads
     */

    Ns_MutexLock(&wrPtr->lock);
    if (wrPtr->curPtr == NULL) {
        wrPtr->curPtr = wrPtr->firstPtr;
    }
    queuePtr = wrPtr->curPtr;
    wrPtr->curPtr = wrPtr->curPtr->nextPtr;
    Ns_MutexUnlock(&wrPtr->lock);

    Ns_Log(WriterDebug, "Writer(%d): started: id=%d fd=%d, "
           "size=%" PRIdz ", flags=%X, rate %d KB/s: %s",
           wrSockPtr->sockPtr->sock,
           queuePtr->id, wrSockPtr->fd,
           nsend, wrSockPtr->flags,
           wrSockPtr->rateLimit,
           connPtr->request.line);

    /*
     * Now add new writer socket to the writer thread's queue
     */
    wrSockPtr->queuePtr = queuePtr;

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = NS_TRUE;
    }

    Push(wrSockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);

    /*
     * Wake up writer thread
     */

    if (trigger) {
        SockTrigger(queuePtr->pipe[1]);
    }

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DriverWriterFromObj --
 *
 *      Lookup driver by name and return its DrvWriter. When driverObj is
 *      NULL, get the driver from the conn.
 *
 * Results:
 *      Ns_ReturnCode
 *
 * Side effects:
 *      Set error message in interp in case of failure.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
DriverWriterFromObj( Tcl_Interp *interp, Tcl_Obj *driverObj, Ns_Conn *conn, DrvWriter **wrPtrPtr) {
    Driver       *drvPtr;
    const char   *driverName = NULL;
    int           driverNameLen = 0;
    DrvWriter    *wrPtr = NULL;
    Ns_ReturnCode result;

    /*
     * If no driver is provided, take the current driver. The caller has
     * to make sure that in cases, where no driver is specified, the
     * command is run in a connection thread.
     */
    if (driverObj == NULL) {
        if (conn != NULL) {
            driverName = Ns_ConnDriverName(conn);
            driverNameLen = (int)strlen(driverName);
        }
    } else {
        driverName = Tcl_GetStringFromObj(driverObj, &driverNameLen);
    }

    if (driverName != NULL) {

        for (drvPtr = firstDrvPtr; drvPtr != NULL; drvPtr = drvPtr->nextPtr) {
            if (strncmp(driverName, drvPtr->threadName, (size_t)driverNameLen) == 0) {
                if (drvPtr->writer.firstPtr != NULL) {
                    wrPtr = &drvPtr->writer;
                }
                break;
            }
        }
    }
    if (unlikely(wrPtr == NULL)) {
        Ns_TclPrintfResult(interp, "no writer configured for a driver with name %s",
                           driverName);
        result = NS_ERROR;
    } else {
        *wrPtrPtr = wrPtr;
        result = NS_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * WriterSubmitObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer submit" command.
 *      Send the provided data to the client.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSubmitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK;
    Ns_Conn          *conn;
    Tcl_Obj          *dataObj;
    Ns_ObjvSpec       args[] = {
        {"data", Ns_ObjvObj,  &dataObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK
        || NsConnRequire(interp, NS_CONN_REQUIRE_ALL, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int            size;
        unsigned char *data = Tcl_GetByteArrayFromObj(dataObj, &size);

        if (data != NULL) {
            struct iovec  vbuf;
            Ns_ReturnCode status;

            vbuf.iov_base = (void *)data;
            vbuf.iov_len = (size_t)size;

            status = NsWriterQueue(conn, (size_t)size, NULL, NULL, NS_INVALID_FD,
                                   &vbuf, 1,  NULL, 0, NS_TRUE);
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status == NS_OK ? 1 : 0));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterCheckInputParams -
 *
 *      Helper command for WriterSubmitFileObjCmd and WriterSubmitFilesObjCmd
 *      to check validity of filename, offset and size.
 *
 * Results:
 *      Standard Tcl result. Returns on success also fd and nrbytes.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterCheckInputParams(Tcl_Interp *interp, const char *filenameString,
                       size_t size, off_t offset,
                       int *fdPtr, size_t *nrbytesPtr)
{
    int         result = TCL_OK, rc;
    struct stat st;

    Ns_Log(DriverDebug, "WriterCheckInputParams %s offset %" PROTd " size %" PRIdz,
           filenameString, offset, size);

    /*
     * Use stat() call to obtain information about the actual file to check
     * later the plausibility of the parameters.
     */
    rc = stat(filenameString, &st);
    if (unlikely(rc != 0)) {
        Ns_TclPrintfResult(interp, "file does not exist '%s'", filenameString);
        result = TCL_ERROR;

    } else {
        size_t nrbytes = 0u;
        int    fd;

        /*
         * Try to open the file and check offset and size parameters.
         */
        fd = ns_open(filenameString, O_RDONLY | O_CLOEXEC, 0);

        if (unlikely(fd == NS_INVALID_FD)) {
            Ns_TclPrintfResult(interp, "could not open file '%s'", filenameString);
            result = TCL_ERROR;

        } else if (unlikely(offset > st.st_size) || offset < 0) {
            Ns_TclPrintfResult(interp, "offset must be a positive value less or equal filesize");
            result = TCL_ERROR;

        } else if (size > 0) {
            if (unlikely((off_t)size + offset > st.st_size)) {
                Ns_TclPrintfResult(interp, "offset + size must be less or equal filesize");
                result = TCL_ERROR;
            } else {
                nrbytes = (size_t)size;
            }
        } else {
            nrbytes = (size_t)st.st_size - (size_t)offset;
        }

        /*
         * When an offset is provide, jump to this offset.
         */
        if (offset > 0 && result == TCL_OK) {
            if (ns_lseek(fd, (off_t)offset, SEEK_SET) == -1) {
                Ns_TclPrintfResult(interp, "cannot seek to position %ld", (long)offset);
                result = TCL_ERROR;
            }
        }

        if (result == TCL_OK) {
            *fdPtr = fd;
            *nrbytesPtr = nrbytes;

        } else if (fd != NS_INVALID_FD) {
            /*
             * On invalid parameters, close the fd.
             */
            ns_close(fd);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSubmitFileObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer submitfile" command.
 *      Send the provided file to the client.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSubmitFileObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Conn    *conn;
    char       *fileNameString;
    int         headers = 0;
    Tcl_WideInt offset = 0, size = 0;
    Ns_ObjvValueRange offsetRange = {0, LLONG_MAX};
    Ns_ObjvValueRange sizeRange = {1, LLONG_MAX};
    Ns_ObjvSpec lopts[] = {
        {"-headers",  Ns_ObjvBool,    &headers, INT2PTR(NS_TRUE)},
        {"-offset",   Ns_ObjvMemUnit, &offset,  &offsetRange},
        {"-size",     Ns_ObjvMemUnit, &size,    &sizeRange},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"file",      Ns_ObjvString, &fileNameString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK)
        || NsConnRequire(interp, NS_CONN_REQUIRE_ALL, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else if (unlikely( Ns_ConnSockPtr(conn) == NULL )) {
        Ns_Log(Warning,
               "NsWriterQueue: called without valid sockPtr, maybe connection already closed");
        Ns_TclPrintfResult(interp, "0");
        result = TCL_OK;

    } else {
        size_t      nrbytes = 0u;
        int         fd = NS_INVALID_FD;

        result = WriterCheckInputParams(interp, fileNameString,
                                        (size_t)size, offset,
                                        &fd, &nrbytes);

        if (likely(result == TCL_OK)) {
            Ns_ReturnCode status;

            /*
             *  The caller requested that we build required headers
             */

            if (headers != 0) {
                Ns_ConnSetTypeHeader(conn, Ns_GetMimeType(fileNameString));
            }
            status = NsWriterQueue(conn, nrbytes, NULL, NULL, fd, NULL, 0,  NULL, 0, NS_TRUE);
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status == NS_OK ? 1 : 0));

            if (fd != NS_INVALID_FD) {
                (void) ns_close(fd);
            } else {
                Ns_Log(Warning, "WriterSubmitFileObjCmd called with invalid fd");
            }

        } else if (fd != NS_INVALID_FD) {
            (void) ns_close(fd);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterGetMemunitFromDict --
 *
 *      Helper function to obtain a memory unit from a dict structure,
 *      optionally checking the value range.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      On errors, an error message is left in the interpreter.
 *
 *----------------------------------------------------------------------
 */
static int
WriterGetMemunitFromDict(Tcl_Interp *interp, Tcl_Obj *dictObj, Tcl_Obj *keyObj, Ns_ObjvValueRange *rangePtr,
                         Tcl_WideInt *valuePtr)
{
    Tcl_Obj *intObj = NULL;
    int      result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(dictObj != NULL);
    NS_NONNULL_ASSERT(keyObj != NULL);
    NS_NONNULL_ASSERT(valuePtr != NULL);

    result = Tcl_DictObjGet(interp, dictObj, keyObj, &intObj);
    if (result == TCL_OK && intObj != NULL) {
        result = Ns_TclGetMemUnitFromObj(interp, intObj, valuePtr);
        if (result == TCL_OK && rangePtr != NULL) {
            result = Ns_CheckWideRange(interp, Tcl_GetString(keyObj), rangePtr, *valuePtr);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * WriterSubmitFilesObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer submitfiles" command.  Send the provided files
 *      to the client. "files" are provided as a list of dicts, where every
 *      dict must contain a "filename" element and can contain an "-offset"
 *      and/or a "-length" element.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSubmitFilesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Conn    *conn;
    int         headers = 0, nrFiles;
    Tcl_Obj    *filesObj = NULL, **fileObjv;
    Ns_ObjvSpec lopts[] = {
        {"-headers",  Ns_ObjvBool,    &headers, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"files",     Ns_ObjvObj, &filesObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK)
        || NsConnRequire(interp, NS_CONN_REQUIRE_ALL, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else if (unlikely( Ns_ConnSockPtr(conn) == NULL )) {
        Ns_Log(Warning,
               "NsWriterQueue: called without valid sockPtr, "
               "maybe connection already closed");
        Ns_TclPrintfResult(interp, "0");
        result = TCL_OK;

    } else if (Tcl_ListObjGetElements(interp, filesObj, &nrFiles, &fileObjv) != TCL_OK) {
        Ns_TclPrintfResult(interp, "not a valid list of files: '%s'", Tcl_GetString(filesObj));
        result = TCL_ERROR;

    } else if (nrFiles == 0) {
        Ns_TclPrintfResult(interp, "The provided list has to contain at least one file spec");
        result = TCL_ERROR;

    } else {
        size_t      totalbytes = 0u, i;
        Tcl_Obj    *keys[3], *filenameObj = NULL;
        Ns_FileVec *filebufs;
        const char *firstFilenameString = NULL;
        Ns_ObjvValueRange offsetRange = {0, LLONG_MAX};
        Ns_ObjvValueRange sizeRange = {1, LLONG_MAX};

        filebufs = (Ns_FileVec *)ns_calloc((size_t)nrFiles, sizeof(Ns_FileVec));
        keys[0] = Tcl_NewStringObj("filename", 8);
        keys[1] = Tcl_NewStringObj("-offset", 7);
        keys[2] = Tcl_NewStringObj("-size", 5);

        Tcl_IncrRefCount(keys[0]);
        Tcl_IncrRefCount(keys[1]);
        Tcl_IncrRefCount(keys[2]);

        for (i = 0u; i < (size_t)nrFiles; i++) {
            filebufs[i].fd = NS_INVALID_FD;
        }

        /*
         * Iterate over the list of dicts.
         */
        for (i = 0u; i < (size_t)nrFiles; i++) {
            Tcl_WideInt offset = 0, size = 0;
            int         rc, fd = NS_INVALID_FD;
            const char *filenameString;
            size_t      nrbytes;

            /*
             * Get required "filename" element.
             */
            filenameObj = NULL;
            rc = Tcl_DictObjGet(interp, fileObjv[i], keys[0], &filenameObj);
            if (rc != TCL_OK || filenameObj == NULL) {
                Ns_TclPrintfResult(interp, "missing filename in dict '%s'",
                                   Tcl_GetString(fileObjv[i]));
                result = TCL_ERROR;
                break;
            }

            filenameString = Tcl_GetString(filenameObj);
            if (firstFilenameString == NULL) {
                firstFilenameString = filenameString;
            }

            /*
             * Get optional "-offset" and "-size" elements.
             */
            if (WriterGetMemunitFromDict(interp, fileObjv[i], keys[1], &offsetRange, &offset) != TCL_OK) {
                result = TCL_ERROR;
                break;
            }
            if (WriterGetMemunitFromDict(interp, fileObjv[i], keys[2], &sizeRange, &size) != TCL_OK) {
                result = TCL_ERROR;
                break;
            }

            /*
             * Check validity of the provided values
             */
            result = WriterCheckInputParams(interp, Tcl_GetString(filenameObj),
                                            (size_t)size, (off_t)offset,
                                            &fd, &nrbytes);
            if (result != TCL_OK) {
                break;
            }

            filebufs[i].fd = fd;
            filebufs[i].offset = offset;
            filebufs[i].length = nrbytes;

            totalbytes = totalbytes + (size_t)nrbytes;
        }
        Tcl_DecrRefCount(keys[0]);
        Tcl_DecrRefCount(keys[1]);
        Tcl_DecrRefCount(keys[2]);

        /*
         * If everything is ok, submit the request to the writer queue.
         */
        if (result == TCL_OK) {
            Ns_ReturnCode status;

            if (headers != 0 && firstFilenameString != NULL) {
                Ns_ConnSetTypeHeader(conn, Ns_GetMimeType(firstFilenameString));
            }
            status = NsWriterQueue(conn, totalbytes, NULL, NULL, NS_INVALID_FD, NULL, 0,
                                   filebufs, nrFiles, NS_TRUE);
            /*
             * Provide a soft error like for "ns_writer submitfile".
             */
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status == NS_OK ? 1 : 0));
        }

        /*
         * The NsWriterQueue() API makes the usual duplicates of the file
         * descriptors and the Ns_FileVec structure, so we have to cleanup
         * here.
         */
        for (i = 0u; i < (size_t)nrFiles; i++) {
            if (filebufs[i].fd != NS_INVALID_FD) {
                (void) ns_close(filebufs[i].fd);
            }
        }
        ns_free(filebufs);

    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * WriterListObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer list" command.
 *      List the current writer jobs.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    NsServer    *servPtr = NULL;
    Ns_ObjvSpec  lopts[] = {
        {"-server",  Ns_ObjvServer,    &servPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK)) {
        result = TCL_ERROR;

    } else {
        Tcl_DString       ds, *dsPtr = &ds;
        const Driver     *drvPtr;
        SpoolerQueue     *queuePtr;

        Tcl_DStringInit(dsPtr);

        for (drvPtr = firstDrvPtr; drvPtr != NULL; drvPtr = drvPtr->nextPtr) {
            const DrvWriter *wrPtr;

            /*
             * If server was specified, list only results from this server.
             */
            if (servPtr != NULL && servPtr != drvPtr->servPtr) {
                continue;
            }

            wrPtr = &drvPtr->writer;
            queuePtr = wrPtr->firstPtr;
            while (queuePtr != NULL) {
                const WriterSock *wrSockPtr;

                Ns_MutexLock(&queuePtr->lock);
                wrSockPtr = queuePtr->curPtr;
                while (wrSockPtr != NULL) {
                    char ipString[NS_IPADDR_SIZE];
                    ns_inet_ntop((struct sockaddr *)&(wrSockPtr->sockPtr->sa),
                                 ipString,sizeof(ipString));

                    (void) Ns_DStringNAppend(dsPtr, "{", 1);
                    (void) Ns_DStringAppendTime(dsPtr, &wrSockPtr->startTime);
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, queuePtr->threadName);
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, drvPtr->threadName);
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, NsPoolName(wrSockPtr->poolPtr->pool));
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, ipString);
                    (void) Ns_DStringPrintf(dsPtr, " %d %" PRIdz " %" TCL_LL_MODIFIER "d %d %d ",
                                            wrSockPtr->fd,
                                            wrSockPtr->size,
                                            wrSockPtr->nsent,
                                            wrSockPtr->currentRate,
                                            wrSockPtr->rateLimit);
                    (void) Ns_DStringAppendElement(dsPtr,
                                                   (wrSockPtr->clientData != NULL)
                                                   ? wrSockPtr->clientData
                                                   : NS_EMPTY_STRING);
                    (void) Ns_DStringNAppend(dsPtr, "} ", 2);
                    wrSockPtr = wrSockPtr->nextPtr;
                }
                Ns_MutexUnlock(&queuePtr->lock);
                queuePtr = queuePtr->nextPtr;
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSizeObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer size" command.
 *      Sets or queries size limit for sending via writer.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK;
    Tcl_Obj          *driverObj = NULL;
    Ns_Conn          *conn = NULL;
    Tcl_WideInt       intValue = -1;
    const char       *firstArgString;
    Ns_ObjvValueRange range = {1024, INT_MAX};
    Ns_ObjvSpec   *opts, optsNew[] = {
        {"-driver", Ns_ObjvObj, &driverObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   *args, argsNew[] = {
        {"?value", Ns_ObjvMemUnit, &intValue, &range},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   argsLegacy[] = {
        {"driver", Ns_ObjvObj,     &driverObj, NULL},
        {"?value", Ns_ObjvMemUnit, &intValue, &range},
        {NULL, NULL, NULL, NULL}
    };

    firstArgString = objc > 2 ? Tcl_GetString(objv[2]) : NULL;
    if (firstArgString != NULL) {
        if (*firstArgString != '-'
            && ((objc == 3 && CHARTYPE(digit, *firstArgString) == 0) ||
                objc == 4)) {
            args = argsLegacy;
            opts = NULL;
            Ns_LogDeprecated(objv, objc, "ns_writer size ?-driver drv? ?size?", NULL);
        } else {
            args = argsNew;
            opts = optsNew;
        }
    } else {
        args = argsNew;
        opts = optsNew;
    }

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if ((driverObj == NULL)
               && NsConnRequire(interp, NS_CONN_REQUIRE_ALL, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        DrvWriter *wrPtr;

        if (DriverWriterFromObj(interp, driverObj, conn, &wrPtr) != NS_OK) {
            result = TCL_ERROR;

        } else if (intValue != -1) {
            /*
             * The optional argument was provided.
             */
            wrPtr->writersize = (size_t)intValue;
        }

        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj((int)wrPtr->writersize));
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterStreamingObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer streaming" command.
 *      Sets or queries streaming state of the writer.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterStreamingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                      int objc, Tcl_Obj *const* objv)
{
    int          boolValue = -1, result = TCL_OK;
    Tcl_Obj     *driverObj = NULL;
    Ns_Conn     *conn = NULL;
    const char  *firstArgString;
    Ns_ObjvSpec *opts, optsNew[] = {
        {"-driver", Ns_ObjvObj, &driverObj, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec *args, argsNew[] = {
        {"?value", Ns_ObjvBool, &boolValue, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  argsLegacy[] = {
        {"driver", Ns_ObjvObj,  &driverObj, NULL},
        {"?value", Ns_ObjvBool, &boolValue, NULL},
        {NULL, NULL, NULL, NULL}
    };

    firstArgString = objc > 2 ? Tcl_GetString(objv[2]) : NULL;
    if (firstArgString != NULL) {
        int argValue;
        if (*firstArgString != '-'
            && ((objc == 3 &&  Tcl_ExprBoolean(interp, firstArgString, &argValue) == TCL_OK) ||
                objc == 4)) {
            args = argsLegacy;
            opts = NULL;
            Ns_LogDeprecated(objv, objc, "ns_writer streaming ?-driver drv? ?value?", NULL);
        } else {
            args = argsNew;
            opts = optsNew;
        }
    } else {
        args = argsNew;
        opts = optsNew;
    }

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if ((driverObj == NULL)
               && NsConnRequire(interp, NS_CONN_REQUIRE_ALL, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        DrvWriter *wrPtr;

        if (DriverWriterFromObj(interp, driverObj, conn, &wrPtr) != NS_OK) {
            result = TCL_ERROR;

        } else if (boolValue != -1) {
            /*
             * The optional argument was provided.
             */
            wrPtr->doStream = (boolValue == 1 ? NS_WRITER_STREAM_ACTIVE : NS_WRITER_STREAM_NONE);
        }

        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(wrPtr->doStream == NS_WRITER_STREAM_ACTIVE ? 1 : 0));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclWriterObjCmd --
 *
 *      Implements "ns_writer" command for submitting data to the writer
 *      threads and to configure and query the state of the writer threads at
 *      runtime.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclWriterObjCmd(ClientData clientData, Tcl_Interp *interp,
                  int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"list",       WriterListObjCmd},
        {"size",       WriterSizeObjCmd},
        {"streaming",  WriterStreamingObjCmd},
        {"submit",     WriterSubmitObjCmd},
        {"submitfile", WriterSubmitFileObjCmd},
        {"submitfiles",WriterSubmitFilesObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 *======================================================================
 *  Async (log) writer: Write asynchronously to a disk
 *======================================================================
 */


/*
 *----------------------------------------------------------------------
 *
 * NsAsyncWriterQueueEnable --
 *
 *      Enable async writing and start the AsyncWriterThread if
 *      necessary
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially starting a thread and set "stopped" to NS_FALSE.
 *
 *----------------------------------------------------------------------
 */
void
NsAsyncWriterQueueEnable(void)
{
    if (Ns_ConfigBool(NS_CONFIG_PARAMETERS, "asynclogwriter", NS_FALSE) == NS_TRUE) {
        SpoolerQueue  *queuePtr;

        /*
         * In case, the async writer has not started, the static variable
         * asyncWriter is NULL.
         */
        if (asyncWriter == NULL) {
            Ns_MutexLock(&reqLock);
            if (likely(asyncWriter == NULL)) {
                /*
                 * Allocate and initialize writer thread context.
                 */
                asyncWriter = ns_calloc(1u, sizeof(AsyncWriter));
                Ns_MutexUnlock(&reqLock);
                Ns_MutexSetName2(&asyncWriter->lock, "ns:driver", "async-writer");
                /*
                 * Allocate and initialize a Spooler Queue for this thread.
                 */
                queuePtr = ns_calloc(1u, sizeof(SpoolerQueue));
                Ns_MutexSetName2(&queuePtr->lock, "ns:driver:async-writer", "queue");
                asyncWriter->firstPtr = queuePtr;
                /*
                 * Start the spooler queue
                 */
                SpoolerQueueStart(queuePtr, AsyncWriterThread);

            } else {
                Ns_MutexUnlock(&reqLock);
            }
        }


        assert(asyncWriter != NULL);
        queuePtr = asyncWriter->firstPtr;
        assert(queuePtr != NULL);

        Ns_MutexLock(&queuePtr->lock);
        queuePtr->stopped = NS_FALSE;
        Ns_MutexUnlock(&queuePtr->lock);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsAsyncWriterQueueDisable --
 *
 *      Disable async writing but don't touch the writer thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Disable async writing by setting stopped to 1.
 *
 *----------------------------------------------------------------------
 */
void
NsAsyncWriterQueueDisable(bool shutdown)
{
    if (asyncWriter != NULL) {
        SpoolerQueue *queuePtr = asyncWriter->firstPtr;
        Ns_Time       timeout;

        assert(queuePtr != NULL);

        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, nsconf.shutdowntimeout.sec, nsconf.shutdowntimeout.usec);

        Ns_MutexLock(&queuePtr->lock);
        queuePtr->stopped = NS_TRUE;
        queuePtr->shutdown = shutdown;

        /*
         * Trigger the AsyncWriter Thread to drain the spooler queue.
         */
        SockTrigger(queuePtr->pipe[1]);
        (void)Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, &timeout);

        Ns_MutexUnlock(&queuePtr->lock);

        if (shutdown) {
            ns_free(queuePtr);
            ns_free(asyncWriter);
            asyncWriter = NULL;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsAsyncWrite --
 *
 *      Perform an asynchronous write operation via a writer thread in
 *      case a writer thread is configured and running. The intention
 *      of the asynchronous write operations is to reduce latencies in
 *      connection threads.
 *
 * Results:
 *      NS_OK, when write was performed via writer thread,
 *      NS_ERROR otherwise (but data is written).
 *
 * Side effects:
 *      I/O Operation.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsAsyncWrite(int fd, const char *buffer, size_t nbyte)
{
    Ns_ReturnCode returnCode = NS_OK;

    NS_NONNULL_ASSERT(buffer != NULL);

    /*
     * If the async writer has not started or is deactivated, behave like a
     * ns_write() command. If the ns_write() fails, we can't do much, since
     * the writing of an error message to the log might bring us into an
     * infinite loop. So we print simple to stderr.
     */
    if (asyncWriter == NULL || asyncWriter->firstPtr->stopped) {
        ssize_t written = ns_write(fd, buffer, nbyte);

        if (unlikely(written != (ssize_t)nbyte)) {
            int retries = 100;

            /*
             * Don't go into an infinite loop when multiple subsequent disk
             * write operations return 0 (maybe disk full).
             */
            returnCode = NS_ERROR;
            do {
                if (written < 0) {
                    fprintf(stderr, "error during async write (fd %d): %s\n",
                           fd, strerror(errno));
                    break;
                }
                /*
                 * All partial writes (written >= 0)
                 */
                WriteWarningRaw("partial write", fd, nbyte, written);
                nbyte -= (size_t)written;
                buffer += written;
                written = ns_write(fd, buffer, nbyte);
                if (written == (ssize_t)nbyte) {
                    returnCode = NS_OK;
                    break;
                }
            } while (retries-- > 0);
        }

    } else {
        SpoolerQueue         *queuePtr;
        bool                  trigger = NS_FALSE;
        const AsyncWriteData *wdPtr;
        AsyncWriteData       *newWdPtr;

        /*
         * Allocate a writer cmd and initialize it. In order to provide an
         * interface compatible to ns_write(), we copy the provided data,
         * such it can be freed by the caller. When we would give up the
         * interface, we could free the memory block after writing, and
         * save a malloc/free operation on the data.
         */
        newWdPtr = ns_calloc(1u, sizeof(AsyncWriteData));
        newWdPtr->fd = fd;
        newWdPtr->bufsize = nbyte;
        newWdPtr->data = ns_malloc(nbyte + 1u);
        memcpy(newWdPtr->data, buffer, newWdPtr->bufsize);
        newWdPtr->buf  = newWdPtr->data;
        newWdPtr->size = newWdPtr->bufsize;

        /*
         * Now add new writer socket to the writer thread's queue. In most
         * cases, the queue will be empty.
         */
        queuePtr = asyncWriter->firstPtr;
        assert(queuePtr != NULL);

        Ns_MutexLock(&queuePtr->lock);
        wdPtr = queuePtr->sockPtr;
        if (wdPtr != NULL) {
            newWdPtr->nextPtr = queuePtr->sockPtr;
            queuePtr->sockPtr = newWdPtr;
        } else {
            queuePtr->sockPtr = newWdPtr;
            trigger = NS_TRUE;
        }
        Ns_MutexUnlock(&queuePtr->lock);

        /*
         * Wake up writer thread if desired
         */
        if (trigger) {
            SockTrigger(queuePtr->pipe[1]);
        }
    }

    return returnCode;
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncWriterRelease --
 *
 *      Deallocate write data.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      free memory
 *
 *----------------------------------------------------------------------
 */
static void
AsyncWriterRelease(AsyncWriteData *wdPtr)
{
    NS_NONNULL_ASSERT(wdPtr != NULL);

    ns_free(wdPtr->data);
    ns_free(wdPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncWriterThread --
 *
 *      Thread that implements non-blocking write operations to files
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Write to files.
 *
 *----------------------------------------------------------------------
 */

static void
AsyncWriterThread(void *arg)
{
    SpoolerQueue   *queuePtr = (SpoolerQueue*)arg;
    char            charBuffer[1];
    int             pollTimeout;
    Ns_ReturnCode   status;
    bool            stopping;
    AsyncWriteData *curPtr, *nextPtr, *writePtr;
    PollData        pdata;

    Ns_ThreadSetName("-asynclogwriter%d-", queuePtr->id);
    queuePtr->threadName = Ns_ThreadGetName();

    /*
     * Allocate and initialize controlling variables
     */

    PollCreate(&pdata);
    writePtr = NULL;
    stopping = NS_FALSE;

    /*
     * Loop forever until signaled to shutdown and all
     * connections are complete and gracefully closed.
     */

    while (!stopping) {

        /*
         * Always listen to the trigger pipe. We could as well perform
         * in the writer thread async write operations, but for the
         * effect of reducing latency in connection threads, this is
         * not an issue. To keep things simple, we perform the
         * typically small write operations without testing for POLLOUT.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, queuePtr->pipe[0], (short)POLLIN, NULL);

        if (writePtr == NULL) {
            pollTimeout = 30 * 1000;
        } else {
            pollTimeout = 0;
        }

        /*
         * Wait for data
         */
        /*n =*/ (void) PollWait(&pdata, pollTimeout);

        /*
         * Select and drain the trigger pipe if necessary.
         */
        if (PollIn(&pdata, 0)) {
            if (ns_recv(queuePtr->pipe[0], charBuffer, 1u, 0) != 1) {
                Ns_Fatal("asynclogwriter: trigger ns_recv() failed: %s",
                         ns_sockstrerror(ns_sockerrno));
            }
            if (queuePtr->stopped) {
                /*
                 * Drain the queue from everything
                 */
                for (curPtr = writePtr; curPtr != NULL;  curPtr = curPtr->nextPtr) {
                    ssize_t written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
                    if (unlikely(written != (ssize_t)curPtr->bufsize)) {
                        WriteWarningRaw("drain writer", curPtr->fd, curPtr->bufsize, written);
                    }
                }
                writePtr = NULL;

                for (curPtr = queuePtr->sockPtr; curPtr != NULL;  curPtr = curPtr->nextPtr) {
                    ssize_t written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
                    if (unlikely(written != (ssize_t)curPtr->bufsize)) {
                        WriteWarningRaw("drain queue", curPtr->fd, curPtr->bufsize, written);
                    }
                }
                queuePtr->sockPtr = NULL;

                /*
                 * Notify the caller (normally
                 * NsAsyncWriterQueueDisable()) that we are done
                 */
                Ns_CondBroadcast(&queuePtr->cond);
            }
        }

        /*
         * Write to all available file descriptors
         */

        curPtr = writePtr;
        writePtr = NULL;

        while (curPtr != NULL) {
            ssize_t written;

            nextPtr = curPtr->nextPtr;
            status = NS_OK;

            /*
             * Write the actual data and allow for partial write operations.
             */
            written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
            if (unlikely(written < 0)) {
                status = NS_ERROR;
            } else {
                curPtr->size -= (size_t)written;
                curPtr->nsent += written;
                curPtr->bufsize -= (size_t)written;
                if (curPtr->data != NULL) {
                    curPtr->buf += written;
                }
            }

            if (unlikely(status != NS_OK)) {
                AsyncWriterRelease(curPtr);
                queuePtr->queuesize--;
            } else {

                /*
                 * The write operation was successful. Check if there
                 * is some remaining data to write. If not we are done
                 * with this request can release the write buffer.
                 */
                if (curPtr->size > 0u) {
                    Push(curPtr, writePtr);
                } else {
                    AsyncWriterRelease(curPtr);
                    queuePtr->queuesize--;
                }
            }

            curPtr = nextPtr;
        }


        /*
         * Check for shutdown
         */
        stopping = queuePtr->shutdown;
        if (stopping) {
            curPtr = queuePtr->sockPtr;
            assert(writePtr == NULL);
            while (curPtr != NULL) {
                ssize_t written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
                if (unlikely(written != (ssize_t)curPtr->bufsize)) {
                    WriteWarningRaw("shutdown", curPtr->fd, curPtr->bufsize, written);
                }
                curPtr = curPtr->nextPtr;
            }
        } else {
            /*
             * Add fresh jobs to the writer queue. This means actually to
             * move jobs from queuePtr->sockPtr (kept name for being able
             * to use the same queue as above) to the currently active
             * jobs in queuePtr->curPtr.
             */
            Ns_MutexLock(&queuePtr->lock);
            curPtr = queuePtr->sockPtr;
            queuePtr->sockPtr = NULL;
            while (curPtr != NULL) {
                nextPtr = curPtr->nextPtr;
                Push(curPtr, writePtr);
                queuePtr->queuesize++;
                curPtr = nextPtr;
            }
            queuePtr->curPtr = writePtr;
            Ns_MutexUnlock(&queuePtr->lock);
        }

    }

    PollFree(&pdata);

    queuePtr->stopped = NS_TRUE;
    Ns_Log(Notice, "exiting");

}

/*
 *----------------------------------------------------------------------
 *
 * AsyncLogfileWriteObjCmd -
 *
 *      Implements "ns_asynclogfile write" command.  Write to a file
 *      descriptor via async writer thread.  The command handles partial write
 *      operations internally.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AsyncLogfileWriteObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK, binary = (int)NS_FALSE, sanitize;
    Tcl_Obj          *stringObj;
    int               fd = 0;
    Ns_ObjvValueRange fd_range = {0, INT_MAX};
    Ns_ObjvValueRange sanitize_range = {0, 2};
    Ns_ObjvSpec opts[] = {
        {"-binary",    Ns_ObjvBool, &binary,   INT2PTR(NS_TRUE)},
        {"-sanitize",  Ns_ObjvInt,  &sanitize, &sanitize_range},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"fd",     Ns_ObjvInt, &fd,        &fd_range},
        {"buffer", Ns_ObjvObj, &stringObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    /*
     * Take the config value as default for "-sanitize", but let the used
     * override it on a per-case basis.
     */
    sanitize = nsconf.sanitize_logfiles;

    if (unlikely(Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK)) {
        result = TCL_ERROR;

    } else {
        const char   *buffer;
        int           length;
        Ns_ReturnCode rc;

        if (binary == (int)NS_TRUE || NsTclObjIsByteArray(stringObj)) {
            buffer = (const char *) Tcl_GetByteArrayFromObj(stringObj, &length);
        } else {
            buffer = Tcl_GetStringFromObj(stringObj, &length);
        }
        if (length > 0) {
            if (sanitize > 0) {
                Tcl_DString ds;
                bool        lastCharNewline = (buffer[length-1] == '\n');

                Tcl_DStringInit(&ds);
                if (lastCharNewline) {
                    length --;
                }
                Ns_DStringAppendPrintable(&ds, sanitize == 2, buffer, (size_t)length);
                if (lastCharNewline) {
                    Tcl_DStringAppend(&ds, "\n", 1);
                }
                rc = NsAsyncWrite(fd, ds.string, (size_t)ds.length);
                Tcl_DStringFree(&ds);

            } else {
                rc = NsAsyncWrite(fd, buffer, (size_t)length);
            }

            if (rc != NS_OK) {
                Ns_TclPrintfResult(interp, "ns_asynclogfile: error during write operation on fd %d: %s",
                                   fd, Tcl_PosixError(interp));
                result = TCL_ERROR;
            }
        } else {
            result = TCL_OK;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncLogfileOpenObjCmd -
 *
 *      Implements "ns_asynclogfile open" command.  The command opens a
 *      write-only log file and return a thread-shareable handle (actually a
 *      numeric file descriptor) which can be used in subsequent "write" or
 *      "close" operations.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AsyncLogfileOpenObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    unsigned int flags = O_APPEND;
    char        *fileNameString;
    Tcl_Obj     *flagsObj = NULL;
    Ns_ObjvTable flagTable[] = {
        {"APPEND", O_APPEND},
        {"EXCL",   O_EXCL},
#ifdef O_DSYNC
        {"DSYNC",  O_DSYNC},
#endif
#ifdef O_SYNC
        {"SYNC",   O_SYNC},
#endif
        {"TRUNC",  O_TRUNC},
        {NULL,     0u}
    };
    Ns_ObjvSpec args[] = {
        {"filename", Ns_ObjvString, &fileNameString, NULL},
        {"?flags", Ns_ObjvObj, &flagsObj, NULL},
        //{"mode", Ns_ObjvString, &mode, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK)) {
        result = TCL_ERROR;

    } else if (flagsObj != NULL) {
        Tcl_Obj **ov;
        int       oc;

        result = Tcl_ListObjGetElements(interp, flagsObj, &oc, &ov);
        if (result == TCL_OK && oc > 0) {
            int i, opt;

            flags = 0u;
            for (i = 0; i < oc; i++) {
                result = Tcl_GetIndexFromObjStruct(interp, ov[i], flagTable,
                                                   (int)sizeof(flagTable[0]),
                                                   "flag", 0, &opt);
                if (result != TCL_OK) {
                    break;
                } else {
                    flags = flagTable[opt].value;
                }
            }
        }
    }

    if (result == TCL_OK) {
        int fd;

        fd = ns_open(fileNameString, (int)(O_CREAT | O_WRONLY | O_CLOEXEC | flags), 0644);

        if (unlikely(fd == NS_INVALID_FD)) {
            Ns_TclPrintfResult(interp, "could not open file '%s': %s",
                               fileNameString, Tcl_PosixError(interp));
            result = TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(fd));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncLogfileCloseObjCmd -
 *
 *      Implements "ns_asynclogfile close" command.  Close the logfile
 *      previously created via "ns_asynclogfile open".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AsyncLogfileCloseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int               fd, result = TCL_OK;
    Ns_ObjvValueRange range = {0, INT_MAX};
    Ns_ObjvSpec args[] = {
        {"fd", Ns_ObjvInt, &fd, &range},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK)) {
        result = TCL_ERROR;

    } else {
        int rc = ns_close(fd);

        if (rc != 0) {
            Ns_TclPrintfResult(interp, "could not close fd %d: %s",
                               fd, Tcl_PosixError(interp));
            result = TCL_ERROR;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAsyncLogfileObjCmd -
 *
 *      Wrapper for "ns_asynclogfile open|write|close" commands.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclAsyncLogfileObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"open",  AsyncLogfileOpenObjCmd},
        {"write", AsyncLogfileWriteObjCmd},
        {"close", AsyncLogfileCloseObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}



/*
 *----------------------------------------------------------------------
 *
 * LookupDriver --
 *
 *      Find a matching driver for the specified protocol and optionally the
 *      specified driver name.
 *
 * Results:
 *      Driver pointer or NULL on failure.
 *
 * Side effects:
 *      When no driver is found, an error is left in the interp result.
 *
 *----------------------------------------------------------------------
 */

static Driver *
LookupDriver(Tcl_Interp *interp, const char* protocol, const char *driverName)
{
    Driver *drvPtr;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(protocol != NULL);

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        Ns_Log(DriverDebug, "... check Driver proto <%s> server %s name %s location %s",
               drvPtr->protocol, drvPtr->server, drvPtr->threadName, drvPtr->location);

        if (STREQ(drvPtr->protocol, protocol)) {
            if (driverName == NULL) {
                /*
                 * If there is no driver name given, take the first driver
                 * with the matching protocol.
                 */
                break;
            } else if (STREQ(drvPtr->moduleName, driverName)) {
                /*
                 * The driver name (name of the loaded module) is equal
                 */
                break;
            }
        }
    }

    if (drvPtr == NULL) {
        if (driverName != NULL) {
            Ns_TclPrintfResult(interp, "no driver for protocol '%s' & driver name '%s' found", protocol, driverName);
        } else {
            Ns_TclPrintfResult(interp, "no driver for protocol '%s' found", protocol);
        }
    }
    return drvPtr;
}
/*
 *----------------------------------------------------------------------
 *
 * NSDriverClientOpen --
 *
 *      Open a client HTTP connection using the driver interface
 *
 * Results:
 *      Tcl return code.
 *
 * Side effects:
 *      Opening a connection
 *
 *----------------------------------------------------------------------
 */

int
NSDriverClientOpen(Tcl_Interp *interp, const char *driverName,
                   const char *url, const char *httpMethod, const char *version,
                   const Ns_Time *timeoutPtr, Sock **sockPtrPtr)
{
    char   *protocol, *host, *portString, *path, *tail, *url2;
    int     result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(httpMethod != NULL);
    NS_NONNULL_ASSERT(version != NULL);
    NS_NONNULL_ASSERT(sockPtrPtr != NULL);

    url2 = ns_strdup(url);

    /*
     * We need here a fully qualified URL, otherwise raise an error
     */
    if (unlikely(Ns_ParseUrl(url2, &protocol, &host, &portString, &path, &tail) != NS_OK)
        || protocol == NULL || host == NULL || path == NULL || tail == NULL) {
        Ns_Log(Notice, "driver: invalid URL '%s' passed to NSDriverClientOpen", url2);
        result = TCL_ERROR;

    } else {
        Driver        *drvPtr;
        unsigned short portNr = 0u; /* make static checker happy */

        assert(protocol != NULL);
        assert(host != NULL);
        assert(path != NULL);
        assert(tail != NULL);

        /*
         * Find a matching driver for the specified protocol and optionally
         * the specified driver name.
         */
        drvPtr = LookupDriver(interp, protocol, driverName);
        if (drvPtr == NULL) {
            result = TCL_ERROR;

        } else if (portString != NULL) {
            portNr = (unsigned short) strtol(portString, NULL, 10);

        } else if (drvPtr->defport != 0u) {
            /*
             * Get the default port from the driver structure;
             */
            portNr = drvPtr->defport;

        } else {
            Ns_TclPrintfResult(interp, "no default port for protocol '%s' defined", protocol);
            result = TCL_ERROR;
        }

        if (result == TCL_OK) {
            NS_SOCKET     sock;
            Ns_ReturnCode status;

            sock = Ns_SockTimedConnect2(host, portNr, NULL, 0u, timeoutPtr, &status);

            if (sock == NS_INVALID_SOCKET) {
                Ns_SockConnectError(interp, host, portNr, status);
                result = TCL_ERROR;

            } else {
                const char    *query;
                Tcl_DString    ds, *dsPtr = &ds;
                Request       *reqPtr;
                Sock          *sockPtr;

                assert(drvPtr != NULL);

                sockPtr = SockNew(drvPtr);
                sockPtr->sock = sock;
                sockPtr->servPtr  = drvPtr->servPtr;
                if (sockPtr->servPtr == NULL) {
                    const NsInterp *itPtr = NsGetInterpData(interp);

                    sockPtr->servPtr = itPtr->servPtr;
                }

                RequestNew(sockPtr);

                Ns_GetTime(&sockPtr->acceptTime);
                reqPtr = sockPtr->reqPtr;

                Tcl_DStringInit(dsPtr);
                Ns_DStringAppend(dsPtr, httpMethod);
                Ns_StrToUpper(Ns_DStringValue(dsPtr));
                Tcl_DStringAppend(dsPtr, " /", 2);
                if (*path != '\0') {
                    if (*path == '/') {
                        path ++;
                    }
                    Tcl_DStringAppend(dsPtr, path, -1);
                    Tcl_DStringAppend(dsPtr, "/", 1);
                }
                Tcl_DStringAppend(dsPtr, tail, -1);
                Tcl_DStringAppend(dsPtr, " HTTP/", 6);
                Tcl_DStringAppend(dsPtr, version, -1);

                reqPtr->request.line = Ns_DStringExport(dsPtr);
                reqPtr->request.method = ns_strdup(httpMethod);
                reqPtr->request.protocol = ns_strdup(protocol);
                reqPtr->request.host = ns_strdup(host);
                query = strchr(tail, INTCHAR('?'));
                if (query != NULL) {
                    reqPtr->request.query = ns_strdup(query+1);
                } else {
                    reqPtr->request.query = NULL;
                }
                /*Ns_Log(Notice, "REQUEST LINE <%s> query <%s>", reqPtr->request.line, reqPtr->request.query);*/

                *sockPtrPtr = sockPtr;
            }
        }
    }
    ns_free(url2);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NSDriverSockNew --
 *
 *      Create a Sock structure based on the driver interface
 *
 * Results:
 *      Tcl return code.
 *
 * Side effects:
 *      Accepting a connection
 *
 *----------------------------------------------------------------------
 */

int
NSDriverSockNew(Tcl_Interp *interp, NS_SOCKET sock,
                 const char *protocol, const char *driverName, const char *methodName,
                 Sock **sockPtrPtr)
{
    int     result = TCL_OK;
    Driver *drvPtr;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(protocol != NULL);
    NS_NONNULL_ASSERT(methodName != NULL);
    NS_NONNULL_ASSERT(sockPtrPtr != NULL);

    drvPtr = LookupDriver(interp, protocol, driverName);
    if (drvPtr == NULL) {
        result = TCL_ERROR;
    } else {
        Sock        *sockPtr;
        Tcl_DString  ds, *dsPtr = &ds;
        Request     *reqPtr;

        sockPtr = SockNew(drvPtr);
        sockPtr->servPtr = drvPtr->servPtr;
        sockPtr->sock = sock;

        RequestNew(sockPtr); // not sure if needed
        // peerAddr is missing

        Ns_GetTime(&sockPtr->acceptTime);
        reqPtr = sockPtr->reqPtr;

        Tcl_DStringInit(dsPtr);
        Ns_DStringAppend(dsPtr, methodName);
        Ns_StrToUpper(Ns_DStringValue(dsPtr));

        reqPtr->request.line = Ns_DStringExport(dsPtr);
        reqPtr->request.method = ns_strdup(methodName);
        reqPtr->request.protocol = ns_strdup(protocol);
        reqPtr->request.host = NULL;
        reqPtr->request.query = NULL;
        /* Ns_Log(Notice, "REQUEST LINE <%s>", reqPtr->request.line);*/

        *sockPtrPtr = sockPtr;
    }

    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */

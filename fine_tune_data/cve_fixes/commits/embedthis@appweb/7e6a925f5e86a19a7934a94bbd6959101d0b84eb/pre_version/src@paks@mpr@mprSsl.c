/* Embedthis MPR SSL Source */




/********* Start of file src/ssl/est.c ************/


/*
    est.c - Embedded Secure Transport

    Individual sockets are not thread-safe. Must only be used by a single thread.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "mpr.h"

#if ME_COM_EST
 /*
    Indent to bypass MakeMe dependencies
  */
 #include    "est.h"

/************************************* Defines ********************************/
/*
    Per-route SSL configuration
 */
typedef struct EstConfig {
    rsa_context     rsa;                /* RSA context */
    x509_cert       cert;               /* Certificate (own) */
    x509_cert       ca;                 /* Certificate authority bundle to verify peer */
    int             *ciphers;           /* Set of acceptable ciphers */
    char            *dhKey;             /* DH keys */
} EstConfig;

/*
    Per socket state
 */
typedef struct EstSocket {
    MprSocket       *sock;              /* MPR socket object */
    MprTicks        started;            /* When connection begun */
    EstConfig       *cfg;               /* Configuration */
    havege_state    hs;                 /* Random HAVEGE state */
    ssl_context     ctx;                /* SSL state */
    ssl_session     session;            /* SSL sessions */
} EstSocket;

static MprSocketProvider *estProvider;  /* EST socket provider */
static EstConfig *defaultEstConfig;     /* Default configuration */

/*
    Regenerate using: dh_genprime
    Generated on 1/1/2014
 */
static char *dhG = "4";
static char *dhKey =
    "E4004C1F94182000103D883A448B3F80"
    "2CE4B44A83301270002C20D0321CFD00"
    "11CCEF784C26A400F43DFB901BCA7538"
    "F2C6B176001CF5A0FD16D2C48B1D0C1C"
    "F6AC8E1DA6BCC3B4E1F96B0564965300"
    "FFA1D0B601EB2800F489AA512C4B248C"
    "01F76949A60BB7F00A40B1EAB64BDD48" 
    "E8A700D60B7F1200FA8E77B0A979DABF";

/*
    Thread-safe session list
 */
static MprList *sessions;

/***************************** Forward Declarations ***************************/

static void     closeEst(MprSocket *sp, bool gracefully);
static void     disconnectEst(MprSocket *sp);
static void     estTrace(void *fp, int level, char *str);
static int      handshakeEst(MprSocket *sp);
static char     *getEstState(MprSocket *sp);
static void     manageEstConfig(EstConfig *cfg, int flags);
static void     manageEstProvider(MprSocketProvider *provider, int flags);
static void     manageEstSocket(EstSocket *ssp, int flags);
static ssize    readEst(MprSocket *sp, void *buf, ssize len);
static int      upgradeEst(MprSocket *sp, MprSsl *sslConfig, cchar *peerName);
static ssize    writeEst(MprSocket *sp, cvoid *buf, ssize len);

static int      setSession(ssl_context *ssl);
static int      getSession(ssl_context *ssl);

/************************************* Code ***********************************/
/*
    Create the EST module. This is called only once
 */
PUBLIC int mprCreateEstModule()
{
    if ((estProvider = mprAllocObj(MprSocketProvider, manageEstProvider)) == NULL) {
        return MPR_ERR_MEMORY;
    }
    estProvider->upgradeSocket = upgradeEst;
    estProvider->closeSocket = closeEst;
    estProvider->disconnectSocket = disconnectEst;
    estProvider->readSocket = readEst;
    estProvider->writeSocket = writeEst;
    estProvider->socketState = getEstState;
    mprAddSocketProvider("est", estProvider);
    sessions = mprCreateList(0, 0);

    if ((defaultEstConfig = mprAllocObj(EstConfig, manageEstConfig)) == 0) {
        return MPR_ERR_MEMORY;
    }
    defaultEstConfig->dhKey = dhKey;
    return 0;
}


static void manageEstProvider(MprSocketProvider *provider, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(provider->name);
        mprMark(defaultEstConfig);
        mprMark(sessions);

    } else if (flags & MPR_MANAGE_FREE) {
        defaultEstConfig = 0;
        sessions = 0;
    }
}


static void manageEstConfig(EstConfig *cfg, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        ;

    } else if (flags & MPR_MANAGE_FREE) {
        rsa_free(&cfg->rsa);
        x509_free(&cfg->cert);
        x509_free(&cfg->ca);
        free(cfg->ciphers);
    }
}


/*
    Destructor for an EstSocket object
 */
static void manageEstSocket(EstSocket *est, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(est->cfg);
        mprMark(est->sock);

    } else if (flags & MPR_MANAGE_FREE) {
        ssl_free(&est->ctx);
    }
}


static void closeEst(MprSocket *sp, bool gracefully)
{
    EstSocket       *est;

    est = sp->sslSocket;
    lock(sp);
    sp->service->standardProvider->closeSocket(sp, gracefully);
    if (!(sp->flags & MPR_SOCKET_EOF)) {
        ssl_close_notify(&est->ctx);
    }
    unlock(sp);
}


/*
    Upgrade a standard socket to use TLS
 */
static int upgradeEst(MprSocket *sp, MprSsl *ssl, cchar *peerName)
{
    EstSocket   *est;
    EstConfig   *cfg;
    int         verifyMode;

    assert(sp);

    if (ssl == 0) {
        ssl = mprCreateSsl(sp->flags & MPR_SOCKET_SERVER);
    }
    if ((est = (EstSocket*) mprAllocObj(EstSocket, manageEstSocket)) == 0) {
        return MPR_ERR_MEMORY;
    }
    est->sock = sp;
    sp->sslSocket = est;
    sp->ssl = ssl;
    verifyMode = ssl->verifyPeer ? SSL_VERIFY_OPTIONAL : SSL_VERIFY_NO_CHECK;

    lock(ssl);
    if (ssl->config && !ssl->changed) {
        est->cfg = cfg = ssl->config;
    } else {
        ssl->changed = 0;

        /*
            One time setup for the SSL configuration for this MprSsl
         */
        if ((cfg = mprAllocObj(EstConfig, manageEstConfig)) == 0) {
            unlock(ssl);
            return MPR_ERR_MEMORY;
        }
        if (ssl->certFile) {
            /*
                Load a PEM format certificate file
             */
            if (x509parse_crtfile(&cfg->cert, (char*) ssl->certFile) != 0) {
                sp->errorMsg = sfmt("Unable to parse certificate %s", ssl->certFile); 
                unlock(ssl);
                return MPR_ERR_CANT_READ;
            }
        }
        if (ssl->keyFile) {
            /*
                Load a decrypted PEM format private key
                Last arg is password if you need to use an encrypted private key
             */
            if (x509parse_keyfile(&cfg->rsa, (char*) ssl->keyFile, 0) != 0) {
                sp->errorMsg = sfmt("Unable to parse key file %s", ssl->keyFile); 
                unlock(ssl);
                return MPR_ERR_CANT_READ;
            }
        }
        if (verifyMode != SSL_VERIFY_NO_CHECK) {
            if (!ssl->caFile) {
                sp->errorMsg = sclone("No defined certificate authority file");
                unlock(ssl);
                return MPR_ERR_CANT_READ;
            }
            if (x509parse_crtfile(&cfg->ca, (char*) ssl->caFile) != 0) {
                sp->errorMsg = sfmt("Unable to open or parse certificate authority file %s", ssl->caFile); 
                unlock(ssl);
                return MPR_ERR_CANT_READ;
            }
        }
        est->cfg = ssl->config = cfg;
        cfg->dhKey = defaultEstConfig->dhKey;
        cfg->ciphers = ssl_create_ciphers(ssl->ciphers);
    }
    unlock(ssl);

    //  TODO - convert to proper entropy source API
    //  TODO - cannot put this in cfg yet as it is not thread safe
    ssl_free(&est->ctx);
    havege_init(&est->hs);
    ssl_init(&est->ctx);
    ssl_set_endpoint(&est->ctx, sp->flags & MPR_SOCKET_SERVER ? SSL_IS_SERVER : SSL_IS_CLIENT);
    ssl_set_authmode(&est->ctx, verifyMode);
    ssl_set_rng(&est->ctx, havege_rand, &est->hs);
    ssl_set_dbg(&est->ctx, estTrace, NULL);
    ssl_set_bio(&est->ctx, net_recv, &sp->fd, net_send, &sp->fd);

    //  TODO - better if the API took a handle (est)
    ssl_set_scb(&est->ctx, getSession, setSession);
    ssl_set_ciphers(&est->ctx, cfg->ciphers);

    /* FUTURE - set protocol versions */

    ssl_set_session(&est->ctx, 1, 0, &est->session);
    memset(&est->session, 0, sizeof(ssl_session));

    ssl_set_ca_chain(&est->ctx, ssl->caFile ? &cfg->ca : NULL, (char*) peerName);
    if (ssl->keyFile && ssl->certFile) {
        ssl_set_own_cert(&est->ctx, &cfg->cert, &cfg->rsa);
    }
    ssl_set_dh_param(&est->ctx, dhKey, dhG);
    est->started = mprGetTicks();

    if (handshakeEst(sp) < 0) {
        return -1;
    }
    return 0;
}


static void disconnectEst(MprSocket *sp)
{
    sp->service->standardProvider->disconnectSocket(sp);
}


/*
    Initiate or continue SSL handshaking with the peer. This routine does not block.
    Return -1 on errors, 0 incomplete and awaiting I/O, 1 if successful
 */
static int handshakeEst(MprSocket *sp)
{
    EstSocket   *est;
    char        cbuf[5120];
    int         rc, vrc;

    est = (EstSocket*) sp->sslSocket;
    assert(!(est->ctx.state == SSL_HANDSHAKE_OVER));
    rc = 0;

    sp->flags |= MPR_SOCKET_HANDSHAKING;
    while (est->ctx.state != SSL_HANDSHAKE_OVER && (rc = ssl_handshake(&est->ctx)) != 0) {
        if (rc == EST_ERR_NET_TRY_AGAIN) {
            if (!mprGetSocketBlockingMode(sp)) {
                return 0;
            }
            continue;
        }
        /* Error */
        break;
    }
    sp->flags &= ~MPR_SOCKET_HANDSHAKING;

    /*
        Get peer details
     */
    if (est->ctx.peer_cn) {
        sp->peerName = sclone(est->ctx.peer_cn);
    }
    sp->cipher = sclone(ssl_get_cipher(&est->ctx));
    if (est->ctx.peer_cert && rc == 0) {
        x509parse_dn_gets("", cbuf, sizeof(cbuf), &est->ctx.peer_cert->subject);
        sp->peerCert = sclone(cbuf);
        x509parse_dn_gets("", cbuf, sizeof(cbuf), &est->ctx.peer_cert->issuer);
        sp->peerCertIssuer = sclone(cbuf);
    }

    /*
        Analyze the handshake result
     */
    if (rc < 0) {
        //  TODO - more codes here or have est set a textual message (better)
        if (rc == EST_ERR_SSL_PRIVATE_KEY_REQUIRED && !(sp->ssl->keyFile || sp->ssl->certFile)) {
            sp->errorMsg = sclone("Peer requires a certificate");
        } else {
            sp->errorMsg = sfmt("Cannot handshake: error -0x%x", -rc);
        }
        sp->flags |= MPR_SOCKET_EOF;
        errno = EPROTO;
        return -1;
    } 

    if ((vrc = ssl_get_verify_result(&est->ctx)) != 0) {
        if (vrc & BADCERT_EXPIRED) {
            sp->errorMsg = sclone("Certificate expired");

        } else if (vrc & BADCERT_REVOKED) {
            sp->errorMsg = sclone("Certificate revoked");

        } else if (vrc & BADCERT_CN_MISMATCH) {
            sp->errorMsg = sclone("Certificate common name mismatch");

        } else if (vrc & BADCERT_NOT_TRUSTED) {
            if (vrc & BADCERT_SELF_SIGNED) {
                sp->errorMsg = sclone("Self-signed certificate");
            } else {
                sp->errorMsg = sclone("Certificate not trusted");
            }
            if (!sp->ssl->verifyIssuer) {
                vrc = 0;
            }

        } else {
            if (est->ctx.client_auth && !sp->ssl->certFile) {
                sp->errorMsg = sclone("Server requires a client certificate");
            } else if (rc == EST_ERR_NET_CONN_RESET) {
                sp->errorMsg = sclone("Peer disconnected");
            } else {
                sp->errorMsg = sfmt("Cannot handshake: error -0x%x", -rc);
            }
        }
    }
    if (sp->ssl->verifyPeer && vrc != 0) {
        if (!est->ctx.peer_cert) {
            sp->errorMsg = sclone("Peer did not provide a certificate");
        }
        sp->flags |= MPR_SOCKET_EOF;
        errno = EPROTO;
        return -1;
    }
    sp->secured = 1;
    return 1;
}


/*
    Return the number of bytes read. Return -1 on errors and EOF. Distinguish EOF via mprIsSocketEof.
    If non-blocking, may return zero if no data or still handshaking.
 */
static ssize readEst(MprSocket *sp, void *buf, ssize len)
{
    EstSocket   *est;
    int         rc;

    est = (EstSocket*) sp->sslSocket;
    assert(est);
    assert(est->cfg);

    if (sp->fd == INVALID_SOCKET) {
        return -1;
    }
    if (est->ctx.state != SSL_HANDSHAKE_OVER) {
        if ((rc = handshakeEst(sp)) <= 0) {
            return rc;
        }
    }
    while (1) {
        rc = ssl_read(&est->ctx, buf, (int) len);
        mprDebug("debug mpr ssl est", 5, "ssl_read %d", rc);
        if (rc < 0) {
            if (rc == EST_ERR_NET_TRY_AGAIN)  {
                rc = 0;
                break;
            } else if (rc == EST_ERR_SSL_PEER_CLOSE_NOTIFY) {
                mprDebug("debug mpr ssl est", 5, "connection was closed gracefully\n");
                sp->flags |= MPR_SOCKET_EOF;
                return -1;
            } else if (rc == EST_ERR_NET_CONN_RESET) {
                mprDebug("debug mpr ssl est", 5, "connection reset");
                sp->flags |= MPR_SOCKET_EOF;
                return -1;
            } else {
                mprDebug("debug mpr ssl est", 4, "read error -0x%x", -rc);
                sp->flags |= MPR_SOCKET_EOF;
                return -1;
            }
        }
        break;
    }
    mprHiddenSocketData(sp, ssl_get_bytes_avail(&est->ctx), MPR_READABLE);
    return rc;
}


/*
    Write data. Return the number of bytes written or -1 on errors or socket closure.
    If non-blocking, may return zero if no data or still handshaking.
 */
static ssize writeEst(MprSocket *sp, cvoid *buf, ssize len)
{
    EstSocket   *est;
    ssize       totalWritten;
    int         rc;

    est = (EstSocket*) sp->sslSocket;
    if (len <= 0) {
        assert(0);
        return -1;
    }
    if (est->ctx.state != SSL_HANDSHAKE_OVER) {
        if ((rc = handshakeEst(sp)) <= 0) {
            return rc;
        }
    }
    totalWritten = 0;
    rc = 0;
    do {
        rc = ssl_write(&est->ctx, (uchar*) buf, (int) len);
        mprDebug("debug mpr ssl est", 5, "written %d, requested len %zd", rc, len);
        if (rc <= 0) {
            if (rc == EST_ERR_NET_TRY_AGAIN) {                                                          
                break;
            }
            if (rc == EST_ERR_NET_CONN_RESET) {                                                         
                mprDebug("debug mpr ssl est", 5, "ssl_write peer closed");
                return -1;
            } else {
                mprDebug("debug mpr ssl est", 5, "ssl_write failed rc -0x%x", -rc);
                return -1;
            }
        } else {
            totalWritten += rc;
            buf = (void*) ((char*) buf + rc);
            len -= rc;
            mprDebug("debug mpr ssl est", 5, "write: len %zd, written %d, total %zd", len, rc, totalWritten);
        }
    } while (len > 0);

    mprHiddenSocketData(sp, est->ctx.out_left, MPR_WRITABLE);

    if (totalWritten == 0 && rc == EST_ERR_NET_TRY_AGAIN) {                                                          
        mprSetError(EAGAIN);
        return -1;
    }
    return totalWritten;
}


static char *getEstState(MprSocket *sp)
{
    EstSocket       *est;
    ssl_context     *ctx;
    MprBuf          *buf;
    char            *ownPrefix, *peerPrefix;
    char            cbuf[5120];

    if ((est = sp->sslSocket) == 0) {
        return 0;
    }
    ctx = &est->ctx;
    buf = mprCreateBuf(0, 0);
    mprPutToBuf(buf, "PROVIDER=est,CIPHER=%s,", ssl_get_cipher(ctx));

    mprPutToBuf(buf, "PEER=\"%s\",", est->ctx.peer_cn);
    if (ctx->peer_cert) {
        peerPrefix = sp->acceptIp ? "CLIENT_" : "SERVER_";
        x509parse_cert_info(peerPrefix, cbuf, sizeof(cbuf), ctx->peer_cert);
        mprPutStringToBuf(buf, cbuf);
    } else {
        mprPutToBuf(buf, "%s=\"none\",", sp->acceptIp ? "CLIENT_CERT" : "SERVER_CERT");
    }
    if (ctx->own_cert) {
        ownPrefix =  sp->acceptIp ? "SERVER_" : "CLIENT_";
        x509parse_cert_info(ownPrefix, cbuf, sizeof(cbuf), ctx->own_cert);
        mprPutStringToBuf(buf, cbuf);
    }
    return mprGetBufStart(buf);
}


/*
    Thread-safe session management
 */
static int getSession(ssl_context *ssl)
{
    ssl_session     *session;
    time_t          t;
    int             next;

    t = time(NULL);
    if (!ssl->resume) {
        return 1;
    }
    for (ITERATE_ITEMS(sessions, session, next)) {
        if (ssl->timeout && (t - session->start) > ssl->timeout) {
            continue;
        }
        if (ssl->session->cipher != session->cipher || ssl->session->length != session->length) {
            continue;
        }
        if (memcmp(ssl->session->id, session->id, session->length) != 0) {
            continue;
        }
        memcpy(ssl->session->master, session->master, sizeof(ssl->session->master));
        return 0;
    }
    return 1;
}


static int setSession(ssl_context *ssl)
{
    time_t          t;
    ssl_session     *session;
    int             next;

    t = time(NULL);
    for (ITERATE_ITEMS(sessions, session, next)) {
        if (ssl->timeout != 0 && (t - session->start) > ssl->timeout) {
            /* expired, reuse this slot */
            break;  
        }
        if (memcmp(ssl->session->id, session->id, session->length) == 0) {
            /* client reconnected */
            break;  
        }
    }
    if (session == NULL) {
        if ((session = mprAlloc(sizeof(ssl_session))) == 0) {
            return 1;
        }
        mprAddItem(sessions, session);
    }
    memcpy(session, ssl->session, sizeof(ssl_session));
    return 0;
}


static void estTrace(void *fp, int level, char *str)
{
    level += 3;
    if (level <= MPR->logLevel) {
        mprLog("info est", level, "%s: %d: %s", MPR->name, level, str);
    }
}

#else
void estDummy() {}
#endif /* ME_COM_EST */

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



/********* Start of file src/ssl/openssl.c ************/


/*
    openssl.c - Support for secure sockets via OpenSSL

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "mpr.h"

#if ME_COM_OPENSSL

/* Clashes with WinCrypt.h */
#undef OCSP_RESPONSE

 /*
    Indent includes to bypass MakeMe dependencies
  */
 #include    <openssl/ssl.h>
 #include    <openssl/evp.h>
 #include    <openssl/rand.h>
 #include    <openssl/err.h>
 #include    <openssl/dh.h>

/************************************* Defines ********************************/

typedef struct OpenConfig {
    SSL_CTX         *context;
    RSA             *rsaKey512;
    RSA             *rsaKey1024;
    DH              *dhKey512;
    DH              *dhKey1024;
} OpenConfig;

typedef struct OpenSocket {
    MprSocket       *sock;
    OpenConfig      *cfg;
    char            *requiredPeerName;
    SSL             *handle;
    BIO             *bio;
} OpenSocket;

typedef struct RandBuf {
    MprTime     now;
    int         pid;
} RandBuf;

static int      numLocks;
static MprMutex **olocks;
static MprSocketProvider *openProvider;
static OpenConfig *defaultOpenConfig;

struct CRYPTO_dynlock_value {
    MprMutex    *mutex;
};
typedef struct CRYPTO_dynlock_value DynLock;

/***************************** Forward Declarations ***************************/

static void     closeOss(MprSocket *sp, bool gracefully);
static int      checkCert(MprSocket *sp);
static int      configureCertificateFiles(MprSsl *ssl, SSL_CTX *ctx, char *key, char *cert);
static OpenConfig *createOpenSslConfig(MprSocket *sp);
static DH       *dhCallback(SSL *ssl, int isExport, int keyLength);
static void     disconnectOss(MprSocket *sp);
static ssize    flushOss(MprSocket *sp);
static char     *getOssState(MprSocket *sp);
static char     *getOssError(MprSocket *sp);
static void     manageOpenConfig(OpenConfig *cfg, int flags);
static void     manageOpenProvider(MprSocketProvider *provider, int flags);
static void     manageOpenSocket(OpenSocket *ssp, int flags);
static ssize    readOss(MprSocket *sp, void *buf, ssize len);
static RSA      *rsaCallback(SSL *ssl, int isExport, int keyLength);
static int      upgradeOss(MprSocket *sp, MprSsl *ssl, cchar *requiredPeerName);
static int      verifyX509Certificate(int ok, X509_STORE_CTX *ctx);
static ssize    writeOss(MprSocket *sp, cvoid *buf, ssize len);

static DynLock  *sslCreateDynLock(const char *file, int line);
static void     sslDynLock(int mode, DynLock *dl, const char *file, int line);
static void     sslDestroyDynLock(DynLock *dl, const char *file, int line);
static void     sslStaticLock(int mode, int n, const char *file, int line);
static ulong    sslThreadId(void);

static DH       *get_dh512();
static DH       *get_dh1024();

/************************************* Code ***********************************/
/*
    Create the Openssl module. This is called only once
 */
PUBLIC int mprCreateOpenSslModule()
{
    RandBuf     randBuf;
    int         i;

    randBuf.now = mprGetTime();
    randBuf.pid = getpid();
    RAND_seed((void*) &randBuf, sizeof(randBuf));
#if ME_UNIX_LIKE
    RAND_load_file("/dev/urandom", 256);
#endif

    if ((openProvider = mprAllocObj(MprSocketProvider, manageOpenProvider)) == NULL) {
        return MPR_ERR_MEMORY;
    }
    openProvider->upgradeSocket = upgradeOss;
    openProvider->closeSocket = closeOss;
    openProvider->disconnectSocket = disconnectOss;
    openProvider->flushSocket = flushOss;
    openProvider->socketState = getOssState;
    openProvider->readSocket = readOss;
    openProvider->writeSocket = writeOss;
    mprAddSocketProvider("openssl", openProvider);

    /*
        Pre-create expensive keys
     */
    if ((defaultOpenConfig = mprAllocObj(OpenConfig, manageOpenConfig)) == 0) {
        return MPR_ERR_MEMORY;
    }
    defaultOpenConfig->rsaKey512 = RSA_generate_key(512, RSA_F4, 0, 0);
    defaultOpenConfig->rsaKey1024 = RSA_generate_key(1024, RSA_F4, 0, 0);
    defaultOpenConfig->dhKey512 = get_dh512();
    defaultOpenConfig->dhKey1024 = get_dh1024();

    /*
        Configure the SSL library. Use the crypto ID as a one-time test. This allows
        users to configure the library and have their configuration used instead.
     */
    if (CRYPTO_get_id_callback() == 0) {
        numLocks = CRYPTO_num_locks();
        if ((olocks = mprAlloc(numLocks * sizeof(MprMutex*))) == 0) {
            return MPR_ERR_MEMORY;
        }
        for (i = 0; i < numLocks; i++) {
            olocks[i] = mprCreateLock();
        }
        CRYPTO_set_id_callback(sslThreadId);
        CRYPTO_set_locking_callback(sslStaticLock);

        CRYPTO_set_dynlock_create_callback(sslCreateDynLock);
        CRYPTO_set_dynlock_destroy_callback(sslDestroyDynLock);
        CRYPTO_set_dynlock_lock_callback(sslDynLock);
#if !ME_WIN_LIKE
        /* OPT - Should be a configure option to specify desired ciphers */
        OpenSSL_add_all_algorithms();
#endif
        /*
            WARNING: SSL_library_init() is not reentrant. Caller must ensure safety.
         */
        SSL_library_init();
        SSL_load_error_strings();
    }
    return 0;
}


static void manageOpenConfig(OpenConfig *cfg, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        ;
    } else if (flags & MPR_MANAGE_FREE) {
        if (cfg->context != 0) {
            SSL_CTX_free(cfg->context);
            cfg->context = 0;
        }
        if (cfg == defaultOpenConfig) {
            if (cfg->rsaKey512) {
                RSA_free(cfg->rsaKey512);
                cfg->rsaKey512 = 0;
            }
            if (cfg->rsaKey1024) {
                RSA_free(cfg->rsaKey1024);
                cfg->rsaKey1024 = 0;
            }
            if (cfg->dhKey512) {
                DH_free(cfg->dhKey512);
                cfg->dhKey512 = 0;
            }
            if (cfg->dhKey1024) {
                DH_free(cfg->dhKey1024);
                cfg->dhKey1024 = 0;
            }
        }
    }
}


static void manageOpenProvider(MprSocketProvider *provider, int flags)
{
    int     i;

    if (flags & MPR_MANAGE_MARK) {
        /* Mark global locks */
        if (olocks) {
            mprMark(olocks);
            for (i = 0; i < numLocks; i++) {
                mprMark(olocks[i]);
            }
        }
        mprMark(defaultOpenConfig);
        mprMark(provider->name);

    } else if (flags & MPR_MANAGE_FREE) {
        olocks = 0;
    }
}


/*
    Create an SSL configuration for a route. An application can have multiple different SSL 
    configurations for different routes. There is default SSL configuration that is used
    when a route does not define a configuration and also for clients.
 */
static OpenConfig *createOpenSslConfig(MprSocket *sp)
{
    MprSsl          *ssl;
    OpenConfig      *cfg;
    SSL_CTX         *context;
    uchar           resume[16];
    int             verifyMode;

    ssl = sp->ssl;
    assert(ssl);

    if ((ssl->config = mprAllocObj(OpenConfig, manageOpenConfig)) == 0) {
        return 0;
    }
    cfg = ssl->config;
    cfg->rsaKey512 = defaultOpenConfig->rsaKey512;
    cfg->rsaKey1024 = defaultOpenConfig->rsaKey1024;
    cfg->dhKey512 = defaultOpenConfig->dhKey512;
    cfg->dhKey1024 = defaultOpenConfig->dhKey1024;

    cfg = ssl->config;
    assert(cfg);

    if ((context = SSL_CTX_new(SSLv23_method())) == 0) {
        mprLog("error openssl", 0, "Unable to create SSL context"); 
        return 0;
    }
    SSL_CTX_set_app_data(context, (void*) ssl);
    SSL_CTX_sess_set_cache_size(context, 512);
    RAND_bytes(resume, sizeof(resume));
    SSL_CTX_set_session_id_context(context, resume, sizeof(resume));

    if (ssl->verifyPeer && !(ssl->caFile || ssl->caPath)) {
        sp->errorMsg = sfmt("Cannot verify peer due to undefined CA certificates");
        SSL_CTX_free(context);
        return 0;
    }
    verifyMode = ssl->verifyPeer ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_NONE;

    /*
        Configure the certificates
     */
    if (ssl->keyFile || ssl->certFile) {
        if (configureCertificateFiles(ssl, context, (char*) ssl->keyFile, (char*) ssl->certFile) != 0) {
            SSL_CTX_free(context);
            return 0;
        }
    }
    if (ssl->ciphers) {
        if (SSL_CTX_set_cipher_list(context, ssl->ciphers) != 1) {
            sp->errorMsg = sfmt("Unable to set cipher list \"%s\". %s", ssl->ciphers, getOssError(sp)); 
            SSL_CTX_free(context);
            return 0;
        }
    }
    if (verifyMode != SSL_VERIFY_NONE) {
        if (!(ssl->caFile || ssl->caPath)) {
            sp->errorMsg = sclone("No defined certificate authority file");
            SSL_CTX_free(context);
            return 0;
        }
        if ((!SSL_CTX_load_verify_locations(context, (char*) ssl->caFile, (char*) ssl->caPath)) ||
                (!SSL_CTX_set_default_verify_paths(context))) {
            sp->errorMsg = sfmt("Unable to set certificate locations: %s: %s", ssl->caFile, ssl->caPath); 
            SSL_CTX_free(context);
            return 0;
        }
        if (ssl->caFile) {
            STACK_OF(X509_NAME) *certNames;
            certNames = SSL_load_client_CA_file(ssl->caFile);
            if (certNames) {
                /*
                    Define the list of CA certificates to send to the client
                    before they send their client certificate for validation
                 */
                SSL_CTX_set_client_CA_list(context, certNames);
            }
        }
        if (sp->flags & MPR_SOCKET_SERVER) {
            SSL_CTX_set_verify_depth(context, ssl->verifyDepth);
        }
    }
    SSL_CTX_set_verify(context, verifyMode, verifyX509Certificate);

    /*
        Define callbacks
     */
    SSL_CTX_set_tmp_rsa_callback(context, rsaCallback);
    SSL_CTX_set_tmp_dh_callback(context, dhCallback);

    SSL_CTX_set_options(context, SSL_OP_ALL);
#ifdef SSL_OP_NO_TICKET
    SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
#endif
#ifdef SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION
    SSL_CTX_set_options(context, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
#endif
    SSL_CTX_set_mode(context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_MSIE_SSLV2_RSA_PADDING
    SSL_CTX_set_options(context, SSL_OP_MSIE_SSLV2_RSA_PADDING);
#endif
#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
#endif
#ifdef SSL_MODE_RELEASE_BUFFERS
    SSL_CTX_set_mode(context, SSL_MODE_RELEASE_BUFFERS);
#endif
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
    SSL_CTX_set_mode(context, SSL_OP_CIPHER_SERVER_PREFERENCE);
#endif
#if KEEP
    SSL_CTX_set_read_ahead(context, 1);
    SSL_CTX_set_info_callback(context, info_callback);
#endif

    /*
        Select the required protocols
        Disable SSLv2 and SSLv3 by default -- they are insecure.
     */
    SSL_CTX_set_options(context, SSL_OP_NO_SSLv2);
    SSL_CTX_set_options(context, SSL_OP_NO_SSLv3);
#ifdef SSL_OP_NO_TLSv1
    if (!(ssl->protocols & MPR_PROTO_TLSV1)) {
        SSL_CTX_set_options(context, SSL_OP_NO_TLSv1);
    }
#endif
#ifdef SSL_OP_NO_TLSv1_1
    if (!(ssl->protocols & MPR_PROTO_TLSV1_1)) {
        SSL_CTX_set_options(context, SSL_OP_NO_TLSv1_1);
    }
#endif
#ifdef SSL_OP_NO_TLSv1_2
    if (!(ssl->protocols & MPR_PROTO_TLSV1_2)) {
        SSL_CTX_set_options(context, SSL_OP_NO_TLSv1_2);
    }
#endif
    /*
        Ensure we generate a new private key for each connection
     */
    SSL_CTX_set_options(context, SSL_OP_SINGLE_DH_USE);
    cfg->context = context;
    return cfg;
}


/*
    Configure the SSL certificate information using key and cert files
 */
static int configureCertificateFiles(MprSsl *ssl, SSL_CTX *ctx, char *key, char *cert)
{
    assert(ctx);

    if (cert == 0) {
        return 0;
    }
    if (cert && SSL_CTX_use_certificate_chain_file(ctx, cert) <= 0) {
        if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_ASN1) <= 0) {
            mprLog("error openssl", 0, "Cannot open certificate file: %s", cert);
            return -1;
        }
    }
    key = (key == 0) ? cert : key;
    if (key) {
        if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) <= 0) {
            /* attempt ASN1 for self-signed format */
            if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_ASN1) <= 0) {
                mprLog("error openssl", 0, "Cannot open private key file: %s", key);
                return -1;
            }
        }
        if (!SSL_CTX_check_private_key(ctx)) {
            mprLog("error openssl", 0, "Check of private key file failed: %s", key);
            return -1;
        }
    }
    return 0;
}


static void manageOpenSocket(OpenSocket *osp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(osp->sock);
        mprMark(osp->cfg);
        mprMark(osp->requiredPeerName);

    } else if (flags & MPR_MANAGE_FREE) {
        if (osp->handle) {
            SSL_set_shutdown(osp->handle, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
            SSL_free(osp->handle);
            osp->handle = 0;
        }
    }
}


static void closeOss(MprSocket *sp, bool gracefully)
{
    OpenSocket    *osp;

    osp = sp->sslSocket;
    lock(sp);
    sp->service->standardProvider->closeSocket(sp, gracefully);
    SSL_free(osp->handle);
    osp->handle = 0;
    unlock(sp);
}


/*
    Upgrade a standard socket to use SSL/TLS
 */
static int upgradeOss(MprSocket *sp, MprSsl *ssl, cchar *requiredPeerName)
{
    OpenSocket      *osp;
    OpenConfig      *cfg;
    int             rc;

    assert(sp);

    if (ssl == 0) {
        ssl = mprCreateSsl(sp->flags & MPR_SOCKET_SERVER);
    }
    if ((osp = (OpenSocket*) mprAllocObj(OpenSocket, manageOpenSocket)) == 0) {
        return MPR_ERR_MEMORY;
    }
    osp->sock = sp;
    sp->sslSocket = osp;
    sp->ssl = ssl;

    if (!ssl->config || ssl->changed) {
        if ((ssl->config = createOpenSslConfig(sp)) == 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
        ssl->changed = 0;
    }

    /*
        Create and configure the SSL struct
     */
    cfg = osp->cfg = sp->ssl->config;
    if ((osp->handle = (SSL*) SSL_new(cfg->context)) == 0) {
        return MPR_ERR_BAD_STATE;
    }
    SSL_set_app_data(osp->handle, (void*) osp);

    /*
        Create a socket bio
     */
    osp->bio = BIO_new_socket((int) sp->fd, BIO_NOCLOSE);
    SSL_set_bio(osp->handle, osp->bio, osp->bio);
    if (sp->flags & MPR_SOCKET_SERVER) {
        SSL_set_accept_state(osp->handle);
    } else {
        if (requiredPeerName) {
            osp->requiredPeerName = sclone(requiredPeerName);
        }
        /*
            Block while connecting
         */
        mprSetSocketBlockingMode(sp, 1);
        sp->errorMsg = 0;
        if ((rc = SSL_connect(osp->handle)) < 1) {
            if (sp->errorMsg) {
                mprLog("info mpr ssl openssl", 4, "Connect failed: %s", sp->errorMsg);
            } else {
                mprLog("info mpr ssl openssl", 4, "Connect failed: error %s", getOssError(sp));
            }
            return MPR_ERR_CANT_CONNECT;
        }
        if (rc > 0 && !(sp->flags & MPR_SOCKET_CHECKED)) {
            if (checkCert(sp) < 0) {
                return MPR_ERR_CANT_CONNECT;
            }
            sp->secured = 1;
            sp->flags |= MPR_SOCKET_CHECKED;
        }
        mprSetSocketBlockingMode(sp, 0);
    }
#if defined(ME_MPR_SSL_RENEGOTIATE) && !ME_MPR_SSL_RENEGOTIATE
    /*
        Disable renegotiation after the initial handshake if renegotiate is explicitly set to false (CVE-2009-3555).
        Note: this really is a bogus CVE as disabling renegotiation is not required nor does it enhance security if
        used with up-to-date (patched) SSL stacks
     */
    if (osp->handle->s3) {
        osp->handle->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
    }
#endif
    return 0;
}


/*
    Parse the cert info and write properties to the buffer
    Modifies the info argument
 */
static void parseCertFields(MprBuf *buf, char *prefix, char *prefix2, char *info)
{
    char    c, *cp, *term, *key, *value;

    term = cp = info;
    do {
        c = *cp;
        if (c == '/' || c == '\0') {
            *cp = '\0';
            key = stok(term, "=", &value);
            if (smatch(key, "emailAddress")) {
                key = "EMAIL";
            }
            mprPutToBuf(buf, "%s%s%s=%s,", prefix, prefix2, key, value);
            term = &cp[1];
            *cp = c;
        }
    } while (*cp++ != '\0');
}


static char *getOssState(MprSocket *sp)
{
    OpenSocket      *osp;
    MprBuf          *buf;
    X509_NAME       *xSubject;
    X509            *cert;
    char            *prefix;
    char            subject[512], issuer[512], peer[512];

    osp = sp->sslSocket;
    buf = mprCreateBuf(0, 0);
    mprPutToBuf(buf, "PROVIDER=openssl,CIPHER=%s,", SSL_get_cipher(osp->handle));

    if ((cert = SSL_get_peer_certificate(osp->handle)) == 0) {
        mprPutToBuf(buf, "%s=\"none\",", sp->acceptIp ? "CLIENT_CERT" : "SERVER_CERT");

    } else {
        xSubject = X509_get_subject_name(cert);
        X509_NAME_get_text_by_NID(xSubject, NID_commonName, peer, sizeof(peer) - 1);
        mprPutToBuf(buf, "PEER=\"%s\",", peer);

        prefix = sp->acceptIp ? "CLIENT_" : "SERVER_";
        X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject) -1);
        parseCertFields(buf, prefix, "S_", &subject[1]);

        X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer) -1);
        parseCertFields(buf, prefix, "I_", &issuer[1]);
        X509_free(cert);
    }
    if ((cert = SSL_get_certificate(osp->handle)) != 0) {
        prefix =  sp->acceptIp ? "SERVER_" : "CLIENT_";
        X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject) -1);
        parseCertFields(buf, prefix, "S_", &subject[1]);

        X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer) -1);
        parseCertFields(buf, prefix, "I_", &issuer[1]);
        /* Don't call X509_free on own cert */
    }
    return mprGetBufStart(buf);
}


static void disconnectOss(MprSocket *sp)
{
    sp->service->standardProvider->disconnectSocket(sp);
}


/*
    Check the certificate peer name
 */
static int checkCert(MprSocket *sp)
{
    MprSsl      *ssl;
    OpenSocket  *osp;
    X509        *cert;
    X509_NAME   *xSubject;
    char        subject[512], issuer[512], peerName[512], *target, *certName, *tp;

    ssl = sp->ssl;
    osp = (OpenSocket*) sp->sslSocket;
    sp->cipher = sclone(SSL_get_cipher(osp->handle));

    cert = SSL_get_peer_certificate(osp->handle);
    if (cert == 0) {
        peerName[0] = '\0';
    } else {
        xSubject = X509_get_subject_name(cert);
        X509_NAME_oneline(xSubject, subject, sizeof(subject) -1);
        X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer) -1);
        X509_NAME_get_text_by_NID(xSubject, NID_commonName, peerName, sizeof(peerName) - 1);
        sp->peerName = sclone(peerName);
        sp->peerCert = sclone(subject);
        sp->peerCertIssuer = sclone(issuer);
        X509_free(cert);
    }
    if (ssl->verifyPeer && osp->requiredPeerName) {
        target = osp->requiredPeerName;
        certName = peerName;

        if (target == 0 || *target == '\0' || strchr(target, '.') == 0) {
            sp->errorMsg = sfmt("Bad peer name");
            return -1;
        }
        if (!smatch(certName, "localhost")) {
            if (strchr(certName, '.') == 0) {
                sp->errorMsg = sfmt("Peer certificate must have a domain: \"%s\"", certName);
                return -1;
            }
            if (*certName == '*' && certName[1] == '.') {
                /* Wildcard cert */
                certName = &certName[2];
                if (strchr(certName, '.') == 0) {
                    /* Peer must be of the form *.domain.tld. i.e. *.com is not valid */
                    sp->errorMsg = sfmt("Peer CN is not valid %s", peerName);
                    return -1;
                }
                if ((tp = strchr(target, '.')) != 0 && strchr(&tp[1], '.')) {
                    /* Strip host name if target has a host name */
                    target = &tp[1];
                }
            }
        }
        if (!smatch(target, certName)) {
            sp->errorMsg = sfmt("Certificate common name mismatch CN \"%s\" vs required \"%s\"", peerName,
                osp->requiredPeerName);
            return -1;
        }
    }
    return 0;
}


/*
    Return the number of bytes read. Return -1 on errors and EOF. Distinguish EOF via mprIsSocketEof.
    If non-blocking, may return zero if no data or still handshaking.
 */
static ssize readOss(MprSocket *sp, void *buf, ssize len)
{
    OpenSocket      *osp;
    int             rc, error, retries, i;

    //  OPT - should not need these locks
    lock(sp);
    osp = (OpenSocket*) sp->sslSocket;
    assert(osp);

    if (osp->handle == 0) {
        unlock(sp);
        return -1;
    }
    /*
        Limit retries on WANT_READ. If non-blocking and no data, then this can spin forever.
     */
    retries = 5;
    for (i = 0; i < retries; i++) {
        rc = SSL_read(osp->handle, buf, (int) len);
        if (rc < 0) {
            error = SSL_get_error(osp->handle, rc);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_CONNECT || error == SSL_ERROR_WANT_ACCEPT) {
                continue;
            }
            mprLog("info mpr ssl openssl", 5, "SSL_read %s", getOssError(sp));
        }
        break;
    }
    if (rc > 0 && !(sp->flags & MPR_SOCKET_CHECKED)) {
        if (checkCert(sp) < 0) {
            return MPR_ERR_BAD_STATE;
        }
        sp->secured = 1;
        sp->flags |= MPR_SOCKET_CHECKED;
    }
    if (rc <= 0) {
        error = SSL_get_error(osp->handle, rc);
        if (error == SSL_ERROR_WANT_READ) {
            rc = 0;
        } else if (error == SSL_ERROR_WANT_WRITE) {
            mprNap(10);
            rc = 0;
        } else if (error == SSL_ERROR_ZERO_RETURN) {
            sp->flags |= MPR_SOCKET_EOF;
            rc = -1;
        } else if (error == SSL_ERROR_SYSCALL) {
            sp->flags |= MPR_SOCKET_EOF;
            rc = -1;
        } else if (error != SSL_ERROR_ZERO_RETURN) {
            /* SSL_ERROR_SSL */
            mprLog("info mpr ssl openssl", 4, "%s", getOssError(sp));
            rc = -1;
            sp->flags |= MPR_SOCKET_EOF;
        }
    } else if (SSL_pending(osp->handle) > 0) {
        sp->flags |= MPR_SOCKET_BUFFERED_READ;
        mprRecallWaitHandlerByFd(sp->fd);
    }
    unlock(sp);
    return rc;
}


/*
    Write data. Return the number of bytes written or -1 on errors.
 */
static ssize writeOss(MprSocket *sp, cvoid *buf, ssize len)
{
    OpenSocket  *osp;
    ssize       totalWritten;
    int         rc;

    //  OPT - should not need these locks
    lock(sp);
    osp = (OpenSocket*) sp->sslSocket;

    if (osp->bio == 0 || osp->handle == 0 || len <= 0) {
        assert(0);
        unlock(sp);
        return -1;
    }
    totalWritten = 0;
    ERR_clear_error();
    rc = 0;

    do {
        rc = SSL_write(osp->handle, buf, (int) len);
        mprLog("info mpr ssl openssl", 7, "Wrote %d, requested len %zd", rc, len);
        if (rc <= 0) {
            if (SSL_get_error(osp->handle, rc) == SSL_ERROR_WANT_WRITE) {
                break;
            }
            unlock(sp);
            return -1;
        }
        totalWritten += rc;
        buf = (void*) ((char*) buf + rc);
        len -= rc;
        mprLog("info mpr ssl openssl", 7, "write len %zd, written %d, total %zd, error %d", len, rc, totalWritten,
            SSL_get_error(osp->handle, rc));
    } while (len > 0);
    unlock(sp);

    if (totalWritten == 0 && rc == SSL_ERROR_WANT_WRITE) {
        mprSetError(EAGAIN);
        return -1;
    }
    return totalWritten;
}


/*
    Called to verify X509 client certificates
 */
static int verifyX509Certificate(int ok, X509_STORE_CTX *xContext)
{
    X509            *cert;
    SSL             *handle;
    OpenSocket      *osp;
    MprSocket       *sp;
    MprSsl          *ssl;
    char            subject[512], issuer[512], peerName[512];
    int             error, depth;

    subject[0] = issuer[0] = '\0';

    handle = (SSL*) X509_STORE_CTX_get_app_data(xContext);
    osp = (OpenSocket*) SSL_get_app_data(handle);
    sp = osp->sock;
    ssl = sp->ssl;

    cert = X509_STORE_CTX_get_current_cert(xContext);
    depth = X509_STORE_CTX_get_error_depth(xContext);
    error = X509_STORE_CTX_get_error(xContext);

    ok = 1;
    if (X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject) - 1) < 0) {
        sp->errorMsg = sclone("Cannot get subject name");
        ok = 0;
    }
    if (X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer) - 1) < 0) {
        sp->errorMsg = sclone("Cannot get issuer name");
        ok = 0;
    }
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, peerName, sizeof(peerName) - 1) == 0) {
        sp->errorMsg = sclone("Cannot get peer name");
        ok = 0;
    }
    if (ok && ssl->verifyDepth < depth) {
        if (error == 0) {
            error = X509_V_ERR_CERT_CHAIN_TOO_LONG;
        }
    }
    switch (error) {
    case X509_V_OK:
        break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        /* Normal self signed certificate */
        if (ssl->verifyIssuer) {
            sp->errorMsg = sclone("Self-signed certificate");
            ok = 0;
        }
        break;

    case X509_V_ERR_CERT_UNTRUSTED:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        if (ssl->verifyIssuer) {
            /* Issuer cannot be verified */
            sp->errorMsg = sclone("Certificate not trusted");
            ok = 0;
        }
        break;

    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        if (ssl->verifyIssuer) {
            /* Issuer cannot be verified */
            sp->errorMsg = sclone("Certificate not trusted");
            ok = 0;
        }
        break;

    case X509_V_ERR_CERT_HAS_EXPIRED:
        sp->errorMsg = sfmt("Certificate has expired");
        ok = 0;
        break;

    case X509_V_ERR_CERT_CHAIN_TOO_LONG:
    case X509_V_ERR_CERT_NOT_YET_VALID:
    case X509_V_ERR_CERT_REJECTED:
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
    case X509_V_ERR_INVALID_CA:
    default:
        sp->errorMsg = sfmt("Certificate verification error %d", error);
        ok = 0;
        break;
    }
    return ok;
}


static ssize flushOss(MprSocket *sp)
{
    return 0;
}

 
static ulong sslThreadId()
{
    return (long) mprGetCurrentOsThread();
}


//  OPT - should not need these locks

static void sslStaticLock(int mode, int n, const char *file, int line)
{
    assert(0 <= n && n < numLocks);

    if (olocks) {
        if (mode & CRYPTO_LOCK) {
            mprLock(olocks[n]);
        } else {
            mprUnlock(olocks[n]);
        }
    }
}


//  OPT - should not need these locks
static DynLock *sslCreateDynLock(const char *file, int line)
{
    DynLock     *dl;

    dl = mprAllocZeroed(sizeof(DynLock));
    dl->mutex = mprCreateLock(dl);
    mprHold(dl->mutex);
    return dl;
}


static void sslDestroyDynLock(DynLock *dl, const char *file, int line)
{
    if (dl->mutex) {
        mprRelease(dl->mutex);
        dl->mutex = 0;
    }
}


static void sslDynLock(int mode, DynLock *dl, const char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        mprLock(dl->mutex);
    } else {
        mprUnlock(dl->mutex);
    }
}


static char *getOssError(MprSocket *sp)
{
    char    ebuf[ME_MAX_BUFFER];
    ulong   error;

    error = ERR_get_error();
    ERR_error_string_n(error, ebuf, sizeof(ebuf) - 1);
    sp->errorMsg = sclone(ebuf);
    return sp->errorMsg;
}


/*
    Used for ephemeral RSA keys
 */
static RSA *rsaCallback(SSL *handle, int isExport, int keyLength)
{
    MprSocket       *sp;
    OpenSocket      *osp;
    OpenConfig      *cfg;
    RSA             *key;

    osp = (OpenSocket*) SSL_get_app_data(handle);
    sp = osp->sock;
    assert(sp);
    cfg = sp->ssl->config;

    key = 0;
    switch (keyLength) {
    case 512:
        key = cfg->rsaKey512;
        break;

    case 1024:
    default:
        key = cfg->rsaKey1024;
    }
    return key;
}


/*
    Used for ephemeral DH keys
 */
static DH *dhCallback(SSL *handle, int isExport, int keyLength)
{
    MprSocket       *sp;
    OpenSocket      *osp;
    OpenConfig      *cfg;
    DH              *key;

    osp = (OpenSocket*) SSL_get_app_data(handle);
    sp = osp->sock;
    cfg = sp->ssl->config;

    key = 0;
    switch (keyLength) {
    case 512:
        key = cfg->dhKey512;
        break;

    case 1024:
    default:
        key = cfg->dhKey1024;
    }
    return key;
}


/*
    openSslDh.c - OpenSSL DH get routines. Generated by openssl.
    Use bit gendh to generate new content.
 */
static DH *get_dh512()
{
    static unsigned char dh512_p[] = {
        0x8E,0xFD,0xBE,0xD3,0x92,0x1D,0x0C,0x0A,0x58,0xBF,0xFF,0xE4,
        0x51,0x54,0x36,0x39,0x13,0xEA,0xD8,0xD2,0x70,0xBB,0xE3,0x8C,
        0x86,0xA6,0x31,0xA1,0x04,0x2A,0x09,0xE4,0xD0,0x33,0x88,0x5F,
        0xEF,0xB1,0x70,0xEA,0x42,0xB6,0x0E,0x58,0x60,0xD5,0xC1,0x0C,
        0xD1,0x12,0x16,0x99,0xBC,0x7E,0x55,0x7C,0xE4,0xC1,0x5D,0x15,
        0xF6,0x45,0xBC,0x73,
    };
    static unsigned char dh512_g[] = {
        0x02,
    };
    DH *dh;
    if ((dh = DH_new()) == NULL) {
        return(NULL);
    }
    dh->p = BN_bin2bn(dh512_p, sizeof(dh512_p), NULL);
    dh->g = BN_bin2bn(dh512_g, sizeof(dh512_g), NULL);
    if ((dh->p == NULL) || (dh->g == NULL)) { 
        DH_free(dh); 
        return(NULL); 
    }
    return dh;
}


static DH *get_dh1024()
{
    static unsigned char dh1024_p[] = {
        0xCD,0x02,0x2C,0x11,0x43,0xCD,0xAD,0xF5,0x54,0x5F,0xED,0xB1,
        0x28,0x56,0xDF,0x99,0xFA,0x80,0x2C,0x70,0xB5,0xC8,0xA8,0x12,
        0xC3,0xCD,0x38,0x0D,0x3B,0xE1,0xE3,0xA3,0xE4,0xE9,0xCB,0x58,
        0x78,0x7E,0xA6,0x80,0x7E,0xFC,0xC9,0x93,0x3A,0x86,0x1C,0x8E,
        0x0B,0xA2,0x1C,0xD0,0x09,0x99,0x29,0x9B,0xC1,0x53,0xB8,0xF3,
        0x98,0xA7,0xD8,0x46,0xBE,0x5B,0xB9,0x64,0x31,0xCF,0x02,0x63,
        0x0F,0x5D,0xF2,0xBE,0xEF,0xF6,0x55,0x8B,0xFB,0xF0,0xB8,0xF7,
        0xA5,0x2E,0xD2,0x6F,0x58,0x1E,0x46,0x3F,0x74,0x3C,0x02,0x41,
        0x2F,0x65,0x53,0x7F,0x1C,0x7B,0x8A,0x72,0x22,0x1D,0x2B,0xE9,
        0xA3,0x0F,0x50,0xC3,0x13,0x12,0x6C,0xD2,0x17,0xA9,0xA5,0x82,
        0xFC,0x91,0xE3,0x3E,0x28,0x8A,0x97,0x73,
    };
    static unsigned char dh1024_g[] = {
        0x02,
    };
    DH *dh;
    if ((dh = DH_new()) == NULL) {
        return(NULL);
    }
    dh->p = BN_bin2bn(dh1024_p, sizeof(dh1024_p), NULL);
    dh->g = BN_bin2bn(dh1024_g, sizeof(dh1024_g), NULL);
    if ((dh->p == NULL) || (dh->g == NULL)) {
        DH_free(dh); 
        return(NULL); 
    }
    return dh;
}

#else
void opensslDummy() {}
#endif /* ME_COM_OPENSSL */

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



/********* Start of file src/ssl/ssl.c ************/


/**
    ssl.c -- Initialization for libmprssl. Load the SSL provider.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "mpr.h"

/********************************** Globals ***********************************/

/*
    See: http://www.iana.org/assignments/tls-parameters/tls-parameters.xml
*/
MprCipher mprCiphers[] = {
    { 0x0001, "SSL_RSA_WITH_NULL_MD5" },
    { 0x0002, "SSL_RSA_WITH_NULL_SHA" },
    { 0x0004, "TLS_RSA_WITH_RC4_128_MD5" },
    { 0x0005, "TLS_RSA_WITH_RC4_128_SHA" },
    { 0x0009, "SSL_RSA_WITH_DES_CBC_SHA" },
    { 0x000A, "SSL_RSA_WITH_3DES_EDE_CBC_SHA" },
    { 0x0015, "SSL_DHE_RSA_WITH_DES_CBC_SHA" },
    { 0x0016, "SSL_DHE_RSA_WITH_3DES_EDE_CBC_SHA" },
    { 0x001A, "SSL_DH_ANON_WITH_DES_CBC_SHA" },
    { 0x001B, "SSL_DH_ANON_WITH_3DES_EDE_CBC_SHA" },
    { 0x002F, "TLS_RSA_WITH_AES_128_CBC_SHA" },
    { 0x0033, "TLS_DHE_RSA_WITH_AES_128_CBC_SHA" },
    { 0x0034, "TLS_DH_ANON_WITH_AES_128_CBC_SHA" },
    { 0x0035, "TLS_RSA_WITH_AES_256_CBC_SHA" },
    { 0x0039, "TLS_DHE_RSA_WITH_AES_256_CBC_SHA" },
    { 0x003A, "TLS_DH_ANON_WITH_AES_256_CBC_SHA" },
    { 0x003B, "SSL_RSA_WITH_NULL_SHA256" },
    { 0x003C, "TLS_RSA_WITH_AES_128_CBC_SHA256" },
    { 0x003D, "TLS_RSA_WITH_AES_256_CBC_SHA256" },
    { 0x0041, "TLS_RSA_WITH_CAMELLIA_128_CBC_SHA" },
    { 0x0067, "TLS_DHE_RSA_WITH_AES_128_CBC_SHA256" },
    { 0x006B, "TLS_DHE_RSA_WITH_AES_256_CBC_SHA256" },
    { 0x006C, "TLS_DH_ANON_WITH_AES_128_CBC_SHA256" },
    { 0x006D, "TLS_DH_ANON_WITH_AES_256_CBC_SHA256" },
    { 0x0084, "TLS_DHE_RSA_WITH_CAMELLIA_256_CBC_SHA" },
    { 0x0088, "TLS_RSA_WITH_CAMELLIA_256_CBC_SHA" },
    { 0x008B, "TLS_PSK_WITH_3DES_EDE_CBC_SHA" },
    { 0x008C, "TLS_PSK_WITH_AES_128_CBC_SHA" },
    { 0x008D, "TLS_PSK_WITH_AES_256_CBC_SHA" },
    { 0x008F, "SSL_DHE_PSK_WITH_3DES_EDE_CBC_SHA" },
    { 0x0090, "TLS_DHE_PSK_WITH_AES_128_CBC_SHA" },
    { 0x0091, "TLS_DHE_PSK_WITH_AES_256_CBC_SHA" },
    { 0x0093, "TLS_RSA_PSK_WITH_3DES_EDE_CBC_SHA" },
    { 0x0094, "TLS_RSA_PSK_WITH_AES_128_CBC_SHA" },
    { 0x0095, "TLS_RSA_PSK_WITH_AES_256_CBC_SHA" },
    { 0xC001, "TLS_ECDH_ECDSA_WITH_NULL_SHA" },
    { 0xC003, "SSL_ECDH_ECDSA_WITH_3DES_EDE_CBC_SHA" },
    { 0xC004, "TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA" },
    { 0xC005, "TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA" },
    { 0xC006, "TLS_ECDHE_ECDSA_WITH_NULL_SHA" },
    { 0xC008, "SSL_ECDHE_ECDSA_WITH_3DES_EDE_CBC_SHA" },
    { 0xC009, "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA" },
    { 0xC00A, "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA" },
    { 0xC00B, "TLS_ECDH_RSA_WITH_NULL_SHA" },
    { 0xC00D, "SSL_ECDH_RSA_WITH_3DES_EDE_CBC_SHA" },
    { 0xC00E, "TLS_ECDH_RSA_WITH_AES_128_CBC_SHA" },
    { 0xC00F, "TLS_ECDH_RSA_WITH_AES_256_CBC_SHA" },
    { 0xC010, "TLS_ECDHE_RSA_WITH_NULL_SHA" },
    { 0xC012, "SSL_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA" },
    { 0xC013, "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA" },
    { 0xC014, "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA" },
    { 0xC015, "TLS_ECDH_anon_WITH_NULL_SHA" },
    { 0xC017, "SSL_ECDH_anon_WITH_3DES_EDE_CBC_SHA" },
    { 0xC018, "TLS_ECDH_anon_WITH_AES_128_CBC_SHA" },
    { 0xC019, "TLS_ECDH_anon_WITH_AES_256_CBC_SHA " },
    { 0xC023, "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256" },
    { 0xC024, "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384" },
    { 0xC025, "TLS_ECDH_ECDSA_WITH_AES_128_CBC_SHA256" },
    { 0xC026, "TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA384" },
    { 0xC027, "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256" },
    { 0xC028, "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384" },
    { 0xC029, "TLS_ECDH_RSA_WITH_AES_128_CBC_SHA256" },
    { 0xC02A, "TLS_ECDH_RSA_WITH_AES_256_CBC_SHA384" },
    { 0xC02B, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256" },
    { 0xC02C, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384" },
    { 0xC02D, "TLS_ECDH_ECDSA_WITH_AES_128_GCM_SHA256" },
    { 0xC02E, "TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384" },
    { 0xC02F, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256" },
    { 0xC030, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384" },
    { 0xC031, "TLS_ECDH_RSA_WITH_AES_128_GCM_SHA256" },
    { 0xC032, "TLS_ECDH_RSA_WITH_AES_256_GCM_SHA384" },
    { 0xFFF0, "TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8" },
    { 0x0, 0 },
};

/************************************ Code ************************************/
/*
    Module initialization entry point
 */
PUBLIC int mprSslInit(void *unused, MprModule *module)
{
#if ME_COM_SSL
    assert(module);

    /*
        Order matters. The last enabled stack becomes the default.
     */
#if ME_COM_MATRIXSSL
    if (mprCreateMatrixSslModule() < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    MPR->socketService->sslProvider = sclone("matrixssl");
#endif
#if ME_COM_NANOSSL
    if (mprCreateNanoSslModule() < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    MPR->socketService->sslProvider = sclone("nanossl");
#endif
#if ME_COM_OPENSSL
    if (mprCreateOpenSslModule() < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    MPR->socketService->sslProvider = sclone("openssl");
#endif
#if ME_COM_EST
    if (mprCreateEstModule() < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    MPR->socketService->sslProvider = sclone("est");
#endif
    return 0;
#else
    return MPR_ERR_BAD_STATE;
#endif /* BLD_COMP_SSL */
}


PUBLIC cchar *mprGetSslCipherName(int code) 
{
    MprCipher   *cp;

    for (cp = mprCiphers; cp->name; cp++) {
        if (cp->code == code) {
            return cp->name;
        }
    }
    return 0;
}


PUBLIC int mprGetSslCipherCode(cchar *cipher) 
{
    MprCipher   *cp;

    for (cp = mprCiphers; cp->name; cp++) {
        if (smatch(cp->name, cipher)) {
            return cp->code;
        }
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

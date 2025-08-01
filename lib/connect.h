#ifndef HEADER_CURL_CONNECT_H
#define HEADER_CURL_CONNECT_H
/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
#include "curl_setup.h"

#include "curlx/nonblock.h" /* for curlx_nonblock() */
#include "sockaddr.h"
#include "curlx/timeval.h"

struct Curl_dns_entry;
struct ip_quadruple;

enum alpnid Curl_alpn2alpnid(const char *name, size_t len);

/* generic function that returns how much time there is left to run, according
   to the timeouts set */
timediff_t Curl_timeleft(struct Curl_easy *data,
                         struct curltime *nowp,
                         bool duringconnect);

#define DEFAULT_CONNECT_TIMEOUT 300000 /* milliseconds == five minutes */

#define DEFAULT_SHUTDOWN_TIMEOUT_MS   (2 * 1000)

void Curl_shutdown_start(struct Curl_easy *data, int sockindex,
                         int timeout_ms, struct curltime *nowp);

/* return how much time there is left to shutdown the connection at
 * sockindex. Returns 0 if there is no limit or shutdown has not started. */
timediff_t Curl_shutdown_timeleft(struct connectdata *conn, int sockindex,
                                  struct curltime *nowp);

/* return how much time there is left to shutdown the connection.
 * Returns 0 if there is no limit or shutdown has not started. */
timediff_t Curl_conn_shutdown_timeleft(struct connectdata *conn,
                                       struct curltime *nowp);

void Curl_shutdown_clear(struct Curl_easy *data, int sockindex);

/* TRUE iff shutdown has been started */
bool Curl_shutdown_started(struct Curl_easy *data, int sockindex);

/*
 * Used to extract socket and connectdata struct for the most recent
 * transfer on the given Curl_easy.
 *
 * The returned socket will be CURL_SOCKET_BAD in case of failure!
 */
curl_socket_t Curl_getconnectinfo(struct Curl_easy *data,
                                  struct connectdata **connp);

bool Curl_addr2string(struct sockaddr *sa, curl_socklen_t salen,
                      char *addr, int *port);

/*
 * Curl_conncontrol() marks the end of a connection/stream. The 'closeit'
 * argument specifies if it is the end of a connection or a stream.
 *
 * For stream-based protocols (such as HTTP/2), a stream close will not cause
 * a connection close. Other protocols will close the connection for both
 * cases.
 *
 * It sets the bit.close bit to TRUE (with an explanation for debug builds),
 * when the connection will close.
 */

#define CONNCTRL_KEEP 0 /* undo a marked closure */
#define CONNCTRL_CONNECTION 1
#define CONNCTRL_STREAM 2

void Curl_conncontrol(struct connectdata *conn,
                      int closeit
#if defined(DEBUGBUILD) && !defined(CURL_DISABLE_VERBOSE_STRINGS)
                      , const char *reason
#endif
  );

#if defined(DEBUGBUILD) && !defined(CURL_DISABLE_VERBOSE_STRINGS)
#define streamclose(x,y) Curl_conncontrol(x, CONNCTRL_STREAM, y)
#define connclose(x,y) Curl_conncontrol(x, CONNCTRL_CONNECTION, y)
#define connkeep(x,y) Curl_conncontrol(x, CONNCTRL_KEEP, y)
#else /* if !DEBUGBUILD || CURL_DISABLE_VERBOSE_STRINGS */
#define streamclose(x,y) Curl_conncontrol(x, CONNCTRL_STREAM)
#define connclose(x,y) Curl_conncontrol(x, CONNCTRL_CONNECTION)
#define connkeep(x,y) Curl_conncontrol(x, CONNCTRL_KEEP)
#endif

CURLcode Curl_cf_setup_insert_after(struct Curl_cfilter *cf_at,
                                    struct Curl_easy *data,
                                    int transport,
                                    int ssl_mode);

/**
 * Setup the cfilters at `sockindex` in connection `conn`.
 * If no filter chain is installed yet, inspects the configuration
 * in `data` and `conn? to install a suitable filter chain.
 */
CURLcode Curl_conn_setup(struct Curl_easy *data,
                         struct connectdata *conn,
                         int sockindex,
                         struct Curl_dns_entry *dns,
                         int ssl_mode);

extern struct Curl_cftype Curl_cft_setup;

#endif /* HEADER_CURL_CONNECT_H */

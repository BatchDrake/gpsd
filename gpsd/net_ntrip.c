/* net_ntrip.c -- gather and dispatch DGNSS data from Ntrip broadcasters
 *
 * This file is Copyright 2010 by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 *
 * See:
 * https://igs.bkg.bund.de/root_ftp/NTRIP/documentation/NtripDocumentation.pdf
 */

#include "../include/gpsd_config.h"  /* must be before all includes */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/gpsd.h"
#include "../include/strfuncs.h"

#define NTRIP_SOURCETABLE       "SOURCETABLE 200 OK\r\n"
#define NTRIP_ENDSOURCETABLE    "ENDSOURCETABLE"
#define NTRIP_CAS               "CAS;"
#define NTRIP_NET               "NET;"
#define NTRIP_STR               "STR;"
#define NTRIP_BR                "\r\n"
#define NTRIP_QSC               "\";\""
#define NTRIP_ICY               "ICY 200 OK"
#define NTRIP_UNAUTH            "401 Unauthorized"

static char *ntrip_field_iterate(char *start,
                                 char *prev,
                                 const char *eol,
                                 const struct gpsd_errout_t *errout)
{
    char *s, *t, *u;

    if (start)
        s = start;
    else {
        if (!prev)
            return NULL;
        s = prev + strlen(prev) + 1;
        if (s >= eol)
            return NULL;
    }

    /* ignore any quoted ; chars as they are part of the field content */
    t = s;
    while ((u = strstr(t, NTRIP_QSC)))
        t = u + strlen(NTRIP_QSC);

    if ((t = strstr(t, ";")))
        *t = '\0';

    GPSD_LOG(LOG_RAW, errout, "Next Ntrip source table field %s\n", s);

    return s;
}


/* Decode a stream record from the sourcetable
 * See: http://software.rtcm-ntrip.org/wiki/STR
 */
static void ntrip_str_parse(char *str, size_t len,
                            struct ntrip_stream_t *hold,
                            const struct gpsd_errout_t *errout)
{
    char *s, *eol = str + len;

    memset(hold, 0, sizeof(*hold));

    /* <mountpoint> */
    if ((s = ntrip_field_iterate(str, NULL, eol, errout)))
        (void)strlcpy(hold->mountpoint, s, sizeof(hold->mountpoint));
    /* <identifier> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <format> */
    if ((s = ntrip_field_iterate(NULL, s, eol, errout))) {
        if ((strcasecmp("RTCM 2", s) == 0) ||
            (strcasecmp("RTCM2", s) == 0))
            hold->format = fmt_rtcm2;
        else if (strcasecmp("RTCM 2.0", s) == 0)
            hold->format = fmt_rtcm2_0;
        else if (strcasecmp("RTCM 2.1", s) == 0)
            hold->format = fmt_rtcm2_1;
        else if ((strcasecmp("RTCM 2.2", s) == 0) ||
                 (strcasecmp("RTCM22", s) == 0))
            hold->format = fmt_rtcm2_2;
        else if ((strcasecmp("RTCM2.3", s) == 0) ||
                 (strcasecmp("RTCM 2.3", s) == 0))
            hold->format = fmt_rtcm2_3;
        /* required for the SAPOS derver in Gemany, confirmed as RTCM2.3 */
        else if (strcasecmp("RTCM1_", s) == 0)
            hold->format = fmt_rtcm2_3;
        else if ((strcasecmp("RTCM 3", s) == 0) ||
                 (strcasecmp("RTCM 3.0", s) == 0) ||
                 (strcasecmp("RTCM3.0", s) == 0) ||
                 (strcasecmp("RTCM3", s) == 0))
            hold->format = fmt_rtcm3_0;
        else if ((strcasecmp("RTCM3.1", s) == 0) ||
                 (strcasecmp("RTCM 3.1", s) == 0))
            hold->format = fmt_rtcm3_1;
        else if ((strcasecmp("RTCM 3.2", s) == 0) ||
                 (strcasecmp("RTCM32", s) == 0))
            hold->format = fmt_rtcm3_2;
        else if (strcasecmp("RTCM 3.3", s) == 0)
            hold->format = fmt_rtcm3_3;
        else {
            hold->format = fmt_unknown;
            GPSD_LOG(LOG_WARN, errout, "NTRIP: Got unknown format '%s'\n", s);
        }
    }
    /* <format-details> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <carrier> */
    if ((s = ntrip_field_iterate(NULL, s, eol, errout)))
        hold->carrier = atoi(s);
    /* <nav-system> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <network> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <country> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <latitude> */
    hold->latitude = NAN;
    if ((s = ntrip_field_iterate(NULL, s, eol, errout)))
        hold->latitude = safe_atof(s);
    /* <longitude> */
    hold->longitude = NAN;
    if ((s = ntrip_field_iterate(NULL, s, eol, errout)))
        hold->longitude = safe_atof(s);
    /* <nmea> */
    if ((s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->nmea = atoi(s);
    }
    /* <solution> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <generator> */
    s = ntrip_field_iterate(NULL, s, eol, errout);
    /* <compr-encryp> */
    if ((s = ntrip_field_iterate(NULL, s, eol, errout))) {
        if ((0 == strcmp(" ", s)) || (0 == strlen(s)) ||
            (0 == strcasecmp("none", s))) {
            hold->compr_encryp = cmp_enc_none;
        } else {
            hold->compr_encryp = cmp_enc_unknown;
            GPSD_LOG(LOG_WARN, errout,
                     "NTRIP: Got unknown {compress,encrypt}ion '%s'\n", s);
        }
    }
    // <authentication>
    if ((s = ntrip_field_iterate(NULL, s, eol, errout))) {
        if (strcasecmp("N", s) == 0)
            hold->authentication = auth_none;
        else if (strcasecmp("B", s) == 0)
            hold->authentication = auth_basic;
        else if (strcasecmp("D", s) == 0)
            hold->authentication = auth_digest;
        else {
            hold->authentication = auth_unknown;
            GPSD_LOG(LOG_WARN, errout,
                     "NTRIP: Got unknown authenticatiion '%s'\n", s);
        }
    }
    /* <fee> */
    if ((s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->fee = atoi(s);
    }
    /* <bitrate> */
    if ((s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->bitrate = atoi(s);
    }
    /* ...<misc> */
    while ((s = ntrip_field_iterate(NULL, s, eol, errout)));
}

static int ntrip_sourcetable_parse(struct gps_device_t *device)
{
    struct ntrip_stream_t hold;
    ssize_t llen, len = 0;
    char *line;
    bool sourcetable = false;
    bool match = false;
    char buf[BUFSIZ];
    size_t blen = sizeof(buf);
    socket_t fd = device->gpsdata.gps_fd;

    for (;;) {
        char *eol;
        ssize_t rlen;

        memset(&buf[len], 0, (size_t) (blen - len));
        rlen = read(fd, &buf[len], (size_t)(blen - 1 - len));
        if (rlen == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (sourcetable && !match && errno == EAGAIN) {
                /* we have not yet found a match, but there currently
                 * is no more data */
                return 0;
            }
            if (match) {
                return 1;
            }
            GPSD_LOG(LOG_ERROR, &device->context->errout,
                     "ntrip stream read error %s on fd %d\n",
                     strerror(errno), fd);
            return -1;
        } else if (rlen == 0) { // server closed the connection
            GPSD_LOG(LOG_ERROR, &device->context->errout,
                     "ntrip stream unexpected close %s on fd %d "
                     "during sourcetable read\n",
                     strerror(errno), fd);
            return -1;
        }

        line = buf;
        rlen = len += rlen;
        line[rlen] = '\0';      // pacify coverity that this is NUL terminated

        GPSD_LOG(LOG_RAW, &device->context->errout,
                 "Ntrip source table buffer %s\n", buf);

        sourcetable = device->ntrip.sourcetable_parse;
        if (!sourcetable) {
            /* parse SOURCETABLE */
            if (str_starts_with(line, NTRIP_SOURCETABLE)) {
                sourcetable = true;
                device->ntrip.sourcetable_parse = true;
                llen = (ssize_t) strlen(NTRIP_SOURCETABLE);
                line += llen;
                len -= llen;
            } else {
                GPSD_LOG(LOG_WARN, &device->context->errout,
                         "Received unexpected Ntrip reply %s.\n",
                         buf);
                return -1;
            }
        }

        while (len > 0) {
            /* parse ENDSOURCETABLE */
            if (str_starts_with(line, NTRIP_ENDSOURCETABLE))
                goto done;

            eol = strstr(line, NTRIP_BR);
            if (NULL == eol){
                break;
            }

            GPSD_LOG(LOG_DATA, &device->context->errout,
                     "next Ntrip source table line %s\n", line);

            *eol = '\0';
            llen = (ssize_t) (eol - line);

            /* TODO: parse headers */

            /* parse STR */
            if (str_starts_with(line, NTRIP_STR)) {
                ntrip_str_parse(line + strlen(NTRIP_STR),
                                (size_t) (llen - strlen(NTRIP_STR)),
                                &hold, &device->context->errout);
                if (strcmp(device->ntrip.stream.mountpoint,
                           hold.mountpoint) == 0) {
                    /* TODO: support for RTCM 3.0, SBAS (WAAS, EGNOS), ... */
                    if (hold.format == fmt_unknown) {
                        GPSD_LOG(LOG_ERROR, &device->context->errout,
                                 "Ntrip stream %s format not supported\n",
                                 line);
                        return -1;
                    }
                    /* TODO: support encryption and compression algorithms */
                    if (hold.compr_encryp != cmp_enc_none) {
                        GPSD_LOG(LOG_ERROR, &device->context->errout,
                                 "Ntrip stream %s compression/encryption "
                                 "algorithm not supported\n",
                                 line);
                        return -1;
                    }
                    /* TODO: support digest authentication */
                    if (hold.authentication != auth_none
                            && hold.authentication != auth_basic) {
                        GPSD_LOG(LOG_ERROR, &device->context->errout,
                                 "Ntrip stream %s authentication method "
                                 "not supported\n",
                                line);
                        return -1;
                    }
                    /* no memcpy, so we can keep the other infos */
                    device->ntrip.stream.format = hold.format;
                    device->ntrip.stream.carrier = hold.carrier;
                    device->ntrip.stream.latitude = hold.latitude;
                    device->ntrip.stream.longitude = hold.longitude;
                    device->ntrip.stream.nmea = hold.nmea;
                    device->ntrip.stream.compr_encryp = hold.compr_encryp;
                    device->ntrip.stream.authentication = hold.authentication;
                    device->ntrip.stream.fee = hold.fee;
                    device->ntrip.stream.bitrate = hold.bitrate;
                    device->ntrip.stream.set = true;
                    match = true;
                }
                /* TODO: compare stream location to own location to
                 * find nearest stream if user hasn't provided one */
            }
            else if (str_starts_with(line, NTRIP_CAS)) {
                // TODO: parse CAS
                // See: http://software.rtcm-ntrip.org/wiki/CAS
                GPSD_LOG(LOG_WARN, &device->context->errout,
                         "NTRIP: Can't parse CAS '%s'\n", line);
            } else if (str_starts_with(line, NTRIP_NET)) {
                // TODO: parse NET
                // See: http://software.rtcm-ntrip.org/wiki/NET
                GPSD_LOG(LOG_WARN, &device->context->errout,
                         "NTRIP: Can't parse NET '%s'\n", line);
            }

            llen += strlen(NTRIP_BR);
            line += llen;
            len -= llen;
            GPSD_LOG(LOG_RAW, &device->context->errout,
                     "Remaining Ntrip source table buffer %zd %s\n", len,
                     line);
        }
        /* message too big to fit into buffer */
        if ((size_t)len == (blen - 1))
            return -1;

        if (len > 0)
            memmove(buf, &buf[rlen - len], (size_t) len);
    }

done:
    return match ? 1 : -1;
}

static int ntrip_stream_req_probe(const struct ntrip_stream_t *stream,
                                  struct gpsd_errout_t *errout)
{
    int dsock;
    ssize_t r;
    char buf[BUFSIZ];

    dsock = netlib_connectsock(AF_UNSPEC, stream->url, stream->port, "tcp");
    if (dsock < 0) {
        GPSD_LOG(LOG_ERROR, errout,
                 "ntrip stream connect error %d in req probe\n", dsock);
        return -1;
    }
    GPSD_LOG(LOG_SPIN, errout,
             "ntrip stream for req probe connected on fd %d\n", dsock);
    (void)snprintf(buf, sizeof(buf),
            "GET / HTTP/1.1\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "User-Agent: NTRIP gpsd/%s\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n", VERSION, stream->url);
    r = write(dsock, buf, strlen(buf));
    if (r != (ssize_t)strlen(buf)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "ntrip stream write error %s on fd %d "
                 "during probe request %zd\n",
                 strerror(errno), dsock, r);
        (void)close(dsock);
        return -1;
    }
    /* coverity[leaked_handle] This is an intentional allocation */
    return dsock;
}

static int ntrip_auth_encode(const struct ntrip_stream_t *stream,
                             const char *auth,
                             char buf[],
                             size_t size)
{
    memset(buf, 0, size);
    if (stream->authentication == auth_none)
        return 0;
    else if (stream->authentication == auth_basic) {
        char authenc[64];
        if (!auth)
            return -1;
        memset(authenc, 0, sizeof(authenc));
        if (b64_ntop
                ((unsigned char *)auth, strlen(auth), authenc,
                 sizeof(authenc) - 1) < 0)
            return -1;
        (void)snprintf(buf, size - 1, "Authorization: Basic %s\r\n", authenc);
    } else {
        /* TODO: support digest authentication */
    }
    return 0;
}

/* *INDENT-ON* */

static socket_t ntrip_stream_get_req(const struct ntrip_stream_t *stream,
                                     const struct gpsd_errout_t *errout)
{
    int dsock;
    char buf[BUFSIZ];

    dsock = netlib_connectsock(AF_UNSPEC, stream->url, stream->port, "tcp");
    if (BAD_SOCKET(dsock)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "ntrip stream connect error %d\n", dsock);
        return -1;
    }

    GPSD_LOG(LOG_SPIN, errout,
             "netlib_connectsock() returns socket on fd %d\n",
             dsock);

    (void)snprintf(buf, sizeof(buf),
            "GET /%s HTTP/1.1\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "User-Agent: NTRIP gpsd/%s\r\n"
            "Host: %s\r\n"
            "Accept: rtk/rtcm, dgps/rtcm\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n", stream->mountpoint, VERSION, stream->url, stream->authStr);
    if (write(dsock, buf, strlen(buf)) != (ssize_t) strlen(buf)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "ntrip stream write error %s on fd %d during get request\n",
                 strerror(errno), dsock);
        (void)close(dsock);
        return -1;
    }
    return dsock;
}

static int ntrip_stream_get_parse(const struct ntrip_stream_t *stream,
                                  const int dsock,
                                  const struct gpsd_errout_t *errout)
{
    char buf[BUFSIZ];
    int opts;
    memset(buf, 0, sizeof(buf));
    while (read(dsock, buf, sizeof(buf) - 1) == -1) {
        if (errno == EINTR)
            continue;
        GPSD_LOG(LOG_ERROR, errout,
                 "ntrip stream read error %s on fd %d during get rsp\n",
                 strerror(errno), dsock);
        goto close;
    }
    buf[sizeof(buf) - 1] = '\0';   // pacify coverity about NUL-terminated.

    /* parse 401 Unauthorized */
    if (NULL != strstr(buf, NTRIP_UNAUTH)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "not authorized for Ntrip stream %s/%s\n", stream->url,
                 stream->mountpoint);
        goto close;
    }
    /* parse SOURCETABLE */
    if (NULL != strstr(buf, NTRIP_SOURCETABLE)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "Broadcaster doesn't recognize Ntrip stream %s:%s/%s\n",
                 stream->url, stream->port, stream->mountpoint);
        goto close;
    }
    /* parse ICY 200 OK */
    if (NULL == strstr(buf, NTRIP_ICY)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "Unknown reply %s from Ntrip service %s:%s/%s\n", buf,
                 stream->url, stream->port, stream->mountpoint);
        goto close;
    }
    opts = fcntl(dsock, F_GETFL);

    if (opts >= 0)
        (void)fcntl(dsock, F_SETFL, opts | O_NONBLOCK);

    return dsock;
close:
    (void)close(dsock);
    return -1;
}

/* open a connection to a Ntrip broadcaster */
int ntrip_open(struct gps_device_t *device, char *orig)
{
    char *amp, *colon, *slash;
    char *auth = NULL, dup[256], *caster = dup;
    char *port = NULL;
    char *stream = NULL;
    char *url = NULL;
    socket_t ret = -1;

    switch (device->ntrip.conn_state) {
    case ntrip_conn_init:
        /* this has to be done here,
         * because it is needed for multi-stage connection */
        // strlcpy() ensures dup is NUL terminated.
        strlcpy(dup, orig, 255);
        device->servicetype = SERVICE_NTRIP;
        device->ntrip.works = false;
        device->ntrip.sourcetable_parse = false;
        device->ntrip.stream.set = false;

        /* Test cases
         * ntrip://userid:passwd@ntrip.com:2101/MOUNT-POINT
         * ntrip://a@b.com:passwd@ntrip.com:2101/MOUNT-POINT
         * ntrip://userid:passwd@@@ntrip.com:2101/MOUNT-POINT
         * ntrip://a@b.com:passwd@@@ntrip.com:2101/MOUNT-POINT */
        if ((amp = strrchr(caster, '@')) != NULL) {
            if (((colon = strchr(caster, ':')) != NULL) && colon < amp) {
                auth = caster;
                *amp = '\0';
                caster = amp + 1;
                url = caster;
            }
        }
        if ((slash = strchr(caster, '/')) != NULL) {
            *slash = '\0';
            stream = slash + 1;
        } else {
            /* TODO: add autoconnect like in dgpsip.c */
            GPSD_LOG(LOG_ERROR, &device->context->errout,
                     "can't extract Ntrip stream from %s\n",
                     caster);
            device->ntrip.conn_state = ntrip_conn_err;
            return -1;
        }
        if ((colon = strchr(caster, ':')) != NULL) {
            port = colon + 1;
            *colon = '\0';
        }

        if (NULL == url) {
            // there was no @ in caster
            url = caster;
        }
        if (!port) {
            port = "rtcm-sc104";
            if (!getservbyname(port, "tcp"))
                port = DEFAULT_RTCM_PORT;
        }

        (void)strlcpy(device->ntrip.stream.mountpoint,
                stream,
                sizeof(device->ntrip.stream.mountpoint));
        if (auth != NULL)
            (void)strlcpy(device->ntrip.stream.credentials,
                          auth,
                          sizeof(device->ntrip.stream.credentials));
        /*
         * Semantically url and port ought to be non-NULL by now,
         * but just in case...this code appeases Coverity.
         */
        if (url != NULL)
            (void)strlcpy(device->ntrip.stream.url,
                          url,
                          sizeof(device->ntrip.stream.url));
        if (port != NULL)
            (void)strlcpy(device->ntrip.stream.port,
                          port,
                          sizeof(device->ntrip.stream.port));

        ret = ntrip_stream_req_probe(&device->ntrip.stream,
                                     &device->context->errout);
        if (ret == -1) {
            device->ntrip.conn_state = ntrip_conn_err;
            return -1;
        }
        device->gpsdata.gps_fd = ret;
        device->ntrip.conn_state = ntrip_conn_sent_probe;
        return ret;
    case ntrip_conn_sent_probe:
        ret = ntrip_sourcetable_parse(device);
        if (ret == -1) {
            device->ntrip.conn_state = ntrip_conn_err;
            return -1;
        }
        if (ret == 0 && device->ntrip.stream.set == false) {
            return ret;
        }
        (void)close(device->gpsdata.gps_fd);
        if (ntrip_auth_encode(&device->ntrip.stream,
                              device->ntrip.stream.credentials,
                              device->ntrip.stream.authStr,
                              sizeof(device->ntrip.stream.authStr)) != 0) {
            device->ntrip.conn_state = ntrip_conn_err;
            return -1;
        }
        ret = ntrip_stream_get_req(&device->ntrip.stream,
                                   &device->context->errout);
        if (ret == -1) {
            device->ntrip.conn_state = ntrip_conn_err;
            return -1;
        }
        device->gpsdata.gps_fd = ret;
        device->ntrip.conn_state = ntrip_conn_sent_get;
        break;
    case ntrip_conn_sent_get:
        ret = ntrip_stream_get_parse(&device->ntrip.stream,
                                     device->gpsdata.gps_fd,
                                     &device->context->errout);
        if (ret == -1) {
            device->ntrip.conn_state = ntrip_conn_err;
            return -1;
        }
        device->ntrip.conn_state = ntrip_conn_established;
        device->ntrip.works = true; // we know, this worked.
        break;
    case ntrip_conn_established:
    case ntrip_conn_err:
        return -1;
    }
    return ret;
}

/* may be time to ship a usage report to the Ntrip caster */
void ntrip_report(struct gps_context_t *context,
                  struct gps_device_t *gps,
                  struct gps_device_t *caster)
{
    static int count;
    /*
     * 10 is an arbitrary number, the point is to have gotten several good
     * fixes before reporting usage to our Ntrip caster.
     *
     * count % 5 is as arbitrary a number as the fixcnt. But some delay
     * was needed here
     */
    count ++;
    if (caster->ntrip.stream.nmea != 0 &&
        context->fixcnt > 10 && (count % 5) == 0) {
        if (caster->gpsdata.gps_fd > -1) {
            char buf[BUFSIZ];
            gpsd_position_fix_dump(gps, buf, sizeof(buf));
            if (write(caster->gpsdata.gps_fd, buf, strlen(buf)) ==
                    (ssize_t) strlen(buf)) {
                GPSD_LOG(LOG_IO, &context->errout, "=> dgps %s\n", buf);
            } else {
                GPSD_LOG(LOG_IO, &context->errout,
                         "ntrip report write failed\n");
            }
        }
    }
}

// vim: set expandtab shiftwidth=4

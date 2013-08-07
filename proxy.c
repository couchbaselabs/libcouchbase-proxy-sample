/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#define _XOPEN_SOURCE

#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <libcouchbase/couchbase.h>
#include "ringbuffer.h"
#include "protocol_binary.h"

#define BUFFER_SIZE 1024

struct lcb_create_st opts;
int port;

void
info(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void
fail(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void
fail_e(lcb_error_t err, const char *fmt, ...)
{
    int en = errno;
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (en) {
        fprintf(stderr, " (0x%02x: %s)", en, strerror(en));
    }
    if (err != 0) {
        fprintf(stderr, " (0x%02x: %s)", err, lcb_strerror(NULL, err));
    }
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void usage(void)
{
    fprintf(stderr,
            "proxy [-?] [-p port] [-h endpoint] [-b bucket] [-u user] [-P password]\n"
            "\t-?\tshow this help\n"
            "\t-p\tport to listen at (default: 1987)\n"
            "\t-h\tcluster endpoint (default: 'example.com:8091')\n"
            "\t-b\tbucket name (default: 'default')\n"
            "\t-u\tuser name (default: none)\n"
            "\t-P\tpassword (default: none)\n");

}

void
scan_options(int argc, char *argv[])
{
    struct lcb_create_io_ops_st io_opts;
    lcb_error_t err;
    int c;

    io_opts.version = 0;
    io_opts.v.v0.type = LCB_IO_OPS_DEFAULT;
    io_opts.v.v0.cookie = NULL;
    port = 1987;
    opts.version = 0;
    opts.v.v0.host = "localhost:8091";
    opts.v.v0.bucket = "default";
    opts.v.v0.user = NULL;
    opts.v.v0.passwd = NULL;

    err = lcb_create_io_ops(&opts.v.v0.io, &io_opts);
    if (err != LCB_SUCCESS) {
        fail_e(err, "lcb_create_io_ops()");
    }
    if (opts.v.v0.io->version != 0) {
        fail_e(err, "this application designed to use v0 IO layout");
    }

    while ((c = getopt(argc, argv, "?p:h:b:u:P:")) != -1) {
        switch (c) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'h':
            opts.v.v0.host = optarg;
            break;
        case 'b':
            opts.v.v0.bucket = optarg;
            break;
        case 'u':
            opts.v.v0.user = optarg;
            break;
        case 'P':
            opts.v.v0.passwd = optarg;
            break;
        default:
            usage();
            if (optopt) {
                exit(EXIT_FAILURE);
            } else {
                exit(EXIT_SUCCESS);
            }
        }
    }
    if (optind < argc) {
        usage();
        fail("unexpected arguments");
    }
}

void run_proxy(lcb_t conn);

int
main(int argc, char *argv[])
{
    lcb_error_t err;
    lcb_t conn = NULL;

    scan_options(argc, argv);
    err = lcb_create(&conn, &opts);
    if (err != LCB_SUCCESS) {
        fail_e(err, "lcb_create()");
    }
    info("connecting to bucket \"%s\" at \"%s\"",
         opts.v.v0.bucket, opts.v.v0.host);
    if (opts.v.v0.user) {
        info("\tas user \"%s\"", opts.v.v0.user);
    }
    if (opts.v.v0.passwd) {
        info("\twith password \"%s\"", opts.v.v0.passwd);
    }
    err = lcb_connect(conn);
    if (err != LCB_SUCCESS) {
        fail_e(err, "lcb_connect()");
    }
    err = lcb_wait(conn);
    if (err != LCB_SUCCESS) {
        fail_e(err, "lcb_connect()");
    }

    run_proxy(conn);

    return EXIT_SUCCESS;
}

void proxy_client_callback(lcb_socket_t sock, short which, void *data);

typedef struct server_st server_t;
struct server_st {
    lcb_t conn;
    lcb_io_opt_t io;
    int sock;
    void *event;
};

typedef struct client_st client_t;
struct client_st {
    server_t *server;
    int sock;
    ringbuffer_t in;
    ringbuffer_t out;
    void *event;
};

typedef struct cookie_st cookie_t;
struct cookie_st {
    lcb_uint32_t opaque;
    client_t *client;
};

void
error_callback(lcb_t conn, lcb_error_t err, const char *info)
{
    fail_e(err, info);
    (void)conn;
}


protocol_binary_response_status
map_status(lcb_error_t err)
{
    switch (err) {
    case LCB_SUCCESS:
        return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    case LCB_AUTH_CONTINUE:
        return PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE;
    case LCB_AUTH_ERROR:
        return PROTOCOL_BINARY_RESPONSE_AUTH_ERROR;
    case LCB_DELTA_BADVAL:
        return PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL;
    case LCB_E2BIG:
        return PROTOCOL_BINARY_RESPONSE_E2BIG;
    case LCB_EBUSY:
        return PROTOCOL_BINARY_RESPONSE_EBUSY;
    case LCB_EINVAL:
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    case LCB_ENOMEM:
        return PROTOCOL_BINARY_RESPONSE_ENOMEM;
    case LCB_ERANGE:
        return PROTOCOL_BINARY_RESPONSE_ERANGE;
    case LCB_ETMPFAIL:
        return PROTOCOL_BINARY_RESPONSE_ETMPFAIL;
    case LCB_KEY_EEXISTS:
        return PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
    case LCB_KEY_ENOENT:
        return PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
    case LCB_NOT_MY_VBUCKET:
        return PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET;
    case LCB_NOT_STORED:
        return PROTOCOL_BINARY_RESPONSE_NOT_STORED;
    case LCB_NOT_SUPPORTED:
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    case LCB_UNKNOWN_COMMAND:
        return PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND;
    default:
        return PROTOCOL_BINARY_RESPONSE_EINTERNAL;
    }
}

void
get_callback(lcb_t conn, const void *cookie, lcb_error_t err,
             const lcb_get_resp_t *item)
{
    cookie_t *c = (cookie_t *)cookie;
    client_t *cl = c->client;
    lcb_io_opt_t io = cl->server->io;
    protocol_binary_response_get res;

    res.message.header.response.magic = PROTOCOL_BINARY_RES;
    res.message.header.response.opcode = PROTOCOL_BINARY_CMD_GET;
    res.message.header.response.keylen = 0;
    res.message.header.response.extlen = 4;
    res.message.header.response.datatype = item->v.v0.datatype;
    res.message.header.response.status = map_status(err);
    res.message.header.response.bodylen = htonl(4 + item->v.v0.nbytes);
    res.message.header.response.opaque = c->opaque;
    res.message.header.response.cas = item->v.v0.cas;
    res.message.body.flags = htonl(item->v.v0.flags);
    ringbuffer_ensure_capacity(&cl->out, sizeof(res.bytes) + item->v.v0.nbytes);
    ringbuffer_write(&cl->out, res.bytes, sizeof(res.bytes));
    ringbuffer_write(&cl->out, item->v.v0.bytes, item->v.v0.nbytes);
    io->v.v0.update_event(io, cl->sock, cl->event,
                          LCB_WRITE_EVENT, cl,
                          proxy_client_callback);
    (void)conn;
}

void
store_callback(lcb_t conn, const void *cookie,
               lcb_storage_t operation,
               lcb_error_t err,
               const lcb_store_resp_t *item)
{
    cookie_t *c = (cookie_t *)cookie;
    client_t *cl = c->client;
    lcb_io_opt_t io = cl->server->io;
    protocol_binary_response_set res;

    res.message.header.response.magic = PROTOCOL_BINARY_RES;
    res.message.header.response.opcode = PROTOCOL_BINARY_CMD_SET;
    res.message.header.response.keylen = 0;
    res.message.header.response.extlen = 0;
    /* lcb_store_resp_t doesn't carry datatype */
    res.message.header.response.datatype = PROTOCOL_BINARY_RAW_BYTES;
    res.message.header.response.status = map_status(err);
    res.message.header.response.bodylen = 0;
    res.message.header.response.opaque = c->opaque;
    res.message.header.response.cas = item->v.v0.cas;
    ringbuffer_ensure_capacity(&cl->out, sizeof(res));
    ringbuffer_write(&cl->out, res.bytes, sizeof(res));
    io->v.v0.update_event(io, cl->sock, cl->event,
                          LCB_WRITE_EVENT, cl,
                          proxy_client_callback);
    (void)operation;
    (void)conn;
}

char notsup_msg[] = "Not supported";

void
handle_packet(client_t *cl, char *buf)
{
    protocol_binary_request_header *req = (void *)buf;
    protocol_binary_response_header res;
    union {
        lcb_get_cmd_t get;
        lcb_store_cmd_t set;
    } cmd;
    union {
        const lcb_get_cmd_t *get[1];
        const lcb_store_cmd_t *set[1];
    } cmds;
    cookie_t *cookie;

    cookie = malloc(sizeof(cookie_t));
    if (cookie == NULL) {
        fail("cannot allocate buffer for command cookie");
    }
    cookie->client = cl;
    cookie->opaque = req->request.opaque;
    memset(&cmd, 0, sizeof(cmd));
    switch (req->request.opcode) {
    case PROTOCOL_BINARY_CMD_GET:
        cmds.get[0] = &cmd.get;
        cmd.get.v.v0.nkey = ntohs(req->request.keylen);
        cmd.get.v.v0.key = buf + sizeof(*req);
        info("[%p] get \"%.*s\"", (void *)cl,
             (int)cmd.get.v.v0.nkey, (char *)cmd.get.v.v0.key);
        lcb_get(cl->server->conn, (const void*)cookie, 1, cmds.get);
        break;
    case PROTOCOL_BINARY_CMD_SET:
        cmds.set[0] = &cmd.set;
        cmd.set.v.v0.operation = LCB_SET;
        cmd.set.v.v0.nkey = ntohs(req->request.keylen);
        cmd.set.v.v0.key = buf + sizeof(*req) + 8;
        cmd.set.v.v0.nbytes = ntohl(req->request.bodylen) - 8 - ntohs(req->request.keylen);
        cmd.set.v.v0.bytes = buf + sizeof(*req) + 8 + ntohs(req->request.keylen);
        cmd.set.v.v0.cas = req->request.cas;
        cmd.set.v.v0.datatype = req->request.datatype;
        memcpy(&cmd.set.v.v0.flags, buf + sizeof(*req), 4);
        cmd.set.v.v0.flags = ntohl(cmd.set.v.v0.flags);
        memcpy(&cmd.set.v.v0.exptime, buf + sizeof(*req) + 4, 4);
        cmd.set.v.v0.exptime = ntohl(cmd.set.v.v0.exptime);
        info("[%p] set \"%.*s\"", (void *)cl,
             (int)cmd.set.v.v0.nkey, (char *)cmd.set.v.v0.key);
        lcb_store(cl->server->conn, (const void*)cookie, 1, cmds.set);
        break;
    default:
        free(cookie);
        info("[%p] not supported command", (void *)cl);
        res.response.magic = PROTOCOL_BINARY_RES;
        res.response.opcode = req->request.opcode;
        res.response.keylen = 0;
        res.response.extlen = 0;
        res.response.datatype = PROTOCOL_BINARY_RAW_BYTES;
        res.response.status = PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
        res.response.bodylen = htonl(sizeof(notsup_msg));
        res.response.opaque = req->request.opaque;
        res.response.cas = 0;
        ringbuffer_ensure_capacity(&cl->out, sizeof(notsup_msg) + sizeof(res));
        ringbuffer_write(&cl->out, res.bytes, sizeof(res));
        ringbuffer_write(&cl->out, notsup_msg, sizeof(notsup_msg));
    }
}

void
proxy_client_callback(lcb_socket_t sock, short which, void *data)
{
    struct lcb_iovec_st iov[2];
    ssize_t rv;
    lcb_io_opt_t io;
    client_t *cl = data;

    io = cl->server->io;
    if (which & LCB_READ_EVENT) {
        while (1) {
            ringbuffer_ensure_capacity(&cl->in, BUFFER_SIZE);
            ringbuffer_get_iov(&cl->in, RINGBUFFER_WRITE, iov);
            rv = io->v.v0.recvv(io, cl->sock, iov, 2);
            if (rv == -1) {
                if (io->v.v0.error == EINTR) {
                    /* interrupted by signal */
                    continue;
                } else if (io->v.v0.error == EWOULDBLOCK) {
                    /* nothing to read right now */
                    io->v.v0.update_event(io, cl->sock, cl->event,
                                          LCB_WRITE_EVENT, cl,
                                          proxy_client_callback);
                    break;
                } else {
                    fail("read error");
                }
            } else if (rv == 0) {
                io->v.v0.destroy_event(io, cl->event);
                ringbuffer_destruct(&cl->in);
                ringbuffer_destruct(&cl->out);
                free(cl);
                info("[%p] disconnected", (void *)cl);
                return;
            } else {
                protocol_binary_request_header req;
                lcb_size_t nr, sz;
                char *buf;

                ringbuffer_produced(&cl->in, rv);
                if (ringbuffer_ensure_alignment(&cl->in) != 0) {
                    fail("cannot align the buffer");
                }
                nr = ringbuffer_peek(&cl->in, req.bytes, sizeof(req));
                if (nr < sizeof(req)) {
                    continue;
                }
                sz = ntohl(req.request.bodylen) + sizeof(req);
                if (cl->in.nbytes < sz) {
                    continue;
                }
                buf = malloc(sizeof(char) * sz);
                if (buf == NULL) {
                    fail("cannot allocate buffer for packet");
                }
                nr = ringbuffer_read(&cl->in, buf, sz);
                if (nr < sizeof(req)) {
                    fail("input buffer doesn't contain enough data");
                }
                handle_packet(cl, buf);
            }
        }
    }
    if (which & LCB_WRITE_EVENT) {
        ringbuffer_get_iov(&cl->out, RINGBUFFER_READ, iov);
        if (iov[0].iov_len + iov[1].iov_len == 0) {
            io->v.v0.delete_event(io, cl->sock, cl->event);
            return;
        }
        rv = io->v.v0.sendv(io, cl->sock, iov, 2);
        if (rv < 0) {
            fail("write error");
        } else if (rv == 0) {
            io->v.v0.destroy_event(io, cl->event);
            ringbuffer_destruct(&cl->in);
            ringbuffer_destruct(&cl->out);
            free(cl);
            info("write: peer disconnected");
            return;
        } else {
            ringbuffer_consumed(&cl->out, rv);
            io->v.v0.update_event(io, cl->sock, cl->event,
                                  LCB_READ_EVENT, cl,
                                  proxy_client_callback);
        }
    }
    (void)sock;
}

void
proxy_accept_callback(lcb_socket_t sock, short which, void *data)
{
    server_t *sv = data;
    client_t *cl;
    lcb_io_opt_t io;
    struct sockaddr_in addr;
    socklen_t naddr;
    int flags;

    if (!(which & LCB_READ_EVENT)) {
        fail("got invalid event");
    }
    io = sv->io;
    cl = malloc(sizeof(client_t));
    if (cl == NULL) {
        fail("failed to allocate memory for client");
    }
    if (ringbuffer_initialize(&cl->in, BUFFER_SIZE) == 0) {
        fail("failed to allocate input buffer for client");
    }
    if (ringbuffer_initialize(&cl->out, BUFFER_SIZE) == 0) {
        fail("failed to allocate output buffer for client");
    }
    naddr = sizeof(addr);
    cl->server = sv;
    cl->sock = accept((int)sock, (struct sockaddr *)&addr, &naddr);
    if (cl->sock == -1) {
        fail("accept()");
    }
    if ((flags = fcntl(cl->sock, F_GETFL, NULL)) == -1) {
        fail("fcntl(F_GETFL)");
    }
    if (fcntl(cl->sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        fail("fcntl(F_SETFL, O_NONBLOCK)");
    }
    cl->event = io->v.v0.create_event(io);
    if (cl->event == NULL) {
        fail("failed to create event for client");
    }
    info("[%p] connected", (void *)cl);
    io->v.v0.update_event(io, cl->sock, cl->event, LCB_READ_EVENT, cl,
                          proxy_client_callback);
}

void
run_proxy(lcb_t conn)
{
    lcb_io_opt_t io;
    struct sockaddr_in addr;
    int sock, rv;
    server_t server;

    io = opts.v.v0.io;
    info("starting proxy on port %d", port);
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        fail("socket()", strerror(errno));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    rv = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rv == -1) {
        fail("bind()");
    }
    rv = listen(sock, 10);
    if (rv == -1) {
        fail("listen()");
    }
    server.conn = conn;
    server.io = io;
    server.event = io->v.v0.create_event(io);
    if (server.event == NULL) {
        fail("failed to create event for proxy");
    }
    io->v.v0.update_event(io, sock, server.event, LCB_READ_EVENT,
                          &server, proxy_accept_callback);

    lcb_set_error_callback(conn, error_callback);
    lcb_set_get_callback(conn, get_callback);
    lcb_set_store_callback(conn, store_callback);
    io->v.v0.run_event_loop(io);
}

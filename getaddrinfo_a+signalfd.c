/*
 * This example resolves the hostnames supplied as command arguments using
 * getaddrinfo_a(3) and signalfd(2).
 *
 * To compile: gcc -Wall getaddrinfo_a+signalfd.c -lanl
 */
/*
 * Copyright (c) 2015 DeNA Co., Ltd., Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static size_t reqs_pending = 0;

static int send_request(const char *node, const char *service, int socktype, int protocol, int flags)
{
    size_t node_len = strlen(node) + 1, service_len = service != NULL ? strlen(service) + 1 : 0;
    struct {
      struct gaicb gaicb;
      struct addrinfo hints;
      struct gaicb *list[1];
      char bufs[0];
    } *req = malloc(sizeof(*req) + node_len + service_len);

    req->list[0] = &req->gaicb;
    req->gaicb = (struct gaicb){};
    req->gaicb.ar_name = req->bufs;
    memcpy((char *)req->gaicb.ar_name, node, node_len);
    if (service != NULL) {
        req->gaicb.ar_service = req->bufs + node_len;
        memcpy((char *)req->gaicb.ar_service, service, service_len);
    }
    req->gaicb.ar_request = &req->hints;

    req->hints = (struct addrinfo){};
    req->hints.ai_socktype = socktype;
    req->hints.ai_protocol = protocol;
    req->hints.ai_flags = flags;

    struct sigevent sev = {};
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &req->gaicb;

    int ret = getaddrinfo_a(GAI_NOWAIT, req->list, 1, &sev);
    if (ret == 0)
        ++reqs_pending;
    return ret;
}

static void handle_response(struct gaicb *req)
{
    int ret;

    if ((ret = gai_error(req)) != 0) {
        if (ret == EAI_INPROGRESS)
            return;
        fprintf(stderr, "getaddrinfo_a failed:%s\n", gai_strerror(ret));
        goto Done;
    }

    assert(req->ar_result != NULL);

    printf("%s:", req->ar_name);
    struct addrinfo *ai = req->ar_result;
    do {
        char host[NI_MAXHOST];
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST) == 0) {
            printf(" %s", host);
        } else {
            fprintf(stderr, "could not convert response from getaddrinfo_a to string\n");
            abort();
        }
    } while ((ai = ai->ai_next) != NULL);
    printf("\n");

    freeaddrinfo(req->ar_result);
Done:
    free(req);
    if (--reqs_pending == 0)
        exit(0);
}

static int setup_signalfd(void)
{
    sigset_t sigs;

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
 
    return signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
}

static void handle_sigs_via_fd(int sfd)
{
    struct signalfd_siginfo ssi;
    ssize_t rret;

Again:
    while ((rret = read(sfd, &ssi, sizeof(ssi))) == -1 && errno == EINTR)
        ;
    if (rret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        perror("failed to read signalfd");
        abort();
    } else if (rret != sizeof(ssi)) {
        fprintf(stderr, "unexpected number of bytes read from signalfd (%zd)\n", rret);
        abort();
    }

    if (ssi.ssi_code == SI_ASYNCNL) {
        /* received response from getaddrinfo_a */
        struct gaicb *req = (void *)ssi.ssi_ptr;
        handle_response(req);
        goto Again;
    }

    fprintf(stderr, "received signal with unexpected si_code:%" PRId32 "\n", ssi.ssi_code);
    abort();
}

int main(int argc, char **argv)
{
    int sfd = setup_signalfd(), i;

    if (argc == 1) {
        fprintf(stderr, "Usage: %s host1 host2 ...\n", argv[0]);
        return 1;
    }
    for (i = 1; i != argc; ++i)
        send_request(argv[i], NULL, SOCK_STREAM, IPPROTO_TCP, AI_ADDRCONFIG);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        if (select(sfd + 1, &rfds, NULL, NULL, NULL) > 0) {
            if (FD_ISSET(sfd, &rfds))
                handle_sigs_via_fd(sfd);
        }
    }

    return 0;
}

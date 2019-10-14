//
// Created by wong on 10/24/18.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "mem-allocator.h"
#include "util.h"
#include "log.h"
#include "context.h"
#include "signal.h"
#include "listener-tcp.h"
#include "listener-udp.h"
#include "pump.h"
#include "transparent-method.h"
#include "netutils.h"


static transocks_global_env *globalEnv = NULL;

int main(int argc, char **argv) {
    int opt;

    char *tcpListenerAddrPort = NULL;
    char *udpListenerAddrPort = NULL;
    char *socks5AddrPort = NULL;
    char *pumpMethod = NULL;
    char *transparentMethod = NULL;

    struct sockaddr_storage tcp_listener_ss;
    socklen_t tcp_listener_ss_size;
    struct sockaddr_storage udp_listener_ss;
    socklen_t udp_listener_ss_size;
    struct sockaddr_storage socks5_ss;
    socklen_t socks5_ss_size;

    static struct option long_options[] = {
            {
                    .name = "tcp-listener-addr-port",
                    .has_arg = required_argument,
                    .flag = NULL,
                    .val = GETOPT_VAL_TCPLISTENERADDRPORT
            },
            {
                    .name = "udp-listener-addr-port",
                    .has_arg = required_argument,
                    .flag = NULL,
                    .val = GETOPT_VAL_UDPLISTENERADDRPORT
            },
            {
                    .name = "socks5-addr-port",
                    .has_arg = required_argument,
                    .flag = NULL,
                    .val = GETOPT_VAL_SOCKS5ADDRPORT
            },
            {
                    .name = "pump-method",
                    .has_arg = optional_argument,
                    .flag = NULL,
                    .val = GETOPT_VAL_PUMPMETHOD
            },
            {
                    .name = "transparent-method",
                    .has_arg = required_argument,
                    .flag = NULL,
                    .val = GETOPT_VAL_TRANSPARENTMETHOD
            },
            {
                    .name = "help",
                    .has_arg = no_argument,
                    .flag = NULL,
                    .val = GETOPT_VAL_HELP
            },
            {
                    .name = NULL,
                    .has_arg = 0,
                    .flag = NULL,
                    .val = 0
            }
    };

    while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {
            case GETOPT_VAL_TCPLISTENERADDRPORT:
                tcpListenerAddrPort = optarg;
                break;
            case GETOPT_VAL_UDPLISTENERADDRPORT:
                udpListenerAddrPort = optarg;
                break;
            case GETOPT_VAL_SOCKS5ADDRPORT:
                socks5AddrPort = optarg;
                break;
            case GETOPT_VAL_PUMPMETHOD:
                pumpMethod = optarg;
                break;
            case GETOPT_VAL_TRANSPARENTMETHOD:
                transparentMethod = optarg;
                break;
            case '?':
            case 'h':
            case GETOPT_VAL_HELP:
                PRINTHELP_EXIT();
            default:
                PRINTHELP_EXIT();
        }
    }

    if (tcpListenerAddrPort == NULL
    || udpListenerAddrPort == NULL
        || socks5AddrPort == NULL) {
        PRINTHELP_EXIT();
    }

    if (pumpMethod == NULL) {
        pumpMethod = PUMPMETHOD_BUFFER;
    }

    if (transparentMethod == NULL) {
        transparentMethod = TRANSPARENTMETHOD_REDIRECT;
    }

    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    if (transocks_parse_sockaddr_port(tcpListenerAddrPort, (struct sockaddr *) &tcp_listener_ss, &tcp_listener_ss_size) != 0) {
        FATAL_WITH_HELPMSG("invalid tcp listener address and port: %s", tcpListenerAddrPort);
    }
    if (transocks_parse_sockaddr_port(udpListenerAddrPort, (struct sockaddr *) &udp_listener_ss, &udp_listener_ss_size) != 0) {
        FATAL_WITH_HELPMSG("invalid udp listener address and port: %s", udpListenerAddrPort);
    }
    if (transocks_parse_sockaddr_port(socks5AddrPort, (struct sockaddr *) &socks5_ss, &socks5_ss_size) != 0) {
        FATAL_WITH_HELPMSG("invalid socks5 address and port: %s", socks5AddrPort);
    }

    // check if port exists
    if (!validate_addr_port(&tcp_listener_ss)) {
        FATAL_WITH_HELPMSG("fail to parse tcp listener address port: %s", tcpListenerAddrPort);
    }
    if (!validate_addr_port(&udp_listener_ss)) {
        FATAL_WITH_HELPMSG("fail to parse udp listener address port: %s", udpListenerAddrPort);
    }
    if (!validate_addr_port(&socks5_ss)) {
        FATAL_WITH_HELPMSG("fail to parse socks5 address port: %s", socks5AddrPort);
    }


    globalEnv = transocks_global_env_new();
    if (globalEnv == NULL) {
        goto bareExit;
    }
    if (signal_init(globalEnv) != 0) {
        goto shutdown;
    }
    memcpy(globalEnv->tcpBindAddr, &tcp_listener_ss, sizeof(struct sockaddr_storage));
    globalEnv->tcpBindAddrLen = tcp_listener_ss_size;
    memcpy(globalEnv->udpBindAddr, &udp_listener_ss, sizeof(struct sockaddr_storage));
    globalEnv->udpBindAddrLen = udp_listener_ss_size;
    memcpy(globalEnv->relayAddr, &socks5_ss, sizeof(struct sockaddr_storage));
    globalEnv->relayAddrLen = socks5_ss_size;

    globalEnv->pumpMethodName = tr_strdup(pumpMethod);
    if (globalEnv->pumpMethodName == NULL) {
        goto shutdown;
    }

    if (transocks_pump_init(globalEnv) != 0) {
        goto shutdown;
    }

    globalEnv->transparentMethodName = tr_strdup(transparentMethod);
    if (globalEnv->transparentMethodName == NULL) {
        goto shutdown;
    }

    if (transocks_transparent_method_init(globalEnv) != 0) {
        goto shutdown;
    }
    // TODO: a generic listener for both TCP and UDP
    if (tcp_listener_init(globalEnv) != 0) {
        goto shutdown;
    }
    if (udp_listener_init(globalEnv) != 0) {
        goto shutdown;
    }

    LOGI("transocks-wong started");
    LOGI("using memory allocator: "
                 TR_USED_MEM_ALLOCATOR);
    LOGI("using pump method: %s", globalEnv->pumpMethodName);
    LOGI("using transparent method: %s", globalEnv->transparentMethodName);

    // start event loop
    event_base_dispatch(globalEnv->eventBaseLoop);

    LOGI("exited event loop, shutting down..");

    shutdown:

    // exit gracefully
    transocks_drop_all_clients(globalEnv);
    // report intentional event loop break
    if (event_base_got_exit(globalEnv->eventBaseLoop)
        || event_base_got_break(globalEnv->eventBaseLoop)) {
        LOGE("exited event loop intentionally");
    }
    // we are done, bye
    TRANSOCKS_FREE(transocks_global_env_free, globalEnv);

    bareExit:
    return 0;
}
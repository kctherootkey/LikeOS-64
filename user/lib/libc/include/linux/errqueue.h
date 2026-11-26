/*
 * Copyright (C) 2026 The LikeOS Project
 *
 * Extended socket errors: what recvmsg(MSG_ERRQUEUE) returns as an
 * IP_RECVERR control message, followed by the address of whoever reported
 * the error (SO_EE_OFFENDER).
 */

#ifndef _LIKEOS_UAPI_ERRQUEUE_H
#define _LIKEOS_UAPI_ERRQUEUE_H

#include <stdint.h>

struct sock_extended_err {
    uint32_t ee_errno;  /* the error */
    uint8_t  ee_origin; /* SO_EE_ORIGIN_* */
    uint8_t  ee_type;   /* ICMP type */
    uint8_t  ee_code;   /* ICMP code */
    uint8_t  ee_pad;
    uint32_t ee_info;   /* EMSGSIZE: the MTU */
    uint32_t ee_data;
};

#define SO_EE_ORIGIN_NONE  0
#define SO_EE_ORIGIN_LOCAL 1 /* this host found the error */
#define SO_EE_ORIGIN_ICMP  2 /* an ICMP message reported it */
#define SO_EE_ORIGIN_ICMP6 3

/* The sockaddr of the reporter, right after the structure. */
#define SO_EE_OFFENDER(ee) ((struct sockaddr *)((ee) + 1))

#endif /* _LIKEOS_UAPI_ERRQUEUE_H */

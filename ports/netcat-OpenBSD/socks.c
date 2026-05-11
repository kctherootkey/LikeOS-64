/* $OpenBSD: socks.c,v 1.30 2024/02/08 20:06:47 tb Exp $ */

/*
 * Copyright (c) 1999 Niklas Hallqvist.  All rights reserved.
 * Copyright (c) 2004, 2007 Damien Miller.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* LikeOS port: pull in compatibility shims before system headers. */
#ifdef __LIKEOS__
#include "compat_likeos.h"
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __LIKEOS__
#include <readpassphrase.h>
#include <resolv.h>
#endif

#include "atomicio.h"

/* forward declaration — defined in netcat.c and linked together */
int remote_connect(const char *, const char *, struct addrinfo, char *);

#define SOCKS_PORT	"1080"
#define HTTP_PROXY_PORT	"3128"
#define HTTP_MAXHDRS	64
#define SOCKS_V5	5
#define SOCKS_V4	4
#define SOCKS_V4A	44	/* nonstandard: 4A requests use 4 in wire fmt */
#define SOCKS_NOAUTH	0
#define SOCKS_NOMETHOD	0xff
#define SOCKS_CONNECT	1
#define SOCKS_IPV4	1
#define SOCKS_DOMAIN	3
#define SOCKS_IPV6	4

int	socks_connect(const char *, const char *, struct addrinfo,
	    const char *, const char *, struct addrinfo, int, const char *);

static int
decode_addrport(const char *h, const char *p, struct sockaddr *addr,
    socklen_t addrlen, int v4only, int numeric)
{
	int r;
	struct addrinfo hints, *res;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = v4only ? AF_INET : AF_UNSPEC;
	hints.ai_flags = numeric ? AI_NUMERICHOST : 0;
	hints.ai_socktype = SOCK_STREAM;
	r = getaddrinfo(h, p, &hints, &res);
	/* Don't fatal when a host isn't found, just return -1 */
	if (r != 0) {
		/* DEAD: errx(1, "%s", gai_strerror(r)); */
		return -1;
	}
	if (addrlen < res->ai_addrlen) {
		freeaddrinfo(res);
		return -1;
	}
	memcpy(addr, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	return 0;
}

static int
proxy_read_line(int fd, char *buf, size_t bufsz)
{
	size_t off;
	char c;

	for (off = 0;;) {
		if (atomicio(read, fd, &c, 1) != 1)
			err(1, "read");
		if (c == '\n' && off > 0 && buf[off - 1] == '\r')
			buf[--off] = '\0';
		else if (off < bufsz - 1) {
			buf[off++] = c;
			buf[off] = '\0';
		}
		if (c == '\n')
			break;
	}
	return (int)off;
}

static const char *
getproxypass(const char *proxyuser, const char *proxyhost)
{
	char prompt[512];
	static char pw[256];

	snprintf(prompt, sizeof(prompt), "Proxy password for %s@%s: ",
	    proxyuser, proxyhost);
	if (readpassphrase(prompt, pw, sizeof(pw), RPP_REQUIRE_TTY) == NULL)
		errx(1, "Unable to read proxy passphrase");
	return pw;
}

int
socks_connect(const char *host, const char *port,
    struct addrinfo hints __attribute__((unused)),
    const char *proxyhost, const char *proxyport,
    struct addrinfo proxyhints, int socksv, const char *proxyuser)
{
	int proxyfd, r, authretries = 3;
	unsigned char buf[1024];
	size_t hlen, hlen2;
	struct sockaddr_storage addr;
	const char *proxypassword = NULL;

	if (proxyport == NULL)
		proxyport = (socksv == -1) ? HTTP_PROXY_PORT : SOCKS_PORT;

	/* Dial the proxy */
	proxyfd = remote_connect(proxyhost, proxyport, proxyhints, NULL);

	if (proxyfd < 0)
		return -1;

	if (socksv == 5) {
		if (decode_addrport(host, port, (struct sockaddr *)&addr,
		    sizeof(addr), 0, 0) == -1)
			addr.ss_family = 0; /* will use SOCKS_DOMAIN */

		/* Version 5, one method: no authentication */
		buf[0] = SOCKS_V5;
		buf[1] = proxyuser ? 2 : 1;
		buf[2] = SOCKS_NOAUTH;
		buf[3] = 2; /* username/password */
		hlen = proxyuser ? 4 : 3;

		if (atomicio(vwrite, proxyfd, buf, hlen) != hlen)
			err(1, "write failed");
		if (atomicio(read, proxyfd, buf, 2) != 2)
			err(1, "read failed");
		if (buf[0] != SOCKS_V5)
			errx(1, "proxy did not accept SOCKS version 5");
		if (buf[1] == SOCKS_NOMETHOD)
			errx(1, "no SOCKS5 method accepted");

		if (buf[1] == 2) { /* username/password sub-negotiation */
			if (!proxyuser)
				errx(1, "proxy requires authentication");
			proxypassword = getproxypass(proxyuser, proxyhost);
			hlen = strlen(proxyuser);
			hlen2 = strlen(proxypassword);
			if (hlen > 255 || hlen2 > 255 || hlen == 0 || hlen2 == 0)
				errx(1, "invalid username or password");
			buf[0] = 1; /* sub-negotiation version */
			buf[1] = (unsigned char)hlen;
			memcpy(buf + 2, proxyuser, hlen);
			buf[2 + hlen] = (unsigned char)hlen2;
			memcpy(buf + 3 + hlen, proxypassword, hlen2);
			hlen = 3 + hlen + hlen2;
			if (atomicio(vwrite, proxyfd, buf, hlen) != hlen)
				err(1, "write failed");
			if (atomicio(read, proxyfd, buf, 2) != 2)
				err(1, "read failed");
			if (buf[0] != 1)
				errx(1, "unexpected auth response from proxy");
			if (buf[1] != 0)
				errx(1, "proxy authentication failed");
		}

		/* Send connect request */
		buf[0] = SOCKS_V5;
		buf[1] = SOCKS_CONNECT;
		buf[2] = 0; /* reserved */
		if (addr.ss_family == AF_INET) {
			struct sockaddr_in *s4 = (struct sockaddr_in *)&addr;
			buf[3] = SOCKS_IPV4;
			memcpy(buf + 4, &s4->sin_addr, 4);
			buf[8] = s4->sin_port >> 8;
			buf[9] = s4->sin_port & 0xff;
			hlen = 10;
		} else if (addr.ss_family == AF_INET6) {
			struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&addr;
			buf[3] = SOCKS_IPV6;
			memcpy(buf + 4, &s6->sin6_addr, 16);
			buf[20] = s6->sin6_port >> 8;
			buf[21] = s6->sin6_port & 0xff;
			hlen = 22;
		} else {
			/* SOCKS_DOMAIN */
			size_t nlen = strlen(host);
			unsigned short nport = htons((unsigned short)atoi(port));

			if (nlen > 255)
				errx(1, "hostname too long for SOCKS5");
			buf[3] = SOCKS_DOMAIN;
			buf[4] = (unsigned char)nlen;
			memcpy(buf + 5, host, nlen);
			buf[5 + nlen] = nport >> 8;
			buf[6 + nlen] = nport & 0xff;
			hlen = 7 + nlen;
		}
		if (atomicio(vwrite, proxyfd, buf, hlen) != hlen)
			err(1, "write failed");
		if (atomicio(read, proxyfd, buf, 4) != 4)
			err(1, "read failed");
		if (buf[0] != SOCKS_V5)
			errx(1, "proxy sent wrong version");
		if (buf[1] != 0)
			errx(1, "proxy error %d", buf[1]);
		/* consume bound address/port */
		switch (buf[3]) {
		case SOCKS_IPV4:
			if (atomicio(read, proxyfd, buf, 6) != 6)
				err(1, "read failed");
			break;
		case SOCKS_IPV6:
			if (atomicio(read, proxyfd, buf, 18) != 18)
				err(1, "read failed");
			break;
		case SOCKS_DOMAIN:
			if (atomicio(read, proxyfd, buf, 1) != 1)
				err(1, "read failed");
			hlen = buf[0];
			if (atomicio(read, proxyfd, buf, hlen + 2) != hlen + 2)
				err(1, "read failed");
			break;
		default:
			errx(1, "proxy returned wrong address type");
		}
	} else if (socksv == 4 || socksv == 44) {
		/* SOCKS 4 / 4A */
		if (decode_addrport(host, port, (struct sockaddr *)&addr,
		    sizeof(addr), 1, 0) == -1) {
			if (socksv == 44) {
				/* use 4A hostname */
				addr.ss_family = 0;
			} else {
				errx(1, "cannot resolve \"%s\" for SOCKS4", host);
			}
		}

		buf[0] = SOCKS_V4;
		buf[1] = SOCKS_CONNECT;
		{
			unsigned short np = htons((unsigned short)atoi(port));
			buf[2] = np >> 8;
			buf[3] = np & 0xff;
		}
		if (addr.ss_family == AF_INET) {
			struct sockaddr_in *s4 = (struct sockaddr_in *)&addr;
			memcpy(buf + 4, &s4->sin_addr, 4);
		} else {
			/* 4A: send fake 0.0.0.x to signal domain follows */
			buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 1;
		}
		if (proxyuser) {
			hlen = strlen(proxyuser);
			if (hlen > 255)
				errx(1, "proxy username too long");
			memcpy(buf + 8, proxyuser, hlen);
		} else {
			hlen = 0;
		}
		buf[8 + hlen] = '\0';
		hlen = 9 + hlen;
		if (socksv == 44 && addr.ss_family != AF_INET) {
			/* append hostname */
			size_t nlen = strlen(host);
			if (nlen > 255 || hlen + nlen + 1 > sizeof(buf))
				errx(1, "hostname too long for SOCKS4A");
			memcpy(buf + hlen, host, nlen);
			buf[hlen + nlen] = '\0';
			hlen += nlen + 1;
		}
		if (atomicio(vwrite, proxyfd, buf, hlen) != hlen)
			err(1, "write failed");
		if (atomicio(read, proxyfd, buf, 8) != 8)
			err(1, "read failed");
		if (buf[0] != 0)
			errx(1, "proxy sent wrong version");
		if (buf[1] != 90)
			errx(1, "proxy error %d", buf[1]);
	} else if (socksv == -1) {
		/* HTTP CONNECT */
		char *headers[HTTP_MAXHDRS];
		int nhdrs = 0;
		char resp[1024];
		int code;

		(void)authretries; /* Used in retry loop */

		if (proxyuser != NULL)
			proxypassword = getproxypass(proxyuser, proxyhost);

		r = snprintf((char *)buf, sizeof(buf),
		    "CONNECT %s:%s HTTP/1.0\r\n"
		    "User-Agent: nc/1.0\r\n",
		    host, port);
		if (r < 0 || (size_t)r >= sizeof(buf))
			errx(1, "HTTP proxy request too long");

		if (proxyuser != NULL) {
			char upbuf[512];
			char b64buf[700];
			int ulen = snprintf(upbuf, sizeof(upbuf), "%s:%s",
			    proxyuser, proxypassword);
			if (ulen < 0 || (size_t)ulen >= sizeof(upbuf))
				errx(1, "proxy credentials too long");
			if (b64_ntop((unsigned char *)upbuf, (size_t)ulen,
			    b64buf, sizeof(b64buf)) == -1)
				errx(1, "b64_ntop failed");
			size_t used = (size_t)r;
			int extra = snprintf((char *)buf + used,
			    sizeof(buf) - used,
			    "Proxy-Authorization: Basic %s\r\n", b64buf);
			if (extra < 0 || used + (size_t)extra >= sizeof(buf))
				errx(1, "HTTP proxy request too long");
			r += extra;
		}

		/* Append final \r\n */
		if ((size_t)r + 3 > sizeof(buf))
			errx(1, "HTTP proxy request too long");
		strlcat((char *)buf, "\r\n", sizeof(buf));
		r += 2;

		if (atomicio(vwrite, proxyfd, buf, (size_t)r) != (size_t)r)
			err(1, "write failed");

		/* Read response */
		proxy_read_line(proxyfd, resp, sizeof(resp));
		if (sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1)
			errx(1, "cannot parse proxy response: %s", resp);
		if (code != 200) {
			warnx("proxy returned status %d (%s)", code, resp);
			close(proxyfd);
			return -1;
		}
		/* Drain headers */
		for (;;) {
			proxy_read_line(proxyfd, resp, sizeof(resp));
			if (nhdrs < HTTP_MAXHDRS) {
				headers[nhdrs++] = NULL; /* ignore them */
			}
			if (resp[0] == '\0')
				break;
		}
		(void)headers; (void)nhdrs;
	} else {
		errx(1, "unsupported SOCKS version %d", socksv);
	}

	return proxyfd;
}

/*
 * Nathanael Eka Oktavian <nathanael@nand.eu.org>
 *
 * BSD-3-Clause
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sockslib.h"
#include "util.h"

static int socks_get_auth_method(int fd)
{
	int ret;
	unsigned char req_buf[4], resp_buf[2];

	req_buf[0] = SOCKS_VERSION;
	req_buf[1] = 2; /* Total of requested methods */
	req_buf[2] = SOCKS_NO_AUTH;
	req_buf[3] = SOCKS_AUTH_USERPASS;

	ret = sockslib_send(fd, req_buf, 4);
	if (ret < 0)
		return ret;

	ret = sockslib_read(fd, resp_buf, 2);
	if (ret < 0)
		return ret;

	ret = resp_buf[1];
	return ret;
}

static int socks_setaddr(int type, void *dest, const char *ip)
{
	int ret;

	if (inet_pton(type, ip, dest) <= 0)
		ret = -SOCKS_ERR_ADDR_NOTSUPP;
	else
		ret = SOCKS_ERR_OK;

	return ret;
}

static void server_clear(struct socks_ctx *ctx)
{
	struct socks_server *s;

	s = &ctx->server;
	s->s_addr = NULL;
	s->s_port = 0;
	s->fd = -1;
}

static void auth_clear(struct socks_ctx *ctx)
{
	struct socks_auth *a;

	a = &ctx->auth;
	a->method = 0;
	a->authed = 0;
	memset(a->username, 0, sizeof(a->username));
	memset(a->password, 0, sizeof(a->password));
	a->user_len = 0;
	a->pass_len = 0;
}

struct socks_ctx *socks_init(void)
{
	struct socks_ctx *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->atyp = 0;
	ctx->d_addr.name[0] = 0;
	ctx->d_port = 0;
	ctx->d_name_len = 0;
	ctx->reply = -1;
	ctx->ver = SOCKS_VERSION;

	auth_clear(ctx);
	server_clear(ctx);

	return ctx;
}

const char *socks_strerror(int code)
{
	/* see enum socks_err in sockslib.h */
	static const char *str[] = {
		"", "SOCKS server failure", "Connection not allowed",
		"Network unreachable", "Host unreachable",
		"Connection refused", "TTL expired", "Command not supported",
		"Address type not supported", "Authentication method not supported",
		"Invalid authentication", "Value too long",
		"Out of memory", "Invalid argument",
		"Empty Request/Response", "System error (check errno)"
	};

	code = (code < 0) ? -code : code;
	if (code >= SOCKS_ERR_OK && code <= SOCKS_ERR_SYS_ERRNO)
		return str[code];

	return "Unknown error";
}

int socks_set_auth(struct socks_ctx *ctx, const char *u, const char *p)
{
	if (!ctx)
		return -SOCKS_ERR_BAD_ARG;

	if (!u || !*u || !p || !*p)
		return -SOCKS_ERR_BAD_ARG;

	size_t ul, pl;

	ul = strlen(u);
	pl = strlen(p);

	if (ul > 255 || pl > 255)
		return -SOCKS_ERR_BAD_AUTH;

	memcpy(ctx->auth.username, u, ul);
	memcpy(ctx->auth.password, p, pl);
	ctx->auth.user_len = ul;
	ctx->auth.pass_len = pl;

	return SOCKS_ERR_OK;
}

int socks_set_server(struct socks_ctx *ctx, const char *host, const char *port)
{
	if (!ctx || !host || !*host)
		return -SOCKS_ERR_BAD_ARG;

	if (!port || !*port)
		port = "1080";

	int fd, ret;
	struct addrinfo hint, *res, *addr;
	memset(&hint, 0, sizeof(hint));

	hint.ai_family = AF_UNSPEC;
	hint.ai_socktype = SOCK_STREAM;

	ret = getaddrinfo(host, port, &hint, &res);
	switch (ret) {
	case 0:
		break;
	case EAI_BADFLAGS:  /* useful for debugging */
		ret = -SOCKS_ERR_BAD_ARG;
		goto fail;
	case EAI_AGAIN:
		ret = -SOCKS_ERR_CONN_REFUSED;
		goto fail;
	case EAI_FAIL:
		ret = -SOCKS_ERR_SERV_FAIL;
		goto fail;
	case EAI_MEMORY:
		ret = -SOCKS_ERR_NO_MEM;
		goto fail;
	case EAI_SYSTEM:
		ret = -SOCKS_ERR_SYS_ERRNO;
		goto fail;
	default:
		ret = -SOCKS_ERR_ADDR_NOTSUPP;
		goto fail;
	}

	for (addr = res; addr; addr = addr->ai_next) {
		fd = socket(addr->ai_family, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		break;
	}

	if (!addr) {
		freeaddrinfo(res);
		ret = -SOCKS_ERR_SERV_FAIL;
		goto fail;
	}

	ctx->server.fd = fd;
	ctx->server.s_addr = addr;
	ctx->server.s_port = htons(atoi(port));
fail:
	return ret;
}

int socks_connect_server(struct socks_ctx *ctx)
{
	if (!ctx)
		return -SOCKS_ERR_BAD_ARG;

	int ret;
	ssize_t len = 0;
	unsigned char *auth_buf = NULL, res_buf[2];

	ret = connect(ctx->server.fd,
		      ctx->server.s_addr->ai_addr,
		      ctx->server.s_addr->ai_addrlen);
	if (ret < 0)
		return ret;

	ret = socks_get_auth_method(ctx->server.fd);
	if (ret < 0)
		return ret;

	ctx->auth.method = ret;
	switch (ctx->auth.method) {
	case SOCKS_NO_AUTH:
		ctx->auth.authed = 1;
		ret = SOCKS_ERR_OK;
		break;
	case SOCKS_AUTH_USERPASS:
		auth_buf = malloc(3 + ctx->auth.user_len + ctx->auth.pass_len);
		if (!auth_buf)
			return -SOCKS_ERR_NO_MEM;

		auth_buf[len++] = SOCKS_AUTH_VERSION;

		auth_buf[len++] = ctx->auth.user_len;
		memcpy(auth_buf + len, ctx->auth.username, ctx->auth.user_len);
		len += ctx->auth.user_len;

		auth_buf[len++] = ctx->auth.pass_len;
		memcpy(auth_buf + len, ctx->auth.password, ctx->auth.pass_len);
		len += ctx->auth.pass_len;

		ret = sockslib_send(ctx->server.fd, auth_buf, len);
		if (ret < 0)
			goto malloc_cleanup;

		ret = sockslib_read(ctx->server.fd, res_buf, 2);
		if (ret < 0)
			goto malloc_cleanup;

		if (res_buf[1] != 0) {
			ret = -SOCKS_ERR_BAD_AUTH;
			goto malloc_cleanup;
		}

		ctx->auth.authed = 1;
		ret = SOCKS_ERR_OK;
malloc_cleanup:
		free(auth_buf);
		auth_buf = NULL;
		break;
	default:
		ret = -SOCKS_ERR_AUTH_NOTSUPP;
		break;
	}

	return ret;
}

int socks_set_addr4(struct socks_ctx *ctx, const char *ip, const char *port)
{
	if (!ctx)
		return -SOCKS_ERR_BAD_ARG;

	if (!ip || !*ip || !port || !*port)
		return -SOCKS_ERR_BAD_ARG;

	ctx->atyp = SOCKS_ATYP_IPV4;
	ctx->d_port = htons(atoi(port));

	return socks_setaddr(AF_INET, ctx->d_addr.ip4, ip);
}

int socks_set_addr6(struct socks_ctx *ctx, const char *ip, const char *port)
{
	if (!ctx)
		return -SOCKS_ERR_BAD_ARG;

	if (!ip || !*ip || !port || !*port)
		return -SOCKS_ERR_BAD_ARG;

	ctx->atyp = SOCKS_ATYP_IPV6;
	ctx->d_port = htons(atoi(port));

	return socks_setaddr(AF_INET6, ctx->d_addr.ip6, ip);
}

int socks_set_addrname(struct socks_ctx *ctx, const char *name, const char *port)
{
	if (!ctx)
		return -SOCKS_ERR_BAD_ARG;

	if (!name || !*name || !port || !*port)
		return -SOCKS_ERR_BAD_ARG;

	ctx->d_name_len = strlen(name);
	if (ctx->d_name_len > 255)
		return -SOCKS_ERR_TOO_LONG;

	ctx->atyp = SOCKS_ATYP_NAME;
	ctx->d_port = htons(atoi(port));
	memcpy(ctx->d_addr.name, name, ctx->d_name_len);

	return SOCKS_ERR_OK;
}

int socks_connect(struct socks_ctx *ctx)
{
	if (!ctx)
		return -SOCKS_ERR_BAD_ARG;

	int ret;
	unsigned char req_buf[512], resp_buf[512];  /* big enough? */
	size_t len = 0;

	req_buf[len++] = SOCKS_VERSION;
	req_buf[len++] = SOCKS_CMD_CONNECT;
	req_buf[len++] = 0x00; /* reserved */
	req_buf[len++] = ctx->atyp;

	switch (ctx->atyp) {
	case SOCKS_ATYP_IPV4: /* ipv4 */
		memcpy(req_buf + len, ctx->d_addr.ip4, 4);
		len += 4;
		break;
	case SOCKS_ATYP_NAME: /* domain name */
		req_buf[len++] = ctx->d_name_len;
		memcpy(req_buf + len, ctx->d_addr.name, ctx->d_name_len);
		len += ctx->d_name_len;
		break;
	case SOCKS_ATYP_IPV6: /* ipv6 */
		memcpy(req_buf + len, ctx->d_addr.ip6, 16);
		len += 16;
		break;
	default:
		return -SOCKS_ERR_ADDR_NOTSUPP;
	}

	memcpy(req_buf + len, &ctx->d_port, 2);
	len += 2;

	ret = sockslib_send(ctx->server.fd, req_buf, len);
	if (ret < 0)
		return ret;

	ret = sockslib_read(ctx->server.fd, resp_buf, len);
	if (ret < 0)
		return ret;

	ctx->reply = resp_buf[1];
	return ctx->reply ? -ctx->reply : ctx->server.fd;
}

void socks_end(struct socks_ctx *ctx)
{
	if (!ctx)
		return;

	if (ctx->server.fd != -1)
		close(ctx->server.fd);
	if (ctx->server.s_addr)
		freeaddrinfo(ctx->server.s_addr);

	free(ctx);
}

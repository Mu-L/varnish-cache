/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Write data to fd
 * We try to use writev() if possible in order to minimize number of
 * syscalls made and packets sent.  It also just might allow the worker
 * thread to complete the request without holding stuff locked.
 *
 * XXX: chunked header (generated in Flush) and Tail (EndChunk)
 *      are not accounted by means of the size_t returned. Obvious ideas:
 *	- add size_t return value to Flush and EndChunk
 *	- base accounting on (struct v1l).cnt
 */

#include "config.h"

#include <sys/uio.h>
#include "cache/cache_varnishd.h"
#include "cache/cache_filter.h"

#include <stdio.h>

#include "cache_http1.h"
#include "vtim.h"

/*--------------------------------------------------------------------*/

struct v1l {
	unsigned		magic;
#define V1L_MAGIC		0x2f2142e5
	int			*wfd;
	stream_close_t		werr;	/* valid after V1L_Flush() */
	struct iovec		*iov;
	int			siov;
	int			niov;
	size_t			liov;
	size_t			cliov;
	int			ciov;	/* Chunked header marker */
	vtim_real		deadline;
	struct vsl_log		*vsl;
	uint64_t		cnt;	/* Flushed byte count */
	struct ws		*ws;
	uintptr_t		ws_snap;
	void			**vdp_priv;
};

/*--------------------------------------------------------------------
 * for niov == 0, reserve the ws for max number of iovs
 * otherwise, up to niov
 */

struct v1l *
V1L_Open(struct ws *ws, int *fd, struct vsl_log *vsl,
    vtim_real deadline, unsigned niov)
{
	struct v1l *v1l;
	unsigned u;
	uintptr_t ws_snap;
	size_t sz;

	if (WS_Overflowed(ws))
		return (NULL);

	if (niov != 0)
		assert(niov >= 3);

	ws_snap = WS_Snapshot(ws);

	v1l = WS_Alloc(ws, sizeof *v1l);
	if (v1l == NULL)
		return (NULL);
	INIT_OBJ(v1l, V1L_MAGIC);

	v1l->ws = ws;
	v1l->ws_snap = ws_snap;

	u = WS_ReserveLumps(ws, sizeof(struct iovec));
	if (u < 3) {
		/* Must have at least 3 in case of chunked encoding */
		WS_Release(ws, 0);
		WS_MarkOverflow(ws);
		return (NULL);
	}
	if (u > IOV_MAX)
		u = IOV_MAX;
	if (niov != 0 && u > niov)
		u = niov;
	v1l->iov = WS_Reservation(ws);
	v1l->siov = (int)u;
	v1l->ciov = (int)u;
	v1l->wfd = fd;
	v1l->deadline = deadline;
	v1l->vsl = vsl;
	v1l->werr = SC_NULL;

	sz = u * sizeof(struct iovec);
	assert(sz < UINT_MAX);
	WS_Release(ws, (unsigned)sz);
	return (v1l);
}

void
V1L_NoRollback(struct v1l *v1l)
{

	CHECK_OBJ_NOTNULL(v1l, V1L_MAGIC);
	v1l->ws_snap = 0;
}

stream_close_t
V1L_Close(struct v1l **v1lp, uint64_t *cnt)
{
	struct v1l *v1l;
	struct ws *ws;
	uintptr_t ws_snap;
	stream_close_t sc;

	AN(cnt);
	TAKE_OBJ_NOTNULL(v1l, v1lp, V1L_MAGIC);
	if (v1l->vdp_priv != NULL) {
		assert(*v1l->vdp_priv == v1l);
		*v1l->vdp_priv = NULL;
	}
	sc = V1L_Flush(v1l);
	*cnt = v1l->cnt;
	ws = v1l->ws;
	ws_snap = v1l->ws_snap;
	ZERO_OBJ(v1l, sizeof *v1l);
	if (ws_snap != 0)
		WS_Rollback(ws, ws_snap);
	return (sc);
}

static void
v1l_prune(struct v1l *v1l, ssize_t abytes)
{
	size_t used = 0;
	size_t sz, bytes, used_here;
	int j;

	assert(abytes > 0);
	bytes = (size_t)abytes;

	for (j = 0; j < v1l->niov; j++) {
		if (used + v1l->iov[j].iov_len > bytes) {
			/* Cutoff is in this iov */
			used_here = bytes - used;
			v1l->iov[j].iov_len -= used_here;
			v1l->iov[j].iov_base =
			    (char*)v1l->iov[j].iov_base + used_here;
			sz = (unsigned)v1l->niov - (unsigned)j;
			sz *= sizeof(struct iovec);
			memmove(v1l->iov, &v1l->iov[j], sz);
			v1l->niov -= j;
			assert(v1l->liov >= bytes);
			v1l->liov -= bytes;
			return;
		}
		used += v1l->iov[j].iov_len;
	}
	AZ(v1l->liov);
}

stream_close_t
V1L_Flush(struct v1l *v1l)
{
	ssize_t i;
	size_t sz;
	int err;
	char cbuf[32];

	CHECK_OBJ_NOTNULL(v1l, V1L_MAGIC);
	CHECK_OBJ_NOTNULL(v1l->werr, STREAM_CLOSE_MAGIC);
	AN(v1l->wfd);

	assert(v1l->niov <= v1l->siov);

	if (*v1l->wfd >= 0 && v1l->liov > 0 && v1l->werr == SC_NULL) {
		if (v1l->ciov < v1l->siov && v1l->cliov > 0) {
			/* Add chunk head & tail */
			bprintf(cbuf, "00%zx\r\n", v1l->cliov);
			sz = strlen(cbuf);
			v1l->iov[v1l->ciov].iov_base = cbuf;
			v1l->iov[v1l->ciov].iov_len = sz;
			v1l->liov += sz;

			/* This is OK, because siov was --'ed */
			v1l->iov[v1l->niov].iov_base = cbuf + sz - 2;
			v1l->iov[v1l->niov++].iov_len = 2;
			v1l->liov += 2;
		} else if (v1l->ciov < v1l->siov) {
			v1l->iov[v1l->ciov].iov_base = cbuf;
			v1l->iov[v1l->ciov].iov_len = 0;
		}

		i = 0;
		err = 0;
		do {
			if (VTIM_real() > v1l->deadline) {
				VSLb(v1l->vsl, SLT_Debug,
				    "Hit total send timeout, "
				    "wrote = %zd/%zd; not retrying",
				    i, v1l->liov);
				i = -1;
				break;
			}

			i = writev(*v1l->wfd, v1l->iov, v1l->niov);
			if (i > 0) {
				v1l->cnt += (size_t)i;
				if ((size_t)i == v1l->liov)
					break;
			}

			/* we hit a timeout, and some data may have been sent:
			 * Remove sent data from start of I/O vector, then retry
			 *
			 * XXX: Add a "minimum sent data per timeout counter to
			 * prevent slowloris attacks
			 */

			err = errno;

			if (err == EWOULDBLOCK) {
				VSLb(v1l->vsl, SLT_Debug,
				    "Hit idle send timeout, "
				    "wrote = %zd/%zd; retrying",
				    i, v1l->liov);
			}

			if (i > 0)
				v1l_prune(v1l, i);
		} while (i > 0 || err == EWOULDBLOCK);

		if (i <= 0) {
			VSLb(v1l->vsl, SLT_Debug,
			    "Write error, retval = %zd, len = %zd, errno = %s",
			    i, v1l->liov, VAS_errtxt(err));
			assert(v1l->werr == SC_NULL);
			if (err == EPIPE)
				v1l->werr = SC_REM_CLOSE;
			else
				v1l->werr = SC_TX_ERROR;
			errno = err;
		}
	}
	v1l->liov = 0;
	v1l->cliov = 0;
	v1l->niov = 0;
	if (v1l->ciov < v1l->siov)
		v1l->ciov = v1l->niov++;
	CHECK_OBJ_NOTNULL(v1l->werr, STREAM_CLOSE_MAGIC);
	return (v1l->werr);
}

size_t
V1L_Write(struct v1l *v1l, const void *ptr, ssize_t alen)
{
	size_t len = 0;

	CHECK_OBJ_NOTNULL(v1l, V1L_MAGIC);
	AN(v1l->wfd);
	if (alen == 0 || *v1l->wfd < 0)
		return (0);
	if (alen > 0)
		len = (size_t)alen;
	else if (alen == -1)
		len = strlen(ptr);
	else
		WRONG("alen");

	assert(v1l->niov < v1l->siov);
	v1l->iov[v1l->niov].iov_base = TRUST_ME(ptr);
	v1l->iov[v1l->niov].iov_len = len;
	v1l->liov += len;
	v1l->niov++;
	v1l->cliov += len;
	if (v1l->niov >= v1l->siov) {
		(void)V1L_Flush(v1l);
		VSC_C_main->http1_iovs_flush++;
	}
	return (len);
}

void
V1L_Chunked(struct v1l *v1l)
{

	CHECK_OBJ_NOTNULL(v1l, V1L_MAGIC);

	assert(v1l->ciov == v1l->siov);
	assert(v1l->siov >= 3);
	/*
	 * If there is no space for chunked header, a chunk of data and
	 * a chunk tail, we might as well flush right away.
	 */
	if (v1l->niov + 3 >= v1l->siov) {
		(void)V1L_Flush(v1l);
		VSC_C_main->http1_iovs_flush++;
	}
	v1l->siov--;
	v1l->ciov = v1l->niov++;
	v1l->cliov = 0;
	assert(v1l->ciov < v1l->siov);
	assert(v1l->niov < v1l->siov);
}

/*
 * XXX: It is not worth the complexity to attempt to get the
 * XXX: end of chunk into the V1L_Flush(), because most of the time
 * XXX: if not always, that is a no-op anyway, because the calling
 * XXX: code already called V1L_Flush() to release local storage.
 */

void
V1L_EndChunk(struct v1l *v1l)
{

	CHECK_OBJ_NOTNULL(v1l, V1L_MAGIC);

	assert(v1l->ciov < v1l->siov);
	(void)V1L_Flush(v1l);
	v1l->siov++;
	v1l->ciov = v1l->siov;
	v1l->niov = 0;
	v1l->cliov = 0;
	(void)V1L_Write(v1l, "0\r\n\r\n", -1);
}

/*--------------------------------------------------------------------
 * VDP using V1L
 */

/* remember priv pointer for V1L_Close() to clear */
static int v_matchproto_(vdp_init_f)
v1l_init(VRT_CTX, struct vdp_ctx *vdc, void **priv)
{
	struct v1l *v1l;

	(void) ctx;
	(void) vdc;
	AN(priv);
	CAST_OBJ_NOTNULL(v1l, *priv, V1L_MAGIC);

	v1l->vdp_priv = priv;
	return (0);
}

static int v_matchproto_(vdp_bytes_f)
v1l_bytes(struct vdp_ctx *vdc, enum vdp_action act, void **priv,
    const void *ptr, ssize_t len)
{
	size_t wl = 0;

	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);
	AN(priv);

	AZ(vdc->nxt);		/* always at the bottom of the pile */

	if (len > 0)
		wl = V1L_Write(*priv, ptr, len);
	if (act > VDP_NULL && V1L_Flush(*priv) != SC_NULL)
		return (-1);
	if ((size_t)len != wl)
		return (-1);
	return (0);
}

/*--------------------------------------------------------------------
 * VDPIO using V1L
 *
 * this is deliverately half-baked to reduce work in progress while heading
 * towards VAI/VDPIO: we update the v1l with the scarab, which we
 * return unmodified.
 *
 */

/* remember priv pointer for V1L_Close() to clear */
static int v_matchproto_(vpio_init_f)
v1l_io_init(VRT_CTX, struct vdp_ctx *vdc, void **priv, int capacity)
{
	struct v1l *v1l;

	(void) ctx;
	(void) vdc;
	AN(priv);

	CAST_OBJ_NOTNULL(v1l, *priv, V1L_MAGIC);

	v1l->vdp_priv = priv;
	return (capacity);
}

static int v_matchproto_(vpio_init_f)
v1l_io_upgrade(VRT_CTX, struct vdp_ctx *vdc, void **priv, int capacity)
{
	return (v1l_io_init(ctx, vdc, priv, capacity));
}

/*
 * API note
 *
 * this VDP is special in that it does not transform data, but prepares
 * the write. From the perspective of VDPIO, its current state is only
 * transitional.
 *
 * Because the VDP prepares the actual writes, but the caller needs
 * to return the scarab's leases, the caller in this case is
 * required to empty the scarab after V1L_Flush()'ing.
 */

static int v_matchproto_(vdpio_lease_f)
v1l_io_lease(struct vdp_ctx *vdc, struct vdp_entry *this, struct vscarab *scarab)
{
	struct v1l *v1l;
	struct viov *v;
	int r;

	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(this, VDP_ENTRY_MAGIC);
	CAST_OBJ_NOTNULL(v1l, this->priv, V1L_MAGIC);
	VSCARAB_CHECK(scarab);
	AZ(scarab->used);	// see note above
	this->calls++;
	r = vdpio_pull(vdc, this, scarab);
	if (r < 0)
		return (r);
	VSCARAB_FOREACH(v, scarab)
		this->bytes_in += V1L_Write(v1l, v->iov.iov_base, v->iov.iov_len);
	return (r);
}

const struct vdp * const VDP_v1l = &(struct vdp){
	.name =		"V1B",
	.init =		v1l_init,
	.bytes =	v1l_bytes,

	.io_init =	v1l_io_init,
	.io_upgrade =	v1l_io_upgrade,
	.io_lease =	v1l_io_lease,
};

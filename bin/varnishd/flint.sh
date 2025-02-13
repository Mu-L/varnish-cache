#!/bin/sh
#
# Copyright (c) 2006-2021 Varnish Software AS
# SPDX-License-Identifier: BSD-2-Clause
# See LICENSE file for full text of license

FLOPS='
	-I../../lib/libvgz
	-I../../lib/libvsc
	-DNOT_IN_A_VMOD
	-DVARNISH_STATE_DIR="foo"
	-DVARNISH_VMOD_DIR="foo"
	-DVARNISH_VCL_DIR="foo"
	-DWITH_PERSISTENT_STORAGE
	acceptor/*.c
	cache/*.c
	common/*.c
	hash/*.c
	http1/*.c
	http2/*.c
	mgt/*.c
	proxy/*.c
	storage/*.c
	waiter/*.c
	../../lib/libvarnish/flint.lnt
	../../lib/libvarnish/*.c
	../../lib/libvcc/flint.lnt
	../../lib/libvcc/*.c
	../../vmod/flint.lnt
	../../vmod/vcc_debug_if.c
	../../vmod/vmod_debug*.c
	../../vmod/VSC_debug*.c
' ../../tools/flint_skel.sh $*

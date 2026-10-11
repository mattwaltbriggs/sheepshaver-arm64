/*
 *  jit-wx.cpp - W^X code-memory primitives for JIT backends
 *
 *  PocketShaver arm64 JIT backend (C) 2026 Sierra Burkhart
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "vm_alloc.h"
#include "jit-wx.hpp"

#if defined(__APPLE__) && defined(__aarch64__)

#include <TargetConditionals.h>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>

struct jit_wx_write_ctx {
	void *dst;
	const void *src;
	size_t len;
};

static int jit_wx_write_cb(void *arg)
{
	jit_wx_write_ctx *ctx = (jit_wx_write_ctx *)arg;
	memcpy(ctx->dst, ctx->src, ctx->len);
	return 0;
}

// May be invoked only once per executable
#ifdef PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP
PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(jit_wx_write_cb);
#endif

static bool jit_wx_api_available(void)
{
	if (__builtin_available(macOS 11.4, iOS 17.4, *))
		return true;
	return false;
}

void *jit_wx_map(uint32 size)
{
	// MAP_JIT + simultaneous RWX is permitted by the allow-jit entitlement.
	// The JIT write-protect API (pthread_jit_write_with_callback_np) is used
	// for publication; direct RWX also lets the emitters write through a
	// plain-RW shadow if the API is unavailable.
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
				   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "jit_wx_map: mmap(%u) FAILED\n", size);
		return NULL;
	}
	return p;
}

void jit_wx_unmap(void *base, uint32 size)
{
	if (base)
		munmap(base, size);
}

bool jit_wx_publish(void *dst, const void *src, uint32 len)
{
	jit_wx_write_ctx ctx = { dst, src, len };
	if (jit_wx_api_available()) {
		if (pthread_jit_write_with_callback_np(jit_wx_write_cb, &ctx) != 0)
			return false;
	} else {
		// No write-protect API: the mapping is RWX, so a plain copy works
		memcpy(dst, src, len);
	}
	sys_icache_invalidate(dst, len);
	return true;
}

bool jit_wx_available(void)
{
	static int cached = -1;
	if (cached < 0) {
		void *p = jit_wx_map(16 * 1024);
		cached = (p != NULL);
		jit_wx_unmap(p, 16 * 1024);
	}
	return cached != 0;
}

// Single registered shadow region (one translation cache per process)
static uint8 *g_wx_exec_base = NULL;
static uint8 *g_wx_write_base = NULL;
static uint32 g_wx_span = 0;

void jit_wx_register_shadow(void *exec_base, void *write_base, uint32 size)
{
	g_wx_exec_base = (uint8 *)exec_base;
	g_wx_write_base = (uint8 *)write_base;
	g_wx_span = size;
}

uint8 *jit_wx_writable(void *exec_addr)
{
	uint8 *p = (uint8 *)exec_addr;
	if (g_wx_write_base && p >= g_wx_exec_base && p < g_wx_exec_base + g_wx_span)
		return g_wx_write_base + (p - g_wx_exec_base);
	return p;
}

void jit_wx_publish_range(uintptr start, uintptr stop)
{
	if (stop <= start)
		return;
	uint8 *w = jit_wx_writable((void *)start);
	if (w != (uint8 *)start)
		jit_wx_publish((void *)start, w, (uint32)(stop - start));
}

bool jit_wx_selftest(void)
{
	if (!jit_wx_available())
		return false;
	const uint32 size = 16 * 1024;
	uint8 *code = (uint8 *)jit_wx_map(size);
	if (code == NULL)
		return false;
	// mov w0, #42 ; ret -- then rewritten to return 99, proving that
	// already-executed code can be republished (chain backpatching)
	static const uint32 fn42[2] = { 0x52800540u, 0xd65f03c0u };
	static const uint32 fn99[2] = { 0x52800c60u, 0xd65f03c0u };
	typedef int (*fn_t)(void);
	bool ok = jit_wx_publish(code, fn42, sizeof(fn42))
		&& ((fn_t)code)() == 42
		&& jit_wx_publish(code, fn99, sizeof(fn99))
		&& ((fn_t)code)() == 99;
	jit_wx_unmap(code, size);
	return ok;
}

#else

#if defined(__APPLE__)
#include <sys/mman.h>

void *jit_wx_map(uint32 size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
				   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	return (p == MAP_FAILED) ? NULL : p;
}

void jit_wx_unmap(void *base, uint32 size)
{
	if (base)
		munmap(base, size);
}
#else

void *jit_wx_map(uint32 size)
{
	void *p = vm_acquire(size, VM_MAP_PRIVATE);
	if (p == VM_MAP_FAILED)
		return NULL;
	if (vm_protect(p, size, VM_PAGE_READ | VM_PAGE_WRITE | VM_PAGE_EXECUTE) < 0) {
		vm_release(p, size);
		return NULL;
	}
	return p;
}

void jit_wx_unmap(void *base, uint32 size)
{
	if (base)
		vm_release(base, size);
}
#endif

bool jit_wx_publish(void *dst, const void *src, uint32 len)
{
	memcpy(dst, src, len);
	return true;
}

bool jit_wx_available(void)
{
	static int cached = -1;
	if (cached < 0) {
		void *p = jit_wx_map(16 * 1024);
		cached = (p != NULL);
		jit_wx_unmap(p, 16 * 1024);
	}
	return cached != 0;
}

bool jit_wx_selftest(void)
{
	if (!jit_wx_available())
		return false;
	const uint32 size = 16 * 1024;
	uint8 *code = (uint8 *)jit_wx_map(size);
	if (code == NULL)
		return false;
	static const uint32 probe[2] = { 0x12345678u, 0x9abcdef0u };
	bool ok = jit_wx_publish(code, probe, sizeof(probe))
		&& memcmp(code, probe, sizeof(probe)) == 0;
	jit_wx_unmap(code, size);
	return ok;
}

#endif

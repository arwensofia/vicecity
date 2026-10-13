/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Recompiler support code linked into vc_game: symbol lookup, trace ring
 * buffer, default indirect dispatch. Not derived from the game; the tables it
 * reads (ps2_recomp_functions, ps2_recomp_symbols) are generated.
 * See recomp/include/ps2_recomp.h.
 */
#include "ps2_recomp.h"

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#define PS2_WEAK __attribute__((weak))

/* ------------------------------------------------------------------------ */
/* Symbols                                                                   */
/* ------------------------------------------------------------------------ */

const ps2_recomp_symbol *ps2_recomp_find_symbol(uint32_t addr)
{
	uint32_t lo = 0, hi = ps2_recomp_symbol_count;
	while (lo < hi) { /* first entry with addr > target */
		uint32_t mid = lo + (hi - lo) / 2;
		if (ps2_recomp_symbols[mid].addr <= addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0) return NULL;
	const ps2_recomp_symbol *s = &ps2_recomp_symbols[lo - 1];
	uint32_t size = s->size ? s->size : 4;
	return addr - s->addr < size ? s : NULL;
}

static size_t put_str(char *buf, size_t pos, size_t size, const char *s)
{
	while (*s && pos + 1 < size) buf[pos++] = *s++;
	return pos;
}

static size_t put_hex(char *buf, size_t pos, size_t size, uint32_t v, int digits)
{
	static const char hx[] = "0123456789abcdef";
	char tmp[8];
	int n = 0;
	do {
		tmp[n++] = hx[v & 15];
		v >>= 4;
	} while (v && n < 8);
	while (n < digits) tmp[n++] = '0';
	while (n > 0 && pos + 1 < size) buf[pos++] = tmp[--n];
	return pos;
}

char *ps2_recomp_format_addr(uint32_t addr, char *buf, size_t size)
{
	if (!buf || !size) return buf;
	size_t p = 0;
	const ps2_recomp_symbol *s = ps2_recomp_find_symbol(addr);
	if (!s) {
		p = put_str(buf, p, size, "0x");
		p = put_hex(buf, p, size, addr, 8);
		p = put_str(buf, p, size, " (?)");
	} else {
		if (s->name) {
			p = put_str(buf, p, size, s->name);
		} else {
			p = put_str(buf, p, size, "ps2f_");
			p = put_hex(buf, p, size, s->addr, 8);
		}
		if (addr != s->addr) {
			p = put_str(buf, p, size, "+0x");
			p = put_hex(buf, p, size, addr - s->addr, 1);
		}
		if (s->module) {
			p = put_str(buf, p, size, " [");
			p = put_str(buf, p, size, s->module);
			p = put_str(buf, p, size, "]");
		}
	}
	buf[p] = 0;
	return buf;
}

uint32_t ps2_recomp_host_to_ee(const void *host_pc)
{
	Dl_info info;
	if (!host_pc || !dladdr(host_pc, &info) || !info.dli_sname) return 0;
	const char *n = info.dli_sname;
	if (n[0] == '_' && strncmp(n + 1, "ps2f_", 5) == 0) n++; /* Mach-O underscore, if present */
	if (strncmp(n, "ps2f_", 5) != 0) return 0;
	uint32_t v = 0;
	for (int k = 5; k < 13; k++) {
		char ch = n[k];
		uint32_t d;
		if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
		else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
		else return 0;
		v = v << 4 | d;
	}
	return v;
}

/* ------------------------------------------------------------------------ */
/* Trace ring                                                                */
/* ------------------------------------------------------------------------ */

ps2_trace_ring ps2_trace_buffer;

void ps2_trace_record(ps2_cpu *c, uint32_t addr, unsigned kind)
{
	uint32_t i = __atomic_fetch_add(&ps2_trace_buffer.head, 1u, __ATOMIC_RELAXED) & (PS2_TRACE_RING_SIZE - 1);
	ps2_trace_buffer.e[i].addr_kind = (addr & ~3u) | (kind & 3u);
	ps2_trace_buffer.e[i].sp = c ? c->r[29].uw[0] : 0;
}

PS2_WEAK void ps2_trace_fn(ps2_cpu *c, uint32_t entry) { ps2_trace_record(c, entry, PS2_TRACE_KIND_FN); }
PS2_WEAK void ps2_trace_bb(ps2_cpu *c, uint32_t addr) { ps2_trace_record(c, addr, PS2_TRACE_KIND_BB); }

static void write_all(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(fd, s, n);
		if (w <= 0) return;
		s += w;
		n -= (size_t)w;
	}
}

void ps2_trace_dump(int fd, unsigned max)
{
	static const char *const kinds[4] = {"fn  ", "bb  ", "MISS", "user"};
	uint32_t head = __atomic_load_n(&ps2_trace_buffer.head, __ATOMIC_RELAXED);
	uint32_t n = head < PS2_TRACE_RING_SIZE ? head : PS2_TRACE_RING_SIZE;
	if (max < n) n = max;
	char line[256];
	size_t p = put_str(line, 0, sizeof line, "--- ps2 trace: last ");
	p = put_hex(line, p, sizeof line, n, 1);
	p = put_str(line, p, sizeof line, " (hex) of ");
	p = put_hex(line, p, sizeof line, head, 1);
	p = put_str(line, p, sizeof line, " records, oldest first ---\n");
	write_all(fd, line, p);
	for (uint32_t k = head - n; k != head; k++) {
		const ps2_trace_entry *e = &ps2_trace_buffer.e[k & (PS2_TRACE_RING_SIZE - 1)];
		uint32_t addr = e->addr_kind & ~3u;
		p = put_str(line, 0, sizeof line, kinds[e->addr_kind & 3u]);
		p = put_str(line, p, sizeof line, " ");
		p = put_hex(line, p, sizeof line, addr, 8);
		p = put_str(line, p, sizeof line, " sp=");
		p = put_hex(line, p, sizeof line, e->sp, 8);
		p = put_str(line, p, sizeof line, "  ");
		char sym[160];
		ps2_recomp_format_addr(addr, sym, sizeof sym);
		p = put_str(line, p, sizeof line, sym);
		p = put_str(line, p, sizeof line, "\n");
		write_all(fd, line, p);
	}
}

/* ------------------------------------------------------------------------ */
/* Loop back-edge hook (PS2_BACKEDGE_HOOK)                                   */
/* ------------------------------------------------------------------------ */

int32_t ps2_backedge_budget = PS2_BACKEDGE_DEFAULT_BUDGET;

PS2_WEAK void ps2_backedge(ps2_cpu *c, uint32_t target)
{
	(void)c;
	(void)target;
	ps2_backedge_budget = PS2_BACKEDGE_DEFAULT_BUDGET;
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------ */

uint32_t ps2_dispatch_unknown_count;

PS2_WEAK void ps2_dispatch_unknown(ps2_cpu *c, uint32_t site, uint32_t target, int kind)
{
	static uint32_t logged_sites[32];
	static uint32_t nlogged;
	uint32_t count = __atomic_add_fetch(&ps2_dispatch_unknown_count, 1u, __ATOMIC_RELAXED);
	ps2_trace_record(c, target, PS2_TRACE_KIND_MISS);
	for (uint32_t k = 0; k < nlogged; k++)
		if (logged_sites[k] == site) return;
	if (nlogged >= 32) return;
	logged_sites[nlogged++] = site;
	char line[512], a[160], b[160];
	size_t p = put_str(line, 0, sizeof line, "ps2: unknown indirect ");
	p = put_str(line, p, sizeof line, kind == PS2_DISPATCH_CALL ? "call" : "jump");
	p = put_str(line, p, sizeof line, " to ");
	p = put_str(line, p, sizeof line, ps2_recomp_format_addr(target, a, sizeof a));
	p = put_str(line, p, sizeof line, " from ");
	p = put_str(line, p, sizeof line, ps2_recomp_format_addr(site, b, sizeof b));
	p = put_str(line, p, sizeof line, " -- ignored\n");
	write_all(2, line, p);
	if (count == 1) ps2_trace_dump(2, 64);
}

PS2_WEAK void ps2_call_indirect(ps2_cpu *c, uint32_t pc, uint32_t target)
{
	ps2_func_t f = ps2_lookup_function(ps2_recomp_functions, ps2_recomp_function_count, target);
	if (PS2_LIKELY(f != NULL)) {
		f(c);
		return;
	}
	ps2_dispatch_unknown(c, pc, target, PS2_DISPATCH_CALL);
}

PS2_WEAK void ps2_jump_indirect(ps2_cpu *c, uint32_t pc, uint32_t target)
{
	ps2_func_t f = ps2_lookup_function(ps2_recomp_functions, ps2_recomp_function_count, target);
	if (PS2_LIKELY(f != NULL)) {
		f(c);
		return;
	}
	ps2_dispatch_unknown(c, pc, target, PS2_DISPATCH_JUMP);
}

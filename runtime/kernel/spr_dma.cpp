// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scratchpad DMA (DMAC channel 8 fromSPR, channel 9 toSPR). vc_gs does not implement these
// channels and reports their start through the dma_start callback (OnDmaStart); the transfer
// is then run here, at the next interrupt delivery point (outside the vc_gs call), to
// completion. Channel registers are accessed through vc::gs::HwRead32/HwWrite32.
//
// Supported: normal and interleave mode both ways, source chain (toSPR), destination chain
// (fromSPR). Not supported: fromSPR into an MFIFO ring (D_CTRL.MFD), stall control.

#include "kernel.h"
#include "log.h"
#include "vc_gs.h"

#include <cstdlib>
#include <cstring>

namespace vc::kernel {

namespace {

constexpr uint32_t kBase[2] = {0x1000D000u, 0x1000D400u}; // ch8, ch9
constexpr uint32_t CHCR = 0x00, MADR = 0x10, QWC = 0x20, TADR = 0x30, SADR = 0x80;
constexpr uint32_t D_CTRL = 0x1000E000u, D_SQWC = 0x1000E030u;

bool g_pending[2] = {};

uint32_t Rd(uint32_t a)
{
	uint32_t v = 0;
	vc::gs::HwRead32(a, &v);
	return v;
}

void Wr(uint32_t a, uint32_t v) { vc::gs::HwWrite32(a, v); }

uint8_t* RamQw(uint32_t addr)
{
	// MADR/TADR: bit 31 selects the scratchpad for other channels; for SPR channels the
	// address is in main RAM.
	return EePtr(addr & 0x01FFFFF0u, 16);
}

uint8_t* SprQw(uint32_t sadr) { return g_spr + (sadr & 0x3FF0u); }

// Copy `qwc` quadwords between RAM `madr` and SPR `sadr`. Returns the advanced addresses.
void Copy(bool to_spr, uint32_t& madr, uint32_t& sadr, uint32_t qwc)
{
	for (uint32_t i = 0; i < qwc; i++) {
		uint8_t* ram = RamQw(madr);
		uint8_t* spr = SprQw(sadr);
		if (ram) {
			if (to_spr)
				std::memcpy(spr, ram, 16);
			else
				std::memcpy(ram, spr, 16);
		}
		madr += 16;
		sadr = (sadr + 16) & 0x3FF0u;
	}
}

void Run(int idx)
{
	const bool to_spr = idx == 1;
	const int channel = 8 + idx;
	const uint32_t base = kBase[idx];
	uint32_t chcr = Rd(base + CHCR);
	if (!(chcr & 0x100))
		return;
	uint32_t madr = Rd(base + MADR), qwc = Rd(base + QWC) & 0xFFFF, tadr = Rd(base + TADR);
	uint32_t sadr = Rd(base + SADR) & 0x3FF0u;
	const uint32_t mode = (chcr >> 2) & 3;
	static const bool log = getenv("VC_LOG_SPR") != nullptr;
	if (log) {
		Log("dmac", "%s CHCR %08x MADR %08x QWC %u TADR %08x SADR %04x", to_spr ? "toSPR" : "fromSPR", chcr, madr, qwc,
			tadr, sadr);
	}

	if (!to_spr && (Rd(D_CTRL) & 0xC) >= 8) {
		static bool warned;
		if (!warned) {
			warned = true;
			Log("dmac", "fromSPR into MFIFO (D_CTRL %08x) not supported", Rd(D_CTRL));
		}
	}

	if (mode == 0) {
		Copy(to_spr, madr, sadr, qwc);
		qwc = 0;
	} else if (mode == 2) {
		const uint32_t sqwc = Rd(D_SQWC);
		const uint32_t skip = sqwc & 0xFF, tqwc = (sqwc >> 16) & 0xFF;
		while (qwc) {
			const uint32_t n = tqwc ? (qwc < tqwc ? qwc : tqwc) : qwc;
			Copy(to_spr, madr, sadr, n);
			qwc -= n;
			if (qwc)
				madr += skip * 16;
		}
	} else if (to_spr) {
		// Source chain: tags in RAM at TADR (and the tag itself goes to SPR with TTE).
		Copy(true, madr, sadr, qwc); // remaining data of the current tag
		for (int guard = 0; guard < 1 << 20; guard++) {
			const uint8_t* t = RamQw(tadr);
			if (!t)
				break;
			uint64_t tag;
			std::memcpy(&tag, t, 8);
			const uint32_t tqwc = static_cast<uint32_t>(tag & 0xFFFF);
			const uint32_t id = static_cast<uint32_t>(tag >> 28) & 7;
			const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
			const bool irq = (tag >> 31) & 1;
			chcr = (chcr & 0xFFFF) | (static_cast<uint32_t>(tag >> 16) & 0xFFFF0000u);
			if (chcr & 0x40) { // TTE
				uint32_t ta = tadr, sa = sadr;
				Copy(true, ta, sa, 1);
				sadr = sa;
			}
			bool end = false;
			switch (id) {
			case 1: // cnt
				madr = tadr + 16;
				Copy(true, madr, sadr, tqwc);
				tadr = madr;
				break;
			case 2: // next
				madr = tadr + 16;
				Copy(true, madr, sadr, tqwc);
				tadr = addr;
				break;
			case 3: // ref
			case 4: // refs
				madr = addr;
				Copy(true, madr, sadr, tqwc);
				tadr += 16;
				break;
			case 0: // refe
				madr = addr;
				Copy(true, madr, sadr, tqwc);
				tadr += 16;
				end = true;
				break;
			case 7: // end
				madr = tadr + 16;
				Copy(true, madr, sadr, tqwc);
				end = true;
				break;
			default:
				Log("dmac", "toSPR: unsupported chain tag id %u at %08x", id, tadr);
				end = true;
				break;
			}
			if (end || (irq && (chcr & 0x80)))
				break;
		}
		qwc = 0;
	} else {
		// Destination chain: tags come from SPR, data goes to the tag's address.
		Copy(false, madr, sadr, qwc);
		for (int guard = 0; guard < 1 << 20; guard++) {
			uint64_t tag;
			std::memcpy(&tag, SprQw(sadr), 8);
			sadr = (sadr + 16) & 0x3FF0u;
			const uint32_t tqwc = static_cast<uint32_t>(tag & 0xFFFF);
			const uint32_t id = static_cast<uint32_t>(tag >> 28) & 7;
			const bool irq = (tag >> 31) & 1;
			madr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
			chcr = (chcr & 0xFFFF) | (static_cast<uint32_t>(tag >> 16) & 0xFFFF0000u);
			Copy(false, madr, sadr, tqwc);
			if (id == 7 || (irq && (chcr & 0x80))) // dest-chain "end" is id 7
				break;
		}
		qwc = 0;
	}

	Wr(base + CHCR, chcr & ~0x100u); // stop
	Wr(base + MADR, madr);
	Wr(base + QWC, qwc);
	Wr(base + TADR, tadr);
	Wr(base + SADR, sadr);
	vc::gs::RaiseDmac(channel);
}

} // namespace

void SprDmaStart(int channel)
{
	if (channel == 8 || channel == 9)
		g_pending[channel - 8] = true;
}

void SprDmaRunPending()
{
	for (int i = 0; i < 2; i++) {
		if (g_pending[i]) {
			g_pending[i] = false;
			Run(i);
		}
	}
}

bool SprDmaPending() { return g_pending[0] || g_pending[1]; }

} // namespace vc::kernel

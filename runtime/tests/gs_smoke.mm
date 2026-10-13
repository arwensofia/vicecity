// SPDX-License-Identifier: GPL-3.0-or-later
//
// gs_smoke: opens a window and draws through all three GIF paths, then reads the displayed
// image back and checks it.
//
//   PATH3, HLE        vc::gs::SubmitPath3        full-screen clear (dark blue sprite)
//   PATH3, DMAC       GIF DMA (D2) normal mode   red sprite
//   PATH2, VIF FIFO   vc::gs::SubmitPath2        DIRECT -> green sprite
//   PATH1, DMAC       VIF1 DMA (D1) chain mode:  STCYCL, MPG (microprogram), UNPACK (GIF packet
//                     into VU1 memory), MSCAL -> VU1 XGKICK -> yellow sprite
//
// usage: gs_smoke [--headless] [--renderer=metal|sw|null] [--frames=N] [--keep-open] [--dump=prefix]
// Exit status 0 when every probe pixel matches.

#import <AppKit/AppKit.h>

#include "vc_gs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t kWidth = 640;
	constexpr uint32_t kHeight = 448;

	// --- GS packet building -------------------------------------------------------------

	enum GsReg : uint64_t
	{
		PRIM = 0x00, RGBAQ = 0x01, XYZ2 = 0x05, XYOFFSET_1 = 0x18, PRMODECONT = 0x1a,
		SCISSOR_1 = 0x40, TEST_1 = 0x47, FRAME_1 = 0x4c, ZBUF_1 = 0x4e,
	};

	struct Packet
	{
		std::vector<uint64_t> q; // pairs of 64-bit words = quadwords

		size_t qwc() const { return q.size() / 2; }
		const void* data() const { return q.data(); }

		// GIFtag, PACKED mode, one A+D register per loop.
		void tag_ad(uint32_t nloop, bool eop)
		{
			q.push_back(uint64_t(nloop) | (uint64_t(eop) << 15) | (0ull << 58) | (1ull << 60));
			q.push_back(0xe); // A+D
		}
		void ad(uint64_t reg, uint64_t value)
		{
			q.push_back(value);
			q.push_back(reg);
		}
	};

	uint64_t rgbaq(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0x80)
	{
		const float one = 1.0f;
		uint32_t qbits;
		std::memcpy(&qbits, &one, 4);
		return uint64_t(r) | (uint64_t(g) << 8) | (uint64_t(b) << 16) | (uint64_t(a) << 24) |
			(uint64_t(qbits) << 32);
	}

	uint64_t xyz(uint32_t x, uint32_t y) { return uint64_t(x << 4) | (uint64_t(y << 4) << 16); }

	// Drawing environment + one sprite, as one GIF packet.
	Packet sprite_packet(bool with_env, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r, uint8_t g,
		uint8_t b)
	{
		Packet p;
		p.tag_ad(with_env ? 10 : 4, true);
		if (with_env)
		{
			p.ad(FRAME_1, 0 | (uint64_t(kWidth / 64) << 16) | (0ull << 24));          // FBP 0, PSMCT32
			p.ad(ZBUF_1, uint64_t(0x70) | (0ull << 24) | (1ull << 32));                // ZMSK
			p.ad(TEST_1, (1ull << 16) | (1ull << 17));                                 // ZTE, ZTST=ALWAYS
			p.ad(SCISSOR_1, (uint64_t(kWidth - 1) << 16) | (uint64_t(kHeight - 1) << 48));
			p.ad(XYOFFSET_1, 0);
			p.ad(PRMODECONT, 1);
		}
		p.ad(PRIM, 6); // sprite, flat, untextured
		p.ad(RGBAQ, rgbaq(r, g, b));
		p.ad(XYZ2, xyz(x0, y0));
		p.ad(XYZ2, xyz(x1, y1));
		return p;
	}

	// --- VIF / DMA helpers --------------------------------------------------------------

	constexpr uint32_t VIF_NOP = 0x00000000;
	uint32_t vif_stcycl(uint32_t cl, uint32_t wl) { return (0x01u << 24) | (wl << 8) | cl; }
	uint32_t vif_mscal(uint32_t addr) { return (0x14u << 24) | addr; }
	uint32_t vif_flush() { return 0x11u << 24; }
	uint32_t vif_mpg(uint32_t num, uint32_t addr) { return (0x4au << 24) | ((num & 0xff) << 16) | addr; }
	uint32_t vif_unpack_v4_32(uint32_t num, uint32_t addr) { return (0x6cu << 24) | ((num & 0xff) << 16) | addr; }
	uint32_t vif_direct(uint32_t qwc) { return (0x50u << 24) | qwc; }

	void pad_to(std::vector<uint32_t>& w, size_t multiple, size_t offset = 0)
	{
		while ((w.size() + offset) % multiple)
			w.push_back(VIF_NOP);
	}

	void append_qwords(std::vector<uint32_t>& w, const Packet& p)
	{
		const uint32_t* src = reinterpret_cast<const uint32_t*>(p.q.data());
		w.insert(w.end(), src, src + p.q.size() * 2);
	}

	// VU1 microprogram: vi01 = 0; xgkick vi01; end.
	const uint32_t kVu1Program[] = {
		0x10010000, 0x000002ff, // iaddiu vi01, vi00, 0      | nop
		0x80000efc, 0x400002ff, // xgkick vi01               | nop [E]
		0x8000033c, 0x000002ff, // nop                       | nop   (E-bit delay slot)
	};

	// --- result checking ----------------------------------------------------------------

	struct Probe
	{
		const char* what;
		uint32_t x, y;
		uint8_t r, g, b;
	};

	const Probe kProbes[] = {
		{"background (PATH3 HLE clear)", 320, 320, 0, 0, 64},
		{"red sprite (PATH3 via GIF DMA)", 128, 128, 255, 0, 0},
		{"green sprite (PATH2 via VIF1 DIRECT)", 320, 128, 0, 255, 0},
		{"yellow sprite (PATH1 via VIF1 DMA + VU1 XGKICK)", 512, 128, 255, 255, 0},
	};

	const char* s_dump_prefix = nullptr;

	// Writes an RGBA8 image as binary PPM (for --dump).
	void dump_ppm(const char* suffix, const std::vector<uint32_t>& img, uint32_t w, uint32_t h)
	{
		if (!s_dump_prefix)
			return;
		const std::string path = std::string(s_dump_prefix) + suffix + ".ppm";
		FILE* f = std::fopen(path.c_str(), "wb");
		if (!f)
			return;
		std::fprintf(f, "P6\n%u %u\n255\n", w, h);
		for (uint32_t px : img)
		{
			const unsigned char rgb[3] = {static_cast<unsigned char>(px), static_cast<unsigned char>(px >> 8),
				static_cast<unsigned char>(px >> 16)};
			std::fwrite(rgb, 1, 3, f);
		}
		std::fclose(f);
		std::printf("gs_smoke: wrote %s\n", path.c_str());
	}

	int s_int0_count = 0;
	int s_int1_count = 0;

	void build_and_submit_frame()
	{
		vc::gs::Memory mem = vc::gs::GetMemory();

		// PATH3, HLE: clear.
		const Packet clear = sprite_packet(true, 0, 0, kWidth, kHeight, 0, 0, 64);
		vc::gs::SubmitPath3(clear.data(), clear.qwc());

		// PATH3 through the DMAC: GIF DMA normal mode from EE RAM 0x100000.
		{
			const Packet red = sprite_packet(false, 64, 64, 192, 192, 255, 0, 0);
			const uint32_t addr = 0x100000;
			std::memcpy(mem.ee_ram + addr, red.data(), red.qwc() * 16);
			vc::gs::HwWrite32(0x1000a010, addr);                               // D2_MADR
			vc::gs::HwWrite32(0x1000a020, static_cast<uint32_t>(red.qwc()));    // D2_QWC
			vc::gs::HwWrite32(0x1000a000, 0x101);                              // D2_CHCR: from mem, normal, STR
		}

		// PATH2 through the VIF1 FIFO: DIRECT (the VIF code must be the last word of a qword).
		{
			const Packet green = sprite_packet(false, 256, 64, 384, 192, 0, 255, 0);
			std::vector<uint32_t> w = {VIF_NOP, VIF_NOP, VIF_NOP, vif_direct(static_cast<uint32_t>(green.qwc()))};
			append_qwords(w, green);
			vc::gs::SubmitPath2(w.data(), w.size() / 4);
		}

		// PATH1 through VIF1 chain DMA: microcode + GIF packet into VU1, then MSCAL.
		{
			const Packet yellow = sprite_packet(false, 448, 64, 576, 192, 255, 255, 0);
			std::vector<uint32_t> w;
			w.push_back(vif_stcycl(1, 1));
			const uint32_t ninsn = sizeof(kVu1Program) / 8;
			w.push_back(vif_mpg(ninsn, 0)); // at an odd word, so the microcode starts 64-bit aligned
			w.insert(w.end(), std::begin(kVu1Program), std::end(kVu1Program));
			w.push_back(vif_unpack_v4_32(static_cast<uint32_t>(yellow.qwc()), 0));
			append_qwords(w, yellow);
			w.push_back(vif_flush());
			w.push_back(vif_mscal(0));
			pad_to(w, 4);

			// Chain: CNT tag with the VIF stream, END tag with nothing.
			const uint32_t base = 0x200000;
			const uint32_t data_qwc = static_cast<uint32_t>(w.size() / 4);
			uint64_t cnt[2] = {uint64_t(data_qwc) | (1ull << 28), 0};
			uint64_t end[2] = {0ull | (7ull << 28), 0};
			std::memcpy(mem.ee_ram + base, cnt, 16);
			std::memcpy(mem.ee_ram + base + 16, w.data(), w.size() * 4);
			std::memcpy(mem.ee_ram + base + 16 + data_qwc * 16, end, 16);
			vc::gs::HwWrite32(0x10009030, base); // D1_TADR
			vc::gs::HwWrite32(0x10009020, 0);    // D1_QWC
			vc::gs::HwWrite32(0x10009000, 0x105); // D1_CHCR: from mem, chain, STR
		}
	}

	void setup_display()
	{
		vc::gs::SetGsCrt(true, 2, false); // NTSC, interlaced, field mode (full 448-line frame buffer)
		vc::gs::GsPrivWrite64(0x12000000, 0xff25);                            // PMODE: EN1, ALP=FF
		vc::gs::GsPrivWrite64(0x12000070, uint64_t(kWidth / 64) << 9);         // DISPFB1: FBP 0, FBW 10
		vc::gs::GsPrivWrite64(0x12000080, 636ull | (50ull << 12) | (3ull << 23) | (0ull << 27) |
			(uint64_t(kWidth * 4 - 1) << 32) | (uint64_t(kHeight - 1) << 44)); // DISPLAY1
		vc::gs::GsPrivWrite64(0x120000e0, 0);                                  // BGCOLOR
	}

	bool check(const std::vector<uint32_t>& img, uint32_t w, uint32_t h)
	{
		bool ok = true;
		for (const Probe& p : kProbes)
		{
			const uint32_t x = p.x * w / kWidth;
			const uint32_t y = p.y * h / kHeight;
			const uint32_t px = img[y * w + x];
			const int r = px & 0xff, g = (px >> 8) & 0xff, b = (px >> 16) & 0xff;
			const bool match = std::abs(r - p.r) <= 8 && std::abs(g - p.g) <= 8 && std::abs(b - p.b) <= 8;
			std::printf("  %-50s (%3u,%3u) = %02x%02x%02x  expect %02x%02x%02x  %s\n", p.what, x, y, r, g, b, p.r,
				p.g, p.b, match ? "OK" : "MISMATCH");
			ok &= match;
		}
		return ok;
	}
} // namespace

int main(int argc, char** argv)
{
	bool headless = false, keep_open = false;
	int frames = 30;
	vc::gs::Renderer renderer = vc::gs::Renderer::Metal;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--headless")
			headless = true;
		else if (a == "--keep-open")
			keep_open = true;
		else if (a.rfind("--dump=", 0) == 0)
			s_dump_prefix = argv[i] + 7;
		else if (a.rfind("--frames=", 0) == 0)
			frames = std::atoi(a.c_str() + 9);
		else if (a == "--renderer=sw")
			renderer = vc::gs::Renderer::Software;
		else if (a == "--renderer=null")
			renderer = vc::gs::Renderer::Null;
		else if (a == "--renderer=metal")
			renderer = vc::gs::Renderer::Metal;
		else
		{
			std::fprintf(stderr, "usage: %s [--headless] [--renderer=metal|sw|null] [--frames=N] [--keep-open] [--dump=prefix]\n", argv[0]);
			return 2;
		}
	}

	@autoreleasepool
	{
		NSWindow* window = nil;
		NSView* view = nil;
		CGFloat scale = 1.0;
		if (!headless)
		{
			[NSApplication sharedApplication];
			[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
			window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, kWidth, kHeight)
			                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
			                                       backing:NSBackingStoreBuffered
			                                         defer:NO];
			[window setTitle:@"vicecity gs_smoke"];
			view = [window contentView];
			[window makeKeyAndOrderFront:nil];
			[NSApp activateIgnoringOtherApps:YES];
			[NSApp finishLaunching];
			scale = [window backingScaleFactor];
		}

		vc::gs::Config cfg;
		cfg.ns_view = (__bridge void*)view;
		cfg.surface_width = static_cast<uint32_t>(kWidth * scale);
		cfg.surface_height = static_cast<uint32_t>(kHeight * scale);
		cfg.surface_scale = static_cast<float>(scale);
		cfg.renderer = renderer;
		cfg.callbacks.int0 = [](void*) { s_int0_count++; };
		cfg.callbacks.int1 = [](void*) { s_int1_count++; };
		if (!vc::gs::Init(cfg))
		{
			std::fprintf(stderr, "gs_smoke: vc::gs::Init failed\n");
			return 1;
		}

		setup_display();

		// Interrupt plumbing: unmask the GS interrupt in INTC and VSINT in the GS IMR (-> int0
		// at every vsync), and the GIF channel in D_STAT.CIM (-> int1 when GIF DMA ends).
		vc::gs::HwWrite32(0x1000f010, 1u << 0);        // INTC_MASK ^= GS
		vc::gs::GsPrivWrite64(0x12001010, 0x7700);     // IMR: everything masked but VSINT
		vc::gs::HwWrite32(0x1000e010, 1u << (16 + 2)); // D_STAT: toggle CIM2

		for (int f = 0; f < frames || keep_open; f++)
		{
			build_and_submit_frame();
			vc::gs::VSync();
			// What the game's handlers do: acknowledge GS VSINT (CSR) and INTC/DMAC status.
			vc::gs::GsPrivWrite64(0x12001000, 1u << 3);
			if (f + 1 < frames || keep_open)
			{
				vc::gs::HwWrite32(0x1000f000, 1u << 0);
				vc::gs::HwWrite32(0x1000e010, 1u << 2);
			}
			if (window)
			{
				while (NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])
					[NSApp sendEvent:ev];
				if (keep_open && ![window isVisible])
					break;
			}
		}

		bool ok = true;
		if (renderer == vc::gs::Renderer::Null)
		{
			std::printf("gs_smoke: null renderer, %d frames processed (no image to check)\n", frames);
		}
		else
		{
			uint32_t w = 0, h = 0;
			std::vector<uint32_t> img;
			if (!vc::gs::CaptureDisplay(&w, &h, &img) || img.empty())
			{
				std::printf("gs_smoke: CaptureDisplay failed\n");
				ok = false;
			}
			else
			{
				std::printf("gs_smoke: captured %ux%u display image\n", w, h);
				dump_ppm("_display", img, w, h);
				ok = check(img, w, h);
			}
		}

		if (renderer != vc::gs::Renderer::Null)
		{
			// The same probes straight from GS local memory (frame buffer at FBP 0), which
			// separates drawing from display (PCRTC) problems.
			std::vector<uint32_t> fb(kWidth * kHeight);
			const uint64_t bitbltbuf = (0ull << 0) | (uint64_t(kWidth / 64) << 16) | (0ull << 24); // SBP 0, SBW, PSMCT32
			const uint64_t trxreg = uint64_t(kWidth) | (uint64_t(kHeight) << 32);
			vc::gs::ReadLocalMemory(bitbltbuf, 0, trxreg, fb.data(), fb.size() / 4);
			std::printf("gs_smoke: GS local memory\n");
			dump_ppm("_vram", fb, kWidth, kHeight);
			ok &= check(fb, kWidth, kHeight);
		}

		uint32_t vu_busy = vc::gs::VU1Busy();
		uint32_t d1 = 0, d2 = 0;
		vc::gs::HwRead32(0x10009000, &d1);
		vc::gs::HwRead32(0x1000a000, &d2);
		std::printf("gs_smoke: D1_CHCR=%08x D2_CHCR=%08x VU1 busy=%u int0=%d int1=%d\n", d1, d2, vu_busy,
			s_int0_count, s_int1_count);
		uint32_t intc_stat = 0, d_stat = 0;
		vc::gs::HwRead32(0x1000f000, &intc_stat);
		vc::gs::HwRead32(0x1000e010, &d_stat);
		std::printf("gs_smoke: INTC_STAT=%08x D_STAT=%08x\n", intc_stat, d_stat);
		if (s_int0_count < frames || !(intc_stat & 1))
		{
			std::printf("gs_smoke: expected a GS (VSINT) interrupt every frame\n");
			ok = false;
		}
		if (s_int1_count < frames || !(d_stat & (1u << 2)))
		{
			std::printf("gs_smoke: expected a DMAC interrupt for every GIF DMA\n");
			ok = false;
		}
		if ((d1 & 0x100) || (d2 & 0x100))
		{
			std::printf("gs_smoke: DMA still running\n");
			ok = false;
		}

		vc::gs::Shutdown();
		std::printf("gs_smoke: %s\n", ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
}

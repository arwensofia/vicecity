// SPDX-License-Identifier: GPL-3.0-or-later
//
// vicecity: macOS host for the recompiled SLUS_205.52. Opens a window with a Metal-backed
// view, initialises the GS layer and the IOP emulation, loads the executable's segments into
// EE RAM and runs the EE kernel/program on a dedicated host thread (the "EE host thread";
// all vc_gs calls happen there, while the main thread runs the AppKit event loop).
//
//   vicecity [--iso <path>] [--elf <path>] [--log <file>] [--trace] [--syscalls]
//            [--headless] [--renderer=metal|sw|null] [--scale <n>]

#import <AppKit/AppKit.h>

#include "elf_loader.h"
#include "kernel.h"
#include "log.h"
#include "vc_gs.h"
#include "vc_iop.h"
#include "vc_pad.h"

#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

struct Options
{
	std::string iso;
	std::string elf;
	std::string log;
	bool trace = false;
	bool syscalls = false;
	bool headless = false;
	vc::gs::Renderer renderer = vc::gs::Renderer::Metal;
	double scale = 2.0;
	std::vector<std::pair<uint64_t, std::string>> screenshots;
};

struct EEStart
{
	Options opt;
	void* view = nullptr;
	uint32_t width = 640, height = 448;
	float backing = 1.0f;
};

constexpr int kWidth = 640;
constexpr int kHeight = 448;
constexpr const char* kElfDiscPath = "cdrom0:\\SLUS_205.52;1";

void Usage()
{
	std::fprintf(stderr,
		"usage: vicecity [--iso <path>] [--elf <path>] [--log <file>] [--trace] [--syscalls]\n"
		"                [--headless] [--renderer=metal|sw|null] [--scale <n>] [--screenshot ...]\n"
		"  --iso       disc image (read by the IOP emulation; also used to load the executable\n"
		"              when --elf is not given and game/SLUS_205.52 does not exist)\n"
		"  --elf       the executable (default: game/SLUS_205.52)\n"
		"  --log       also write the runtime log to a file\n"
		"  --trace     print every recompiled function entry (needs a PS2_TRACE build)\n"
		"  --syscalls  log every EE syscall\n"
		"  --screenshot <vblank>:<file.ppm>  save the displayed image at that vblank (repeatable)\n");
}

bool ParseArgs(int argc, char** argv, Options* o)
{
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto value = [&](std::string* out) {
			if (i + 1 >= argc)
				return false;
			*out = argv[++i];
			return true;
		};
		if (a == "--iso") {
			if (!value(&o->iso))
				return false;
		} else if (a == "--elf") {
			if (!value(&o->elf))
				return false;
		} else if (a == "--log") {
			if (!value(&o->log))
				return false;
		} else if (a == "--trace") {
			o->trace = true;
		} else if (a == "--syscalls") {
			o->syscalls = true;
		} else if (a == "--headless") {
			o->headless = true;
		} else if (a == "--renderer=sw") {
			o->renderer = vc::gs::Renderer::Software;
		} else if (a == "--renderer=null") {
			o->renderer = vc::gs::Renderer::Null;
		} else if (a == "--renderer=metal") {
			o->renderer = vc::gs::Renderer::Metal;
		} else if (a == "--scale") {
			std::string s;
			if (!value(&s))
				return false;
			o->scale = std::atof(s.c_str());
			if (o->scale < 1.0)
				o->scale = 1.0;
		} else if (a == "--screenshot") { // --screenshot <vblank>:<file.ppm>
			std::string v;
			if (!value(&v) || v.find(':') == std::string::npos)
				return false;
			o->screenshots.emplace_back(std::strtoull(v.c_str(), nullptr, 10), v.substr(v.find(':') + 1));
		} else if (a == "-h" || a == "--help") {
			return false;
		} else if (a.rfind("-psn_", 0) == 0) {
			// Finder process serial number
		} else {
			std::fprintf(stderr, "vicecity: unknown argument %s\n", a.c_str());
			return false;
		}
	}
	return true;
}

std::string DataDir()
{
	const char* home = std::getenv("HOME");
	std::string dir = std::string(home ? home : ".") + "/Library/Application Support/vicecity";
	mkdir(dir.c_str(), 0755);
	return dir;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > (64 << 20)) {
		std::fclose(f);
		return false;
	}
	out->resize(static_cast<size_t>(n));
	const size_t got = std::fread(out->data(), 1, out->size(), f);
	std::fclose(f);
	return got == out->size();
}

int RunEE(const EEStart& s)
{
	const Options& opt = s.opt;
	vc::gs::Config cfg;
	cfg.ns_view = s.view;
	cfg.surface_width = s.width;
	cfg.surface_height = s.height;
	cfg.surface_scale = s.backing;
	cfg.renderer = s.view ? opt.renderer : vc::gs::Renderer::Null;
	cfg.callbacks.int0 = vc::kernel::OnInt0;
	cfg.callbacks.int1 = vc::kernel::OnInt1;
	cfg.callbacks.dma_start = vc::kernel::OnDmaStart;
	if (!vc::gs::Init(cfg))
		vc::Fatal("vc::gs::Init failed");
	const vc::gs::Memory& mem = vc::gs::GetMemory();
	vc::kernel::InitMemory(mem.ee_ram, mem.scratchpad, mem.vu0_mem, mem.vu0_micro, mem.vu1_mem, mem.vu1_micro);

	const std::string data_dir = DataDir();
	vc_iop_config iop{};
	iop.iso_path = opt.iso.empty() ? nullptr : opt.iso.c_str();
	iop.data_dir = data_dir.c_str();
	iop.ee_ram = mem.ee_ram;
	if (vc_iop_init(&iop) != 0)
		vc::Fatal("IOP initialisation failed");

	// The executable: --elf, else game/SLUS_205.52, else from the disc image.
	std::vector<uint8_t> elf;
	std::string source = opt.elf.empty() ? "game/SLUS_205.52" : opt.elf;
	if (!ReadFile(source, &elf)) {
		if (!opt.elf.empty())
			vc::Fatal("cannot read %s", opt.elf.c_str());
		void* data = nullptr;
		size_t size = 0;
		if (vc_iop_read_disc_file(kElfDiscPath, &data, &size) != 0)
			vc::Fatal("no executable: pass --elf <path>, or --iso <path> with an IOP implementation");
		elf.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
		std::free(data);
		source = std::string(kElfDiscPath) + " (from disc)";
	}
	vc::app::ElfImage img;
	std::string err;
	if (!vc::app::LoadElf(elf.data(), elf.size(), mem.ee_ram, PS2_RAM_SIZE, &img, &err))
		vc::Fatal("%s: %s", source.c_str(), err.c_str());
	vc::Log("app", "loaded %s: %d segments %08x-%08x, entry %08x", source.c_str(), img.segments, img.lowest,
		img.highest, img.entry);

	vc::kernel::BootConfig boot;
	boot.entry = img.entry;
	boot.argv = {kElfDiscPath};
	boot.log_syscalls = opt.syscalls;
	if (opt.syscalls)
		setenv("VC_LOG_SYSCALLS", "1", 1);
	vc::kernel::SetTracePrint(opt.trace);
	for (const auto& [when, path] : opt.screenshots)
		vc::kernel::AddScreenshot(when, path);
	const int code = vc::kernel::Run(boot);
	vc::kernel::ExitProgram(code);
}

void* EEThreadMain(void* arg)
{
	pthread_setname_np("EE");
	RunEE(*static_cast<EEStart*>(arg));
	return nullptr;
}

void StartEEThread(EEStart* s)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, size_t{64} << 20); // EE main thread runs on this stack
	pthread_t t;
	if (pthread_create(&t, &attr, EEThreadMain, s) != 0)
		vc::Fatal("cannot create the EE host thread");
	pthread_attr_destroy(&attr);
}

} // namespace

// Content view: hosts the GS layer and forwards keys to the pad emulation (vc_pad.h maps
// macOS virtual key codes to buttons; thread-safe).
@interface VCGameView : NSView
@end

@implementation VCGameView
- (BOOL)acceptsFirstResponder
{
	return YES;
}
- (void)keyDown:(NSEvent*)event
{
	if (!event.isARepeat)
		vc_pad_key(event.keyCode, 1);
}
- (void)keyUp:(NSEvent*)event
{
	vc_pad_key(event.keyCode, 0);
}
- (void)flagsChanged:(NSEvent*)event
{
	// Modifier keys arrive here; down if the key's modifier flag is now set.
	const NSEventModifierFlags f = event.modifierFlags;
	bool down = false;
	switch (event.keyCode) {
	case 56: case 60: down = f & NSEventModifierFlagShift; break;
	case 59: case 62: down = f & NSEventModifierFlagControl; break;
	case 58: case 61: down = f & NSEventModifierFlagOption; break;
	case 55: case 54: down = f & NSEventModifierFlagCommand; break;
	default: return;
	}
	vc_pad_key(event.keyCode, down ? 1 : 0);
}
@end

@interface VCWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation VCWindowDelegate
- (void)windowWillClose:(NSNotification*)notification
{
	(void)notification;
	vc::Log("app", "window closed");
	vc::LogShutdown();
	_exit(0);
}
@end

int main(int argc, char** argv)
{
	Options opt;
	if (!ParseArgs(argc, argv, &opt)) {
		Usage();
		return 2;
	}
	vc::LogInit(opt.log.empty() ? nullptr : opt.log.c_str());
	vc::kernel::InstallCrashHandlers();

	static EEStart start;
	start.opt = opt;

	if (opt.headless) {
		// The GS device still attaches to the main queue (dispatch_sync), so keep it serviced.
		StartEEThread(&start);
		for (;;)
			CFRunLoopRun();
	}

	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		const NSRect rect = NSMakeRect(100, 100, kWidth * opt.scale, kHeight * opt.scale);
		NSWindow* window = [[NSWindow alloc] initWithContentRect:rect
		                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		                                                         NSWindowStyleMaskMiniaturizable
		                                                 backing:NSBackingStoreBuffered
		                                                   defer:NO];
		[window setTitle:@"Vice City"];
		static VCWindowDelegate* delegate = [[VCWindowDelegate alloc] init];
		[window setDelegate:delegate];
		[window setReleasedWhenClosed:NO];
		NSView* view = [[VCGameView alloc] initWithFrame:rect];
		[view setWantsLayer:YES];
		[window setContentView:view];
		[window makeFirstResponder:view];
		[window makeKeyAndOrderFront:nil];
		[NSApp activateIgnoringOtherApps:YES];

		const double backing = [window backingScaleFactor];
		start.view = (__bridge_retained void*)view;
		start.width = static_cast<uint32_t>(kWidth * opt.scale * backing);
		start.height = static_cast<uint32_t>(kHeight * opt.scale * backing);
		start.backing = static_cast<float>(backing);
		StartEEThread(&start);
		[NSApp run];
	}
	return 0;
}

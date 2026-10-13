// SPDX-License-Identifier: GPL-3.0-or-later
//
// Implementations of the PCSX2 frontend/VM services that the imported GS, VU and VIF code
// calls but vicecity has no use for (UI, OSD, settings store, capture, GS dumps, memory
// cards, pads, USB, achievements...). Everything here is either a no-op or returns the
// neutral value PCSX2 itself would use with the feature disabled.

#include "pcsx2_host.h"

#include "Config.h"
#include "Counters.h"
#include "GS.h"
#include "GS/GS.h"
#include "GS/GSCapture.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "Host/AudioStream.h"
#include "ImGui/FullscreenUI.h"
#include "PerformanceMetrics.h"
#include "R5900OpcodeTables.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "SaveState.h"
#include "USB/USB.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/HostSys.h"
#include "DebugTools/Debug.h"

#include <mutex>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------

namespace
{
	std::mutex s_translate_mutex;
	std::unordered_map<std::string, std::string> s_translate_cache;
} // namespace

const char* Host::TranslateToCString(const std::string_view, const std::string_view msg)
{
	// Callers keep the pointer, so it must stay valid: intern the string.
	std::lock_guard lock(s_translate_mutex);
	auto [it, inserted] = s_translate_cache.try_emplace(std::string(msg), msg);
	return it->second.c_str();
}

std::string_view Host::TranslateToStringView(const std::string_view context, const std::string_view msg)
{
	return TranslateToCString(context, msg);
}

std::string Host::TranslateToString(const std::string_view, const std::string_view msg)
{
	return std::string(msg);
}

void Host::AddKeyedOSDMessage(std::string, std::string message, float)
{
	Console.WriteLn("[GS OSD] %s", message.c_str());
}

void Host::AddIconOSDMessage(std::string, const char*, const std::string_view message, float)
{
	Console.WriteLn("[GS OSD] %.*s", static_cast<int>(message.size()), message.data());
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	Console.Error("%.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()),
		message.data());
}

bool Host::GetBaseBoolSettingValue(const char*, const char*, bool default_value)
{
	return default_value;
}

std::string Host::GetStringSettingValue(const char*, const char*, const char* default_value)
{
	return default_value ? std::string(default_value) : std::string();
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
	return vc::gs::host::GetWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

// ---------------------------------------------------------------------------------------
// Fullscreen UI: not used. (ImGuiManager lives in ImGuiHost.cpp.)
// ---------------------------------------------------------------------------------------

void FullscreenUI::Render() {}

// ---------------------------------------------------------------------------------------
// Video capture (needs FFmpeg) and GS dump replay: not imported.
// ---------------------------------------------------------------------------------------

bool GSCapture::BeginCapture(float, GSVector2i, float, std::string) { return false; }
bool GSCapture::DeliverVideoFrame(GSTexture*) { return false; }
void GSCapture::EndCapture() {}
void GSCapture::Flush() {}
bool GSCapture::IsCapturing() { return false; }
bool GSCapture::IsCapturingVideo() { return false; }
GSVector2i GSCapture::GetSize() { return GSVector2i(0, 0); }
std::string GSCapture::GetNextCaptureFileName() { return {}; }

bool GSDumpReplayer::IsReplayingDump() { return false; }
bool GSDumpReplayer::IsRunner() { return false; }
void GSDumpReplayer::RenderUI() {}
void GSDumpReplayer::UpdateGSStats() {}

// ---------------------------------------------------------------------------------------
// Texture replacement packs (need libzip): not imported. No replacements, no dumping.
// ---------------------------------------------------------------------------------------

void GSTextureReplacements::Initialize() {}
void GSTextureReplacements::GameChanged() {}
void GSTextureReplacements::ReloadReplacementMap() {}
void GSTextureReplacements::UpdateConfig(Pcsx2Config::GSOptions&) {}
void GSTextureReplacements::Shutdown() {}
bool GSTextureReplacements::HasAnyReplacementTextures() { return false; }
bool GSTextureReplacements::HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey&) { return false; }
GSTexture* GSTextureReplacements::LookupReplacementTexture(
	const GSTextureCache::HashCacheKey&, bool, bool* pending, std::pair<u8, u8>*)
{
	if (pending)
		*pending = false;
	return nullptr;
}
void GSTextureReplacements::ProcessAsyncLoadedTextures() {}
void GSTextureReplacements::DumpTexture(const GSTextureCache::HashCacheKey&, const GIFRegTEX0&, const GIFRegTEXA&,
	GSTextureCache::SourceRegion, GSLocalMemory&, u32)
{
}

// ---------------------------------------------------------------------------------------
// Performance metrics / VM manager
// ---------------------------------------------------------------------------------------

void PerformanceMetrics::Update(bool, bool, bool) {}
void PerformanceMetrics::OnGPUPresent(float, u64, u64) {}
void PerformanceMetrics::SetGSSWThreadCount(u32) {}
void PerformanceMetrics::SetGSSWThread(u32, Threading::ThreadHandle) {}
PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
	return InternalFPSMethod::None;
}
void PerformanceMetrics::StartSavingMetrics(u32) {}
bool PerformanceMetrics::IsSavingMetrics() { return false; }
void PerformanceMetrics::DumpSavedMetrics() {}

bool VMManager::HasValidVM() { return true; }
std::string VMManager::GetDiscSerial() { return "SLUS-20552"; }
std::string VMManager::GetTitle(bool) { return "Grand Theft Auto: Vice City"; }
u32 VMManager::GetDiscCRC() { return 0; }
const std::vector<u32>& VMManager::Internal::GetSoftwareRendererProcessorList()
{
	static const std::vector<u32> empty;
	return empty;
}

// ---------------------------------------------------------------------------------------
// Settings plumbing referenced by Pcsx2Config.cpp (only used when loading settings files,
// which we never do; defaults come from the constructors).
// ---------------------------------------------------------------------------------------

std::optional<AudioBackend> AudioStream::ParseBackendName(const char*) { return std::nullopt; }
const char* AudioStream::GetBackendName(AudioBackend) { return "Null"; }
void AudioStreamParameters::LoadSave(SettingsWrapper&, const char*) {}

uint FileMcd_GetMtapPort(uint slot) { return (slot >= 2) ? 1 : 0; }
uint FileMcd_GetMtapSlot(uint slot) { return (slot >= 2) ? (slot - 2) % 3 + 1 : 0; }
bool FileMcd_IsMultitapSlot(uint slot) { return slot >= 2; }
std::string FileMcd_GetDefaultName(uint slot) { return "Mcd00" + std::to_string(slot + 1) + ".ps2"; }

namespace
{
	const Pad::ControllerInfo s_dualshock2 = {Pad::ControllerType::DualShock2, "DualShock2", "DualShock 2",
		nullptr, {}, {}, Pad::VibrationCapabilities::LargeSmallMotors};
} // namespace

Pad::ControllerType Pad::GetDefaultPadType(u32 pad)
{
	return (pad == 0) ? ControllerType::DualShock2 : ControllerType::NotConnected;
}
const Pad::ControllerInfo* Pad::GetControllerInfo(Pad::ControllerType) { return &s_dualshock2; }
const Pad::ControllerInfo* Pad::GetControllerInfoByName(const std::string_view) { return &s_dualshock2; }
std::string Pad::GetConfigSection(u32 pad_index) { return "Pad" + std::to_string(pad_index + 1); }

s32 USB::DeviceTypeNameToIndex(const std::string_view) { return 0; }
const char* USB::DeviceTypeIndexToName(s32) { return "None"; }
std::string USB::GetConfigSection(int port) { return "USB" + std::to_string(port + 1); }

// ---------------------------------------------------------------------------------------
// Video mode reporting (Counters.cpp). The runtime owns vblank timing; see vc::gs::VSync.
// ---------------------------------------------------------------------------------------

const char* ReportVideoMode() { return "NTSC"; }
const char* ReportInterlaceMode() { return "Interlaced"; }
void UpdateVSyncRate(bool) {}
double GetVerticalFrequency() { return 59.94; }

// ---------------------------------------------------------------------------------------
// EE interpreter COP2 dispatch tables: VU0.cpp references them for macro-mode COP2, which
// recompiled code implements natively, so they are never called.
// ---------------------------------------------------------------------------------------

namespace
{
	void UnusedCop2Op()
	{
		pxFailRel("EE interpreter COP2 op called; macro mode is handled by recompiled code");
	}
} // namespace

#define VC_FILL4(f) f, f, f, f
#define VC_FILL16(f) VC_FILL4(f), VC_FILL4(f), VC_FILL4(f), VC_FILL4(f)
void (*Int_COP2BC2PrintTable[32])() = {VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op)};
void (*Int_COP2SPECIAL1PrintTable[64])() = {VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op),
	VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op)};
void (*Int_COP2SPECIAL2PrintTable[128])() = {VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op),
	VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op),
	VC_FILL16(UnusedCop2Op), VC_FILL16(UnusedCop2Op)};
#undef VC_FILL16
#undef VC_FILL4

// ---------------------------------------------------------------------------------------
// Save states: not supported (the recompiled game has its own save system).
// ---------------------------------------------------------------------------------------

bool SaveStateBase::FreezeTag(const char*)
{
	return false;
}

// ---------------------------------------------------------------------------------------
// Fastmem page-fault handler: we use no fastmem, so faults are never ours.
// ---------------------------------------------------------------------------------------

PageFaultHandler::HandlerResult PageFaultHandler::HandlePageFault(void*, void*, bool)
{
	return HandlerResult::ExecuteNextHandler;
}

// ---------------------------------------------------------------------------------------
// COP2 register name tables used by the VU micro disassemblers (debug-build VU logging).
// Upstream they live in the EE disassembler (DebugTools/DisR5900asm.cpp), which we don't
// import; same contents.
// ---------------------------------------------------------------------------------------

namespace R5900
{
	const char* const COP2_REG_FP[32] = {
		"vf00", "vf01", "vf02", "vf03", "vf04", "vf05", "vf06", "vf07",
		"vf08", "vf09", "vf10", "vf11", "vf12", "vf13", "vf14", "vf15",
		"vf16", "vf17", "vf18", "vf19", "vf20", "vf21", "vf22", "vf23",
		"vf24", "vf25", "vf26", "vf27", "vf28", "vf29", "vf30", "vf31"};

	const char* const COP2_REG_CTL[32] = {
		"vi00", "vi01", "vi02", "vi03", "vi04", "vi05", "vi06", "vi07",
		"vi08", "vi09", "vi10", "vi11", "vi12", "vi13", "vi14", "vi15",
		"Status", "MACflag", "ClipFlag", "c2c19", "R", "I", "Q", "c2c23",
		"c2c24", "c2c25", "TPC", "CMSAR0", "FBRST", "VPU-STAT", "c2c30", "CMSAR1"};

	const char* const COP2_VFnames[4] = {"x", "y", "z", "w"};
} // namespace R5900

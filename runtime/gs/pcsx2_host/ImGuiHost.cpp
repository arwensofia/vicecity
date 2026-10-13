// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal ImGuiManager for the PCSX2 GS. The Metal device renders an ImGui draw list on every
// present (OSD in PCSX2), so a context with a frame in flight must always exist. We draw
// nothing into it today; it is the hook for a future debug overlay.

#include "pcsx2_host.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "ImGui/ImGuiManager.h"

#include "imgui.h"

namespace
{
	void UpdateDisplaySize()
	{
		ImGuiIO& io = ImGui::GetIO();
		const float w = g_gs_device ? static_cast<float>(g_gs_device->GetWindowWidth()) : 0.0f;
		const float h = g_gs_device ? static_cast<float>(g_gs_device->GetWindowHeight()) : 0.0f;
		io.DisplaySize = ImVec2(w > 0.0f ? w : 1.0f, h > 0.0f ? h : 1.0f);
	}
} // namespace

bool ImGuiManager::Initialize()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	io.Fonts->AddFontDefault();
	UpdateDisplaySize();
	NewFrame();
	return true;
}

void ImGuiManager::Shutdown(bool)
{
	if (!ImGui::GetCurrentContext())
		return;
	if (g_gs_device)
		g_gs_device->DestroyImGuiTextures();
	ImGui::DestroyContext();
}

void ImGuiManager::WindowResized()
{
	if (ImGui::GetCurrentContext())
		UpdateDisplaySize();
}

void ImGuiManager::RequestScaleUpdate() {}
void ImGuiManager::ReloadFonts() {}

void ImGuiManager::NewFrame()
{
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::NewFrame();
}

void ImGuiManager::SkipFrame()
{
	ImGui::EndFrame();
	NewFrame();
}

void ImGuiManager::RenderOSD() {}

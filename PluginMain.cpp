#include <windows.h>
#include <vulkan/vulkan.h>

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"

static IUnityInterfaces* s_unity = nullptr;
static IUnityGraphics* s_graphics = nullptr;
static IUnityGraphicsVulkan* s_vulkan = nullptr;

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventInitialize)
	{
		if (s_unity)
			s_vulkan = s_unity->Get<IUnityGraphicsVulkan>();
	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
		s_vulkan = nullptr;
	}
}

static void UNITY_INTERFACE_API OnRenderEvent(int eventId)
{
	if (eventId != 1)
		return;

	if (!s_vulkan)
		return;

	UnityVulkanRecordingState state{};
	if (!s_vulkan->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare))
		return;

	VkCommandBuffer cmd = state.commandBuffer;
	if (state.commandBuffer == VK_NULL_HANDLE)
		return;

	// ‚±‚±‚ÉŒã‚Å vkCmdBindPipeline / vkCmdDraw ‚ðŒÄ‚Ô
}

extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventFunc()
{
	return OnRenderEvent;
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
	s_unity = unityInterfaces;
	s_graphics = unityInterfaces->Get<IUnityGraphics>();

	if (s_graphics)
	{
		s_graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
		OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
	}
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
	if (s_graphics)
		s_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);

	s_vulkan = nullptr;
	s_graphics = nullptr;
	s_unity = nullptr;
}
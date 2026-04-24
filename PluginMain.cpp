#include <windows.h>
#include <vector>
#include <fstream>
#include <cassert>
#include <vulkan/vulkan.h>

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"

static IUnityInterfaces* s_unity = nullptr;
static IUnityGraphics* s_graphics = nullptr;
static IUnityGraphicsVulkan* s_vulkan = nullptr;

static VkDevice s_device = VK_NULL_HANDLE;
static VkPipelineLayout s_pipelineLayout = VK_NULL_HANDLE;
static VkPipeline s_pipeline = VK_NULL_HANDLE;
static VkRenderPass s_cachedRenderPass = VK_NULL_HANDLE;
static int s_cachedSubpassIndex = -1;

static int s_width = 1;
static int s_height = 1;

static std::vector<char> ReadBinaryFile(const char* path)
{
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs) return {};
	std::streamsize size = ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	std::vector<char> data((size_t)size);
	if (!ifs.read(data.data(), size))
		return {};
	return data;
}

static VkShaderModule CreateShaderModule(VkDevice device, const char* path)
{
	std::vector<char> bytes = ReadBinaryFile(path);
	if (bytes.empty())
		return VK_NULL_HANDLE;

	VkShaderModuleCreateInfo ci{};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = bytes.size();
	ci.pCode = reinterpret_cast<const uint32_t*>(bytes.data());

	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

static void DestroyPipelineObjects()
{
	if (s_pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(s_device, s_pipeline, nullptr);
		s_pipeline = VK_NULL_HANDLE;
	}

	if (s_pipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(s_device, s_pipelineLayout, nullptr);
		s_pipelineLayout = VK_NULL_HANDLE;
	}

	s_cachedRenderPass = VK_NULL_HANDLE;
}

static bool CreatePipelineForRenderPass(VkDevice device, VkRenderPass renderPass, int subpassIndex)
{
	DestroyPipelineObjects();

	VkShaderModule vert = CreateShaderModule(device, "Assets/Plugins/x86_64/fullscreen_vs.spv");
	VkShaderModule frag = CreateShaderModule(device, "Assets/Plugins/x86_64/fullscreen_fs.spv");
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
	{
		if (vert) vkDestroyShaderModule(device, vert, nullptr);
		if (frag) vkDestroyShaderModule(device, frag, nullptr);
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";

	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vi{};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo ia{};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp{};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs{};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo dss{};
	dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	dss.depthTestEnable = VK_FALSE;
	dss.depthWriteEnable = VK_FALSE;
	dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	dss.depthBoundsTestEnable = VK_FALSE;
	dss.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState cbAttachment{};
	cbAttachment.colorWriteMask = 
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	cbAttachment.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo cb{};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cbAttachment;

	VkDynamicState dynamics[] =
	{
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};

	VkPipelineDynamicStateCreateInfo ds{};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dynamics;

	VkPipelineLayoutCreateInfo pl{};
	pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	if (vkCreatePipelineLayout(device, &pl, nullptr, &s_pipelineLayout) != VK_SUCCESS)
	{
		vkDestroyShaderModule(device, vert, nullptr);
		vkDestroyShaderModule(device, frag, nullptr);
		return false;
	}

	VkGraphicsPipelineCreateInfo gp{};
	gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pDepthStencilState = &dss;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &ds;
	gp.layout = s_pipelineLayout;
	gp.renderPass = renderPass;
	gp.subpass = subpassIndex;

	bool ok = (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &s_pipeline) == VK_SUCCESS);

	vkDestroyShaderModule(device, vert, nullptr);
	vkDestroyShaderModule(device, frag, nullptr);

	if (!ok)
	{
		DestroyPipelineObjects();
		return false;
	}

	s_cachedRenderPass = renderPass;
	return true;
}

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventInitialize)
	{

		if (!s_unity)
			return;

		s_vulkan = s_unity->Get<IUnityGraphicsVulkan>();
		if (!s_vulkan)
			return;

		UnityVulkanInstance instance = s_vulkan->Instance();
		s_device = instance.device;

	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
#if 0
		if (s_device != VK_NULL_HANDLE)
			DestroyPipelineObjects();
#endif
		s_device = VK_NULL_HANDLE;
		s_vulkan = nullptr;
	}
}

static void UNITY_INTERFACE_API OnRenderEvent(int eventId)
{

	if (eventId != 1 || !s_vulkan || s_device == VK_NULL_HANDLE)
		return;

	UnityVulkanRecordingState state{};
	if (!s_vulkan->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare))
		return;

	if (state.commandBuffer == VK_NULL_HANDLE)
		return;

	if (state.renderPass == VK_NULL_HANDLE)
		return;

	if (state.renderPass != s_cachedRenderPass || 
		state.subPassIndex != s_cachedSubpassIndex ||
		s_pipeline == VK_NULL_HANDLE)
	{
		if (!CreatePipelineForRenderPass(s_device, state.renderPass, state.subPassIndex))
			return;
	}

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)s_width;
	viewport.height = (float)s_height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = { (uint32_t)s_width, (uint32_t)s_height };
	
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	vkCmdSetViewport(state.commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(state.commandBuffer, 0, 1, &scissor);
	vkCmdDraw(state.commandBuffer, 3, 1, 0, 0);
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
#if 0
		OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
#endif
	}
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
	if (s_graphics)
		s_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);

	if (s_device != VK_NULL_HANDLE)
		DestroyPipelineObjects();

	s_device = VK_NULL_HANDLE;
	s_vulkan = nullptr;
	s_graphics = nullptr;
	s_unity = nullptr;
}

extern "C" __declspec(dllexport) void SetRenderSize(int width, int height)
{
	s_width = width;
	s_height = height;
}
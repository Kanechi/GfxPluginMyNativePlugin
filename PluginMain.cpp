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

static void* s_sourceTexture = nullptr;
static void* s_bloomTexture = nullptr;

static VkDescriptorSetLayout s_descSetLayout = VK_NULL_HANDLE;
static VkDescriptorPool s_descPool = VK_NULL_HANDLE;
static VkDescriptorSet s_descSet = VK_NULL_HANDLE;
static VkSampler s_sampler = VK_NULL_HANDLE;

static VkImageView s_sourceImageView = VK_NULL_HANDLE;
static VkImage s_lastSourceImage = VK_NULL_HANDLE;

static VkImageView s_bloomImageView = VK_NULL_HANDLE;
static VkImage s_lastBloomImage = VK_NULL_HANDLE;

static VkRenderPass s_offscreenRenderPass = VK_NULL_HANDLE;
static VkFramebuffer s_offscreenFramebuffer = VK_NULL_HANDLE;
static VkPipelineLayout s_offscreenPipelineLayout = VK_NULL_HANDLE;
static VkPipeline s_offscreenPipeline = VK_NULL_HANDLE;

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

static void DestroyDescriptorObjects()
{
	if (s_sampler != VK_NULL_HANDLE) 
	{
		vkDestroySampler(s_device, s_sampler, nullptr);
		s_sampler = VK_NULL_HANDLE;
	}

	if (s_descPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(s_device, s_descPool, nullptr);
		s_descPool = VK_NULL_HANDLE;
	}

	if (s_descSetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(s_device, s_descSetLayout, nullptr);
		s_descSetLayout = VK_NULL_HANDLE;
	}

	s_descSet = VK_NULL_HANDLE;
}

static bool CreateDescriptorObjects()
{
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &binding;

	if (vkCreateDescriptorSetLayout(s_device, &layoutInfo, nullptr, &s_descSetLayout) != VK_SUCCESS)
		return false;

	VkDescriptorPoolSize poolSize{};
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = 1;

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;

	if (vkCreateDescriptorPool(s_device, &poolInfo, nullptr, &s_descPool) != VK_SUCCESS)
		return false;

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = s_descPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &s_descSetLayout;

	if (vkAllocateDescriptorSets(s_device, &allocInfo, &s_descSet) != VK_SUCCESS)
		return false;

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = 1.0f;

	if (vkCreateSampler(s_device, &samplerInfo, nullptr, &s_sampler) != VK_SUCCESS)
		return false;

	return true;
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
	s_cachedSubpassIndex = -1;
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
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &s_descSetLayout;

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
	s_cachedSubpassIndex = subpassIndex;
	return true;
}

static void DestroyOffscreenObjects()
{
	if (s_offscreenFramebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(s_device, s_offscreenFramebuffer, nullptr);
		s_offscreenFramebuffer = VK_NULL_HANDLE;
	}

	if (s_offscreenPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(s_device, s_offscreenPipeline, nullptr);
		s_offscreenPipeline = VK_NULL_HANDLE;
	}

	if (s_offscreenPipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(s_device, s_offscreenPipelineLayout, nullptr);
		s_offscreenPipelineLayout = VK_NULL_HANDLE;
	}

	if (s_offscreenRenderPass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(s_device, s_offscreenRenderPass, nullptr);
		s_offscreenRenderPass = VK_NULL_HANDLE;
	}

	if (s_sourceImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(s_device, s_sourceImageView, nullptr);
		s_sourceImageView = VK_NULL_HANDLE;
	}
	s_lastSourceImage = VK_NULL_HANDLE;

	if (s_bloomImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(s_device, s_bloomImageView, nullptr);
		s_bloomImageView = VK_NULL_HANDLE;
	}
	s_lastBloomImage = VK_NULL_HANDLE;
}

static bool CreateOffscreenRenderPass(VkFormat colorFormat)
{
	if (s_offscreenRenderPass != VK_NULL_HANDLE)
		return true;

	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = colorFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentReference colorRef{};
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;

	VkSubpassDependency dep{};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp.attachmentCount = 1;
	rp.pAttachments = &colorAttachment;
	rp.subpassCount = 1;
	rp.pSubpasses = &subpass;
	rp.dependencyCount = 1;
	rp.pDependencies = &dep;

	return vkCreateRenderPass(s_device, &rp, nullptr, &s_offscreenRenderPass) == VK_SUCCESS;
}

static bool CreateOffscreenFramebuffer(uint32_t width, uint32_t height)
{
	if (s_offscreenFramebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(s_device, s_offscreenFramebuffer, nullptr);
		s_offscreenFramebuffer = VK_NULL_HANDLE;
	}

	VkImageView attachments[] = { s_bloomImageView };

	VkFramebufferCreateInfo fb{};
	fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb.renderPass = s_offscreenRenderPass;
	fb.attachmentCount = 1;
	fb.pAttachments = attachments;
	fb.width = width;
	fb.height = height;
	fb.layers = 1;

	return vkCreateFramebuffer(s_device, &fb, nullptr, &s_offscreenFramebuffer) == VK_SUCCESS;

}

static bool CreateOffscreenPipeline()
{
	if (s_offscreenPipeline != VK_NULL_HANDLE)
		return true;

	VkShaderModule vert = CreateShaderModule(s_device, "Assets/Plugins/x86_64/fullscreen_vs.spv");
	VkShaderModule frag = CreateShaderModule(s_device, "Assets/Plugins/x86_64/luminance_extraction_fs.spv");
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
	{
		if (vert) vkDestroyShaderModule(s_device, vert, nullptr);
		if (frag) vkDestroyShaderModule(s_device, frag, nullptr);
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

	VkPipelineColorBlendAttachmentState cbAttachment{};
	cbAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;
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
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &s_descSetLayout;

	if (vkCreatePipelineLayout(s_device, &pl, nullptr, &s_offscreenPipelineLayout) != VK_SUCCESS)
	{
		vkDestroyShaderModule(s_device, vert, nullptr);
		vkDestroyShaderModule(s_device, frag, nullptr);
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
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &ds;
	gp.layout = s_offscreenPipelineLayout;
	gp.renderPass = s_offscreenRenderPass;
	gp.subpass = 0;

	bool ok = (vkCreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &gp, nullptr, &s_offscreenPipeline) == VK_SUCCESS);

	vkDestroyShaderModule(s_device, vert, nullptr);
	vkDestroyShaderModule(s_device, frag, nullptr);

	if (!ok)
	{
		if (s_offscreenPipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(s_device, s_offscreenPipelineLayout, nullptr);
			s_offscreenPipelineLayout = VK_NULL_HANDLE;
		}
		return false;
	}

	return true;
}

static bool UpdateImageView(
	UnityVulkanImage& image,
	VkImage& lastImage,
	VkImageView& imageView)
{
	if (image.image == VK_NULL_HANDLE)
		return false;

	if (lastImage != image.image || imageView == VK_NULL_HANDLE)
	{
		if (imageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(s_device, imageView, nullptr);
			imageView = VK_NULL_HANDLE;
		}

		VkImageViewCreateInfo iv{};
		iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		iv.image = image.image;
		iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
		iv.format = image.format;
		iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		iv.subresourceRange.baseMipLevel = 0;
		iv.subresourceRange.levelCount = 1;
		iv.subresourceRange.baseArrayLayer = 0;
		iv.subresourceRange.layerCount = 1;

		if (vkCreateImageView(s_device, &iv, nullptr, &imageView) != VK_SUCCESS)
			return false;

		lastImage = image.image;
	}

	return true;
}

// グラフィックスデバイスの初期化イベントと終了イベント
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

		DestroyDescriptorObjects();
		if (!CreateDescriptorObjects()) 
		{
			DestroyDescriptorObjects();
			return;
		}
	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
		DestroyOffscreenObjects();
		DestroyDescriptorObjects();

		s_device = VK_NULL_HANDLE;
		s_vulkan = nullptr;
	}
}

// 描画イベント
static void UNITY_INTERFACE_API OnRenderEvent(int eventId)
{
	if (eventId != 1 || !s_vulkan || s_device == VK_NULL_HANDLE)
		return;

	if (s_sourceTexture == nullptr || s_bloomTexture == nullptr)
		return;

	// まず Unity の現在の render pass の外へ出る
	s_vulkan->EnsureOutsideRenderPass();

	// 外枝出た後の recording state を取り直す
	UnityVulkanRecordingState state{};
	if (!s_vulkan->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare))
		return;

	if (state.commandBuffer == VK_NULL_HANDLE)
		return;

	UnityVulkanImage sceneImage{};
	if (!s_vulkan->AccessTexture(
		s_sourceTexture,
		UnityVulkanWholeImage,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		kUnityVulkanResourceAccess_PipelineBarrier,
		&sceneImage))
	{
		return;
	}

	UnityVulkanImage bloomImage{};
	if (!s_vulkan->AccessTexture(
		s_bloomTexture,
		UnityVulkanWholeImage,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		kUnityVulkanResourceAccess_PipelineBarrier,
		&bloomImage))
	{
		return;
	}

	if (!UpdateImageView(sceneImage, s_lastSourceImage, s_sourceImageView))
		return;

	if (!UpdateImageView(bloomImage, s_lastBloomImage, s_bloomImageView))
		return;

	if (!CreateOffscreenRenderPass(bloomImage.format))
		return;

	if (!CreateOffscreenFramebuffer(bloomImage.extent.width, bloomImage.extent.height))
		return;

	if (!CreateOffscreenPipeline())
		return;

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = s_sampler;
	imageInfo.imageView = s_sourceImageView;
	imageInfo.imageLayout = sceneImage.layout;

	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = s_descSet;
	write.dstBinding = 0;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;

	vkUpdateDescriptorSets(s_device, 1, &write, 0, nullptr);

	VkClearValue clearValue{};
	clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

	VkRenderPassBeginInfo rpBegin{};
	rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpBegin.renderPass = s_offscreenRenderPass;
	rpBegin.framebuffer = s_offscreenFramebuffer;
	rpBegin.renderArea.offset = { 0, 0 };
	rpBegin.renderArea.extent = { bloomImage.extent.width, bloomImage.extent.height };
	rpBegin.clearValueCount = 1;
	rpBegin.pClearValues = &clearValue;

	vkCmdBeginRenderPass(state.commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)bloomImage.extent.width;
	viewport.height = (float)bloomImage.extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = { 0, 0 };
	scissor.extent = { bloomImage.extent.width, bloomImage.extent.height };

	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_offscreenPipeline);

	vkCmdBindDescriptorSets(
		state.commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		s_offscreenPipelineLayout,
		0,
		1,
		&s_descSet,
		0,
		nullptr);

	vkCmdSetViewport(state.commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(state.commandBuffer, 0, 1, &scissor);
	vkCmdDraw(state.commandBuffer, 3, 1, 0, 0);

	vkCmdEndRenderPass(state.commandBuffer);
}

// Unity 側に描画イベントの関数ポインタを渡すための関数
extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventFunc()
{
	return OnRenderEvent;
}

// Unity の Plugin がロードされたときに呼び出される関数
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
	s_unity = unityInterfaces;
	s_graphics = unityInterfaces->Get<IUnityGraphics>();

	if (s_graphics)
	{
		s_graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
	}
}

// Unity の Plugin がアンロードされるときに呼び出される関数
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
	DestroyOffscreenObjects();
	DestroyDescriptorObjects();

	if (s_graphics)
		s_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);

	if (s_device != VK_NULL_HANDLE)
		DestroyPipelineObjects();

	s_device = VK_NULL_HANDLE;
	s_vulkan = nullptr;
	s_graphics = nullptr;
	s_unity = nullptr;
}

// Unity から描画サイズを受け取るための関数
extern "C" __declspec(dllexport) void SetRenderSize(int width, int height)
{
	s_width = width;
	s_height = height;
}

// Unity から描画に使用するテクスチャを受け取るための関数
extern "C" __declspec(dllexport) void SetSourceTexture(void* nativeTex)
{
	s_sourceTexture = nativeTex;
}

// Unity から描画に使用するテクスチャを受け取るための関数
extern "C" __declspec(dllexport) void SetBloomTexture(void* nativeTex)
{
	s_bloomTexture = nativeTex;
}


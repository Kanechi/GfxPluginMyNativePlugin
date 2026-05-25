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

static void* s_blurTexture = nullptr;

static VkImageView s_blurImageView = VK_NULL_HANDLE;
static VkImage s_lastBlurImage = VK_NULL_HANDLE;

static VkRenderPass s_blurRenderPass = VK_NULL_HANDLE;
static VkFramebuffer s_blurFramebuffer = VK_NULL_HANDLE;
static VkPipelineLayout s_blurPipelineLayout = VK_NULL_HANDLE;
static VkPipeline s_blurPipeline = VK_NULL_HANDLE;

static VkDescriptorSetLayout s_compositeDescSetLayout = VK_NULL_HANDLE;
static VkDescriptorPool s_compositeDescPool = VK_NULL_HANDLE;
static VkDescriptorSet s_compositeDescSet = VK_NULL_HANDLE;

static VkPipelineLayout s_compositePipelineLayout = VK_NULL_HANDLE;
static VkPipeline s_compositePipeline = VK_NULL_HANDLE;
static VkRenderPass s_compositeRenderPass = VK_NULL_HANDLE;
static int s_compositeSubpassIndex = -1;

#if 0
static int s_debugStatus = 0;
extern "C" __declspec(dllexport) int GetDebugStatus()
{
	return s_debugStatus;
}
#endif

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
	if (s_blurFramebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(s_device, s_blurFramebuffer, nullptr);
		s_blurFramebuffer = VK_NULL_HANDLE;
	}

	if (s_blurPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(s_device, s_blurPipeline, nullptr);
		s_blurPipeline = VK_NULL_HANDLE;
	}

	if (s_blurPipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(s_device, s_blurPipelineLayout, nullptr);
		s_blurPipelineLayout = VK_NULL_HANDLE;
	}

	if (s_blurRenderPass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(s_device, s_blurRenderPass, nullptr);
		s_blurRenderPass = VK_NULL_HANDLE;
	}

	if (s_blurImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(s_device, s_blurImageView, nullptr);
		s_blurImageView = VK_NULL_HANDLE;
	}
	s_lastBlurImage = VK_NULL_HANDLE;


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

static bool CreateBlurRenderPass(VkFormat colorFormat)
{
	if (s_blurRenderPass != VK_NULL_HANDLE)
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

	VkRenderPassCreateInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp.attachmentCount = 1;
	rp.pAttachments = &colorAttachment;
	rp.subpassCount = 1;
	rp.pSubpasses = &subpass;

	return vkCreateRenderPass(s_device, &rp, nullptr, &s_blurRenderPass) == VK_SUCCESS;
}

static bool CreateBlurFramebuffer(uint32_t width, uint32_t height)
{
	if (s_blurFramebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(s_device, s_blurFramebuffer, nullptr);
		s_blurFramebuffer = VK_NULL_HANDLE;
	}

	VkImageView attachments[] = { s_blurImageView };

	VkFramebufferCreateInfo fb{};
	fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb.renderPass = s_blurRenderPass;
	fb.attachmentCount = 1;
	fb.pAttachments = attachments;
	fb.width = width;
	fb.height = height;
	fb.layers = 1;

	return vkCreateFramebuffer(s_device, &fb, nullptr, &s_blurFramebuffer) == VK_SUCCESS;
}

static bool CreateBlurPipeline()
{
	if (s_blurPipeline != VK_NULL_HANDLE)
		return true;

	VkShaderModule vert = CreateShaderModule(s_device, "Assets/Plugins/x86_64/fullscreen_vs.spv");
	VkShaderModule frag = CreateShaderModule(s_device, "Assets/Plugins/x86_64/blur_h_fs.spv");
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

	if (vkCreatePipelineLayout(s_device, &pl, nullptr, &s_blurPipelineLayout) != VK_SUCCESS)
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
	gp.layout = s_blurPipelineLayout;
	gp.renderPass = s_blurRenderPass;
	gp.subpass = 0;

	bool ok = (vkCreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &gp, nullptr, &s_blurPipeline) == VK_SUCCESS);

	vkDestroyShaderModule(s_device, vert, nullptr);
	vkDestroyShaderModule(s_device, frag, nullptr);

	if (!ok)
	{
		if (s_blurPipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(s_device, s_blurPipelineLayout, nullptr);
			s_blurPipelineLayout = VK_NULL_HANDLE;
		}
		return false;
	}

	return true;
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

static void DestroyCompositeDescriptorObjects()
{
	if (s_compositeDescPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(s_device, s_compositeDescPool, nullptr);
		s_compositeDescPool = VK_NULL_HANDLE;
	}

	if (s_compositeDescSetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(s_device, s_compositeDescSetLayout, nullptr);
		s_compositeDescSetLayout = VK_NULL_HANDLE;
	}

	s_compositeDescSet = VK_NULL_HANDLE;
}

static bool CreateCompositeDescriptorObjects()
{
	VkDescriptorSetLayoutBinding bindings[2]{};

	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = bindings;

	if (vkCreateDescriptorSetLayout(s_device, &layoutInfo, nullptr, &s_compositeDescSetLayout) != VK_SUCCESS)
		return false;

	VkDescriptorPoolSize poolSize{};
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = 2;

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;

	if (vkCreateDescriptorPool(s_device, &poolInfo, nullptr, &s_compositeDescPool) != VK_SUCCESS)
		return false;

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = s_compositeDescPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &s_compositeDescSetLayout;

	if (vkAllocateDescriptorSets(s_device, &allocInfo, &s_compositeDescSet) != VK_SUCCESS)
		return false;

	return true;
}

static void DestroyCompositePipelineObjects()
{
	if (s_compositePipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(s_device, s_compositePipeline, nullptr);
		s_compositePipeline = VK_NULL_HANDLE;
	}

	if (s_compositePipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(s_device, s_compositePipelineLayout, nullptr);
		s_compositePipelineLayout = VK_NULL_HANDLE;
	}

	s_compositeRenderPass = VK_NULL_HANDLE;
	s_compositeSubpassIndex = -1;
}

static bool CreateCompositePipelineForRenderPass(VkDevice device, VkRenderPass renderPass, int subpassIndex)
{
	DestroyCompositePipelineObjects();

	VkShaderModule vert = CreateShaderModule(device, "Assets/Plugins/x86_64/fullscreen_vs.spv");
	VkShaderModule frag = CreateShaderModule(device, "Assets/Plugins/x86_64/composite_fs.spv");
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
	pl.pSetLayouts = &s_compositeDescSetLayout;

	if (vkCreatePipelineLayout(device, &pl, nullptr, &s_compositePipelineLayout) != VK_SUCCESS)
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
	gp.layout = s_compositePipelineLayout;
	gp.renderPass = renderPass;
	gp.subpass = subpassIndex;

	bool ok = (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &s_compositePipeline) == VK_SUCCESS);

	vkDestroyShaderModule(device, vert, nullptr);
	vkDestroyShaderModule(device, frag, nullptr);

	if (!ok)
	{
		DestroyCompositePipelineObjects();
		return false;
	}

	s_compositeRenderPass = renderPass;
	s_compositeSubpassIndex = subpassIndex;
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

		DestroyCompositeDescriptorObjects();
		if (!CreateCompositeDescriptorObjects())
		{
			DestroyDescriptorObjects();
			return;
		}
	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
		DestroyCompositePipelineObjects();
		DestroyCompositeDescriptorObjects();

		DestroyOffscreenObjects();
		DestroyDescriptorObjects();

		s_device = VK_NULL_HANDLE;
		s_vulkan = nullptr;
	}
}

// 描画イベント
static void UNITY_INTERFACE_API OnRenderEvent_Offscreen(int eventId)
{
	if (eventId != 1 || !s_vulkan || s_device == VK_NULL_HANDLE)
		return;

	if (s_sourceTexture == nullptr || s_bloomTexture == nullptr || s_blurTexture == nullptr)
		return;

	// render pass の外へ出る
	s_vulkan->EnsureOutsideRenderPass();

	// 外へ出た後の state
	UnityVulkanRecordingState offscreenState{};
	if (!s_vulkan->CommandRecordingState(&offscreenState, kUnityVulkanGraphicsQueueAccess_DontCare))
		return;

	if (offscreenState.commandBuffer == VK_NULL_HANDLE)
		return;

	UnityVulkanImage sourceImage{};
	if (!s_vulkan->AccessTexture(
		s_sourceTexture,
		UnityVulkanWholeImage,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		kUnityVulkanResourceAccess_PipelineBarrier,
		&sourceImage))
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

	UnityVulkanImage blurImage{};
	if (!s_vulkan->AccessTexture(
		s_blurTexture,
		UnityVulkanWholeImage,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		kUnityVulkanResourceAccess_PipelineBarrier,
		&blurImage))
	{
		return;
	}

	if (!UpdateImageView(sourceImage, s_lastSourceImage, s_sourceImageView))
		return;

	if (!UpdateImageView(bloomImage, s_lastBloomImage, s_bloomImageView))
		return;

	if (!UpdateImageView(blurImage, s_lastBlurImage, s_blurImageView))
		return;

	// ----------------------------------------
	// 高輝度抽出: SourceRT -> BloomRT
	// ----------------------------------------
	if (!CreateOffscreenRenderPass(bloomImage.format))
		return;

	if (!CreateOffscreenFramebuffer(bloomImage.extent.width, bloomImage.extent.height))
		return;

	if (!CreateOffscreenPipeline())
		return;

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = s_sampler;
	imageInfo.imageView = s_sourceImageView;
	imageInfo.imageLayout = sourceImage.layout;

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

	vkCmdBeginRenderPass(offscreenState.commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

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

	vkCmdBindPipeline(offscreenState.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_offscreenPipeline);

	vkCmdBindDescriptorSets(
		offscreenState.commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		s_offscreenPipelineLayout,
		0,
		1,
		&s_descSet,
		0,
		nullptr);

	vkCmdSetViewport(offscreenState.commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(offscreenState.commandBuffer, 0, 1, &scissor);
	vkCmdDraw(offscreenState.commandBuffer, 3, 1, 0, 0);

	vkCmdEndRenderPass(offscreenState.commandBuffer);

	// ----------------------------------------
	// 横ブラー: BloomRT -> BlurRT
	// ----------------------------------------
	if (!CreateBlurRenderPass(blurImage.format))
		return;

	if (!CreateBlurFramebuffer(blurImage.extent.width, blurImage.extent.height))
		return;

	if (!CreateBlurPipeline())
		return;

	VkDescriptorImageInfo blurInputInfo{};
	blurInputInfo.sampler = s_sampler;
	blurInputInfo.imageView = s_bloomImageView;
	blurInputInfo.imageLayout = bloomImage.layout;

	VkWriteDescriptorSet blurWrite{};
	blurWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrite.dstSet = s_descSet;
	blurWrite.dstBinding = 0;
	blurWrite.dstArrayElement = 0;
	blurWrite.descriptorCount = 1;
	blurWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	blurWrite.pImageInfo = &blurInputInfo;

	vkUpdateDescriptorSets(s_device, 1, &blurWrite, 0, nullptr);

	VkClearValue blurClear{};
	blurClear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

	VkRenderPassBeginInfo blurRpBegin{};
	blurRpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	blurRpBegin.renderPass = s_blurRenderPass;
	blurRpBegin.framebuffer = s_blurFramebuffer;
	blurRpBegin.renderArea.offset = { 0, 0 };
	blurRpBegin.renderArea.extent = { blurImage.extent.width, blurImage.extent.height };
	blurRpBegin.clearValueCount = 1;
	blurRpBegin.pClearValues = &blurClear;

	vkCmdBeginRenderPass(offscreenState.commandBuffer, &blurRpBegin, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport blurViewport{};
	blurViewport.x = 0.0f;
	blurViewport.y = 0.0f;
	blurViewport.width = (float)blurImage.extent.width;
	blurViewport.height = (float)blurImage.extent.height;
	blurViewport.minDepth = 0.0f;
	blurViewport.maxDepth = 1.0f;

	VkRect2D blurScissor{};
	blurScissor.offset = { 0, 0 };
	blurScissor.extent = { blurImage.extent.width, blurImage.extent.height };

	vkCmdBindPipeline(offscreenState.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_blurPipeline);

	vkCmdBindDescriptorSets(
		offscreenState.commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		s_blurPipelineLayout,
		0,
		1,
		&s_descSet,
		0,
		nullptr);

	vkCmdSetViewport(offscreenState.commandBuffer, 0, 1, &blurViewport);
	vkCmdSetScissor(offscreenState.commandBuffer, 0, 1, &blurScissor);
	vkCmdDraw(offscreenState.commandBuffer, 3, 1, 0, 0);

	vkCmdEndRenderPass(offscreenState.commandBuffer);
}

static void UNITY_INTERFACE_API OnRenderEvent_Composite(int eventId)
{
	if (eventId != 2 || !s_vulkan || s_device == VK_NULL_HANDLE) {
		return;
	}

	if (s_sourceTexture == nullptr || s_blurTexture == nullptr) {
		return;
	}

	// ここでは EnsureOutsideRenderPass を呼ばない
	UnityVulkanRecordingState mainState{};
	if (!s_vulkan->CommandRecordingState(&mainState, kUnityVulkanGraphicsQueueAccess_DontCare)) {
		return;
	}

	if (mainState.commandBuffer == VK_NULL_HANDLE) {
		return;
	}

	if (mainState.renderPass == VK_NULL_HANDLE) {
		return;
	}

	if (mainState.subPassIndex < 0) {
		return;
	}

	UnityVulkanImage sourceImage{};
	if (!s_vulkan->AccessTexture(
		s_sourceTexture,
		UnityVulkanWholeImage,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		kUnityVulkanResourceAccess_PipelineBarrier,
		&sourceImage))
	{
		return;
	}

	UnityVulkanImage blurImage{};
	if (!s_vulkan->AccessTexture(
		s_blurTexture,
		UnityVulkanWholeImage,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		kUnityVulkanResourceAccess_PipelineBarrier,
		&blurImage))
	{
		return;
	}

	if (!UpdateImageView(sourceImage, s_lastSourceImage, s_sourceImageView)) {
		return;
	}

	if (!UpdateImageView(blurImage, s_lastBlurImage, s_blurImageView)) {
		return;
	}

	VkDescriptorImageInfo compositeInfos[2]{};

	compositeInfos[0].sampler = s_sampler;
	compositeInfos[0].imageView = s_sourceImageView;
	compositeInfos[0].imageLayout = sourceImage.layout;

	compositeInfos[1].sampler = s_sampler;
	compositeInfos[1].imageView = s_blurImageView;
	compositeInfos[1].imageLayout = blurImage.layout;

	VkWriteDescriptorSet compositeWrites[2]{};

	compositeWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	compositeWrites[0].dstSet = s_compositeDescSet;
	compositeWrites[0].dstBinding = 0;
	compositeWrites[0].dstArrayElement = 0;
	compositeWrites[0].descriptorCount = 1;
	compositeWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	compositeWrites[0].pImageInfo = &compositeInfos[0];

	compositeWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	compositeWrites[1].dstSet = s_compositeDescSet;
	compositeWrites[1].dstBinding = 1;
	compositeWrites[1].dstArrayElement = 0;
	compositeWrites[1].descriptorCount = 1;
	compositeWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	compositeWrites[1].pImageInfo = &compositeInfos[1];

	vkUpdateDescriptorSets(s_device, 2, compositeWrites, 0, nullptr);

	if (mainState.renderPass != s_compositeRenderPass ||
		mainState.subPassIndex != s_compositeSubpassIndex ||
		s_compositePipeline == VK_NULL_HANDLE)
	{
		if (!CreateCompositePipelineForRenderPass(s_device, mainState.renderPass, mainState.subPassIndex)) 
		{
			return;
		}
	}

	VkViewport compositeViewport{};
	compositeViewport.x = 0.0f;
	compositeViewport.y = 0.0f;
	compositeViewport.width = (float)s_width;
	compositeViewport.height = (float)s_height;
	compositeViewport.minDepth = 0.0f;
	compositeViewport.maxDepth = 1.0f;

	VkRect2D compositeScissor{};
	compositeScissor.offset = { 0, 0 };
	compositeScissor.extent = { (uint32_t)s_width, (uint32_t)s_height };

	vkCmdBindPipeline(mainState.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_compositePipeline);

	vkCmdBindDescriptorSets(
		mainState.commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		s_compositePipelineLayout,
		0,
		1,
		&s_compositeDescSet,
		0,
		nullptr);

	vkCmdSetViewport(mainState.commandBuffer, 0, 1, &compositeViewport);
	vkCmdSetScissor(mainState.commandBuffer, 0, 1, &compositeScissor);
	vkCmdDraw(mainState.commandBuffer, 3, 1, 0, 0);
}

// Unity 側に描画イベントの関数ポインタを渡すための関数
extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetOffscreenRenderEventFunc()
{
	return OnRenderEvent_Offscreen;
}

extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetCompositeRenderEventFunc()
{
	return OnRenderEvent_Composite;
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
	DestroyCompositePipelineObjects();
	DestroyCompositeDescriptorObjects();

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

extern "C" __declspec(dllexport) void SetBlurTexture(void* nativeTex)
{
	s_blurTexture = nativeTex;
}
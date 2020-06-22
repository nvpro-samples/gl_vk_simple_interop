/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#pragma once

#include "gl_vkpp.hpp"
#include "nvh/fileoperations.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/shaders_vk.hpp"

extern std::vector<std::string> defaultSearchPaths;

class ComputeImageVk
{

public:
  ComputeImageVk() = default;
  void setup(const vk::Device& device, const vk::PhysicalDevice& physicalDevice, uint32_t queueIdxGraphic, uint32_t queueIdxCompute)
  {
    m_device          = device;
    m_physicalDevice  = physicalDevice;
    m_queueIdxGraphic = queueIdxGraphic;
    m_queueIdxCompute = queueIdxCompute;
    m_pipelineCache   = device.createPipelineCache(vk::PipelineCacheCreateInfo());

    m_alloc.init(device, physicalDevice);
  }

  vk::Device              m_device;
  vk::Queue               m_queue;
  vk::PipelineCache       m_pipelineCache;
  vk::CommandPool         m_commandPool;
  nvvkpp::Texture2DVkGL   m_textureTarget;
  vk::DescriptorPool      m_descriptorPool;
  vk::PipelineLayout      m_pipelineLayout;
  vk::DescriptorSetLayout m_descriptorSetLayout;
  vk::DescriptorSet       m_descriptorSet;
  vk::Pipeline            m_pipeline;
  vk::CommandBuffer       m_commandBuffer;
  uint32_t                m_queueIdxGraphic;
  uint32_t                m_queueIdxCompute;
  vk::PhysicalDevice      m_physicalDevice;

  nvvk::AllocatorVkExport m_alloc;

  struct Semaphores
  {
    vk::Semaphore vkReady;
    vk::Semaphore vkComplete;
    GLuint        glReady;
    GLuint        glComplete;
  } m_semaphores;


  void destroy()
  {
    m_queue.waitIdle();
    m_textureTarget.destroy(m_alloc);
    m_device.freeCommandBuffers(m_commandPool, m_commandBuffer);
    m_device.destroyDescriptorPool(m_descriptorPool);
    m_device.destroySemaphore(m_semaphores.vkReady);
    m_device.destroySemaphore(m_semaphores.vkComplete);
    m_device.destroyPipelineCache(m_pipelineCache);

    // Clean up used Vulkan resources
    m_device.destroyPipelineLayout(m_pipelineLayout);
    m_device.destroyDescriptorSetLayout(m_descriptorSetLayout);
    m_device.destroyPipeline(m_pipeline);
    m_device.destroy(m_commandPool);
  }

  void prepare()
  {
    // Create a compute capable device queue
    m_queue = m_device.getQueue(m_queueIdxCompute, 0);
    // Separate command pool as queue family for compute may be different than graphics
    m_commandPool = m_device.createCommandPool({vk::CommandPoolCreateFlagBits::eResetCommandBuffer, m_queueIdxCompute});

    createSemaphores();

    m_textureTarget = prepareTextureTarget(vk::ImageLayout::eGeneral, {512, 512, 1}, vk::Format::eR8G8B8A8Unorm);

    prepareDescriptors();
    preparePipelines();
  }

  void createSemaphores()
  {
    glGenSemaphoresEXT(1, &m_semaphores.glReady);
    glGenSemaphoresEXT(1, &m_semaphores.glComplete);

    // Create semaphores
#ifdef WIN32
    auto handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32;
#else
    const auto handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
#endif

    vk::ExportSemaphoreCreateInfo esci{handleType};
    vk::SemaphoreCreateInfo       sci;
    sci.pNext               = &esci;
    m_semaphores.vkReady    = m_device.createSemaphore(sci);
    m_semaphores.vkComplete = m_device.createSemaphore(sci);

    // Import semaphores
#ifdef WIN32
    {
      HANDLE hglReady    = m_device.getSemaphoreWin32HandleKHR({m_semaphores.vkReady, handleType});
      HANDLE hglComplete = m_device.getSemaphoreWin32HandleKHR({m_semaphores.vkComplete, handleType});
      glImportSemaphoreWin32HandleEXT(m_semaphores.glReady, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglReady);
      glImportSemaphoreWin32HandleEXT(m_semaphores.glComplete, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglComplete);
    }
#else
    {
      auto fdReady    = m_device.getSemaphoreFdKHR({m_semaphores.vkReady, handleType});
      auto fdComplete = m_device.getSemaphoreFdKHR({m_semaphores.vkComplete, handleType});
      glImportSemaphoreFdEXT(m_semaphores.glReady, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdReady);
      glImportSemaphoreFdEXT(m_semaphores.glComplete, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdComplete);
    }
#endif
  }

  void prepareDescriptors()
  {
    std::vector<vk::DescriptorPoolSize> poolSizes{
        // Compute pipelines uses storage images for writing
        {vk::DescriptorType::eStorageImage, 1},
    };
    m_descriptorPool = m_device.createDescriptorPool({{}, 3, (uint32_t)poolSizes.size(), poolSizes.data()});

    // Create compute pipeline separately from graphics pipelines even if they use the same queue
    std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings{
        // Binding 0 : Sampled image (write)
        {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    };

    m_descriptorSetLayout =
        m_device.createDescriptorSetLayout({{}, (uint32_t)setLayoutBindings.size(), setLayoutBindings.data()});

    vk::DescriptorSetAllocateInfo allocInfo{m_descriptorPool, 1, &m_descriptorSetLayout};

    m_descriptorSet = m_device.allocateDescriptorSets(allocInfo)[0];

    std::vector<vk::DescriptorImageInfo> computeTexDescriptors{
        {{}, m_textureTarget.texVk.descriptor.imageView, vk::ImageLayout::eGeneral},
    };

    std::vector<vk::WriteDescriptorSet> computeWriteDescriptorSets{
        // Binding 0 : Sampled image (write)
        {m_descriptorSet, 0, 0, 1, vk::DescriptorType::eStorageImage, &computeTexDescriptors[0]},
    };

    m_device.updateDescriptorSets(computeWriteDescriptorSets, nullptr);
  }

  void preparePipelines()
  {
    // Create compute shader pipelines
    vk::PushConstantRange        push_constants = {vk::ShaderStageFlagBits::eCompute, 0, 1 * sizeof(float)};
    vk::PipelineLayoutCreateInfo layout_info;
    layout_info.setLayoutCount         = 1;
    layout_info.pSetLayouts            = &m_descriptorSetLayout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push_constants;
    m_pipelineLayout                   = m_device.createPipelineLayout(layout_info);
    vk::ComputePipelineCreateInfo computePipelineCreateInfo{{}, {}, m_pipelineLayout};

    std::vector<std::string> paths = defaultSearchPaths;
    auto                     code  = nvh::loadFile("shaders/shader.comp.spv", true, paths);

    computePipelineCreateInfo.stage = nvvk::createShaderStageInfo(m_device, code, VK_SHADER_STAGE_COMPUTE_BIT);
    m_pipeline = static_cast<const vk::Pipeline&>(m_device.createComputePipeline(m_pipelineCache, computePipelineCreateInfo, nullptr));
    m_device.destroyShaderModule(computePipelineCreateInfo.stage.module);

    m_commandBuffer = m_device.allocateCommandBuffers({m_commandPool, vk::CommandBufferLevel::ePrimary, 1})[0];
    buildCommandBuffers();
  }

  void buildCommandBuffers()
  {
    static auto tStart = std::chrono::high_resolution_clock::now();
    auto        tEnd   = std::chrono::high_resolution_clock::now();
    auto        tDiff  = std::chrono::duration<float, std::milli>(tEnd - tStart).count() / 1000.f;

    m_commandBuffer.begin({vk::CommandBufferUsageFlagBits::eSimultaneousUse});
    m_commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipelineLayout, 0, m_descriptorSet, nullptr);
    m_commandBuffer.pushConstants<float>(m_pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, tDiff);

    m_commandBuffer.dispatch(m_textureTarget.imgSize.width / 16, m_textureTarget.imgSize.height / 16, 1);
    m_commandBuffer.end();
  }


  nvvkpp::Texture2DVkGL prepareTextureTarget(vk::ImageLayout targetLayout, const vk::Extent3D& extent, vk::Format format)
  {
    vk::FormatProperties formatProperties;

    // Get device properties for the requested texture format
    formatProperties = m_physicalDevice.getFormatProperties(format);
    // Check if requested image format supports image storage operations
    assert(formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage);

    vk::ImageCreateInfo imageCreateInfo;
    imageCreateInfo.imageType   = vk::ImageType::e2D;
    imageCreateInfo.format      = format;
    imageCreateInfo.extent      = extent;
    imageCreateInfo.mipLevels   = 1;
    imageCreateInfo.arrayLayers = 1;
    // vk::Image will be sampled in the fragment shader and used as storage target in the compute shader
    imageCreateInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;

    nvvkpp::Texture2DVkGL texture;

    nvvk::ImageDedicated    image  = m_alloc.createImage(imageCreateInfo);
    vk::ImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);

    // Create the texture from the image and adding a default sampler
    texture.texVk   = m_alloc.createTexture(image, ivInfo, vk::SamplerCreateInfo());
    texture.imgSize = vk::Extent2D(extent.width, extent.height);

    {
      // Converting the image to the desired layout
      nvvk::ScopeCommandBuffer cmdBuf(m_device, m_queueIdxGraphic);
      nvvk::cmdBarrierImageLayout(cmdBuf, texture.texVk.image, vk::ImageLayout::eUndefined, targetLayout);
    }


    return texture;
  }

  void submit()
  {
    static const std::vector<vk::PipelineStageFlags> waitStages{vk::PipelineStageFlagBits::eComputeShader};
    // Submit compute commands
    vk::SubmitInfo computeSubmitInfo;
    computeSubmitInfo.commandBufferCount   = 1;
    computeSubmitInfo.pCommandBuffers      = &m_commandBuffer;
    computeSubmitInfo.waitSemaphoreCount   = 1;
    computeSubmitInfo.pWaitSemaphores      = &m_semaphores.vkReady;
    computeSubmitInfo.pWaitDstStageMask    = waitStages.data();
    computeSubmitInfo.signalSemaphoreCount = 1;
    computeSubmitInfo.pSignalSemaphores    = &m_semaphores.vkComplete;
    m_queue.submit(computeSubmitInfo, {});
  }
};

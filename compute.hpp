/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2019-2024 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


#pragma once

#include <chrono>
#include "gl_vk.hpp"
#include "nvh/fileoperations.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/resourceallocator_vk.hpp"
#include "nvvk/shaders_vk.hpp"

extern std::vector<std::string> defaultSearchPaths;

static const VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;

class ComputeImageVk
{

public:
  ComputeImageVk() = default;
  void setup(const VkDevice&                         device,
             const VkPhysicalDevice&                 physicalDevice,
             uint32_t                                queueIdxGraphic,
             uint32_t                                queueIdxCompute,
             nvvk::ExportResourceAllocatorDedicated& alloc)
  {
    m_device          = device;
    m_physicalDevice  = physicalDevice;
    m_queueIdxGraphic = queueIdxGraphic;
    m_queueIdxCompute = queueIdxCompute;
    VkPipelineCacheCreateInfo pipelineCacheInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    NVVK_CHECK(vkCreatePipelineCache(device, &pipelineCacheInfo, nullptr, &m_pipelineCache));

    VkFenceCreateInfo finfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    NVVK_CHECK(vkCreateFence(device, &finfo, nullptr, &m_fence));

    // Create a compute capable device queue
    vkGetDeviceQueue(m_device, m_queueIdxCompute, 0, &m_queue);
    // Separate command pool as queue family for compute may be different than graphics
    VkCommandPoolCreateInfo commandPoolInfo{.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = m_queueIdxCompute};
    NVVK_CHECK(vkCreateCommandPool(m_device, &commandPoolInfo, nullptr, &m_commandPool));

    createSemaphores();
    createDescriptors();
    createPipelines();

    m_alloc = &alloc;
  }

  VkDevice                                m_device{};
  VkQueue                                 m_queue{};
  VkPipelineCache                         m_pipelineCache{};
  VkCommandPool                           m_commandPool{};
  nvvk::Texture2DVkGL                     m_textureTarget;
  VkDescriptorPool                        m_descriptorPool{};
  VkPipelineLayout                        m_pipelineLayout{};
  VkDescriptorSetLayout                   m_descriptorSetLayout{};
  VkDescriptorSet                         m_descriptorSet{};
  VkPipeline                              m_pipeline{};
  VkCommandBuffer                         m_commandBuffer{};
  uint32_t                                m_queueIdxGraphic{};
  uint32_t                                m_queueIdxCompute{};
  VkPhysicalDevice                        m_physicalDevice{};
  VkFence                                 m_fence{};
  nvvk::ExportResourceAllocatorDedicated* m_alloc = nullptr;

  struct Semaphores
  {
    VkSemaphore vkReady;
    VkSemaphore vkComplete;
    GLuint      glReady;
    GLuint      glComplete;
  } m_semaphores{};

  void destroy()
  {
    vkQueueWaitIdle(m_queue);
    m_textureTarget.destroy(*m_alloc);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffer);
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    vkDestroySemaphore(m_device, m_semaphores.vkReady, nullptr);
    vkDestroySemaphore(m_device, m_semaphores.vkComplete, nullptr);
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    vkDestroyFence(m_device, m_fence, nullptr);

    // Clean up used Vulkan resources
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
  }

  void update(VkExtent2D extent)
  {
    m_textureTarget.destroy(*m_alloc);
    m_textureTarget = prepareTextureTarget(VK_IMAGE_LAYOUT_GENERAL, extent, kTextureFormat);
    createTextureGL(*m_alloc, m_textureTarget, GL_RGBA8, GL_LINEAR, GL_LINEAR, GL_REPEAT);

    updateDescriptors();
  }

  void createSemaphores()
  {
    glGenSemaphoresEXT(1, &m_semaphores.glReady);
    glGenSemaphoresEXT(1, &m_semaphores.glComplete);

    // Create semaphores
#ifdef WIN32
    const auto handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    const auto handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkExportSemaphoreCreateInfo esci{.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = handleType};
    VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci};
    NVVK_CHECK(vkCreateSemaphore(m_device, &sci, nullptr, &m_semaphores.vkReady));
    NVVK_CHECK(vkCreateSemaphore(m_device, &sci, nullptr, &m_semaphores.vkComplete));

    // Import semaphores
#ifdef WIN32
    {
      HANDLE                           hglReady{};
      VkSemaphoreGetWin32HandleInfoKHR handleInfo{.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
                                                  .semaphore  = m_semaphores.vkReady,
                                                  .handleType = handleType};
      NVVK_CHECK(vkGetSemaphoreWin32HandleKHR(m_device, &handleInfo, &hglReady));

      HANDLE hglComplete{};
      handleInfo.semaphore = m_semaphores.vkComplete;
      NVVK_CHECK(vkGetSemaphoreWin32HandleKHR(m_device, &handleInfo, &hglComplete));

      glImportSemaphoreWin32HandleEXT(m_semaphores.glReady, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglReady);
      glImportSemaphoreWin32HandleEXT(m_semaphores.glComplete, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglComplete);
    }
#else
    {
      int                     fdReady{};
      VkSemaphoreGetFdInfoKHR handleInfo{.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                         .semaphore  = m_semaphores.vkReady,
                                         .handleType = handleType};
      NVVK_CHECK(vkGetSemaphoreFdKHR(m_device, &handleInfo, &fdReady));

      int fdComplete{};
      handleInfo.semaphore = m_semaphores.vkComplete;
      NVVK_CHECK(vkGetSemaphoreFdKHR(m_device, &handleInfo, &fdComplete));

      glImportSemaphoreFdEXT(m_semaphores.glReady, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdReady);
      glImportSemaphoreFdEXT(m_semaphores.glComplete, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdComplete);
    }
#endif
  }

  void createDescriptors()
  {
    std::vector<VkDescriptorPoolSize> poolSizes{
        // Compute pipelines uses storage images for writing
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1},
    };
    VkDescriptorPoolCreateInfo descriptorPoolInfo{.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                  .maxSets       = 3,
                                                  .poolSizeCount = uint32_t(poolSizes.size()),
                                                  .pPoolSizes    = poolSizes.data()};
    NVVK_CHECK(vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr, &m_descriptorPool));

    // Create compute pipeline separately from graphics pipelines even if they use the same queue
    std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
        // Binding 0 : Sampled image (write)
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                            .bindingCount = uint32_t(setLayoutBindings.size()),
                                                            .pBindings    = setLayoutBindings.data()};
    NVVK_CHECK(vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr, &m_descriptorSetLayout));

    VkDescriptorSetAllocateInfo allocInfo{.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool     = m_descriptorPool,
                                          .descriptorSetCount = 1,
                                          .pSetLayouts        = &m_descriptorSetLayout};
    NVVK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet));
  }

  void updateDescriptors()
  {
    VkDescriptorImageInfo computeTexDescriptor{.imageView   = m_textureTarget.texVk.descriptor.imageView,
                                               .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

    // Binding 0 : Sampled image (write)
    VkWriteDescriptorSet computeWriteDescriptorSet{
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = m_descriptorSet,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo      = &computeTexDescriptor,
    };
    vkUpdateDescriptorSets(m_device, 1, &computeWriteDescriptorSet, 0, nullptr);
  }

  void createPipelines()
  {
    // Create compute shader pipelines
    VkPushConstantRange        pushConstants{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 1 * sizeof(float)};
    VkPipelineLayoutCreateInfo layoutInfo{.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount         = 1,
                                          .pSetLayouts            = &m_descriptorSetLayout,
                                          .pushConstantRangeCount = 1,
                                          .pPushConstantRanges    = &pushConstants};
    NVVK_CHECK(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout));

    auto                        code = nvh::loadFile("shaders/shader.comp.spv", true, defaultSearchPaths);
    VkComputePipelineCreateInfo computePipelineInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                    .stage = nvvk::createShaderStageInfo(m_device, code, VK_SHADER_STAGE_COMPUTE_BIT),
                                                    .layout = m_pipelineLayout};
    NVVK_CHECK(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &computePipelineInfo, nullptr, &m_pipeline));
    vkDestroyShaderModule(m_device, computePipelineInfo.stage.module, nullptr);

    VkCommandBufferAllocateInfo commandBufferInfo{.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool        = m_commandPool,
                                                  .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    NVVK_CHECK(vkAllocateCommandBuffers(m_device, &commandBufferInfo, &m_commandBuffer));
  }

  void buildCommandBuffers()
  {
    static auto tStart = std::chrono::high_resolution_clock::now();
    auto        tEnd   = std::chrono::high_resolution_clock::now();
    auto        tDiff  = std::chrono::duration<float>(tEnd - tStart).count();

    NVVK_CHECK(vkWaitForFences(m_device, 1, &m_fence, true, UINT64_MAX));
    NVVK_CHECK(vkResetFences(m_device, 1, &m_fence));

    VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    NVVK_CHECK(vkBeginCommandBuffer(m_commandBuffer, &beginInfo));
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    vkCmdPushConstants(m_commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float), &tDiff);

    vkCmdDispatch(m_commandBuffer, (m_textureTarget.imgSize.width + 15) / 16, (m_textureTarget.imgSize.height + 15) / 16, 1);
    NVVK_CHECK(vkEndCommandBuffer(m_commandBuffer));
  }


  nvvk::Texture2DVkGL prepareTextureTarget(VkImageLayout targetLayout, const VkExtent2D& extent, VkFormat format)
  {
    // Get device properties for the requested texture format
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
    // Check if requested image format supports image storage operations
    assert(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);

    VkImageCreateInfo imageCreateInfo{.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType   = VK_IMAGE_TYPE_2D,
                                      .format      = format,
                                      .extent      = VkExtent3D{extent.width, extent.height, 1},
                                      .mipLevels   = 1,
                                      .arrayLayers = 1,
                                      .samples     = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling      = VK_IMAGE_TILING_OPTIMAL,
                                      // VkImage will be sampled in the fragment shader and used as storage target in the compute shader
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};

    // Create the texture from the image and add a default sampler
    nvvk::Image           image  = m_alloc->createImage(imageCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
    nvvk::Texture2DVkGL   texture;
    texture.texVk = m_alloc->createTexture(image, ivInfo, {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}), texture.imgSize = extent;

    {
      // Convert the image to the desired layout
      nvvk::ScopeCommandBuffer cmdBuf(m_device, m_queueIdxGraphic);
      nvvk::cmdBarrierImageLayout(cmdBuf, texture.texVk.image, VK_IMAGE_LAYOUT_UNDEFINED, targetLayout);
    }

    return texture;
  }

  void submit()
  {
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    // Submit compute commands
    VkSubmitInfo computeSubmitInfo{.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                   .waitSemaphoreCount   = 1,
                                   .pWaitSemaphores      = &m_semaphores.vkReady,
                                   .pWaitDstStageMask    = &waitStage,
                                   .commandBufferCount   = 1,
                                   .pCommandBuffers      = &m_commandBuffer,
                                   .signalSemaphoreCount = 1,
                                   .pSignalSemaphores    = &m_semaphores.vkComplete};
    NVVK_CHECK(vkQueueSubmit(m_queue, 1, &computeSubmitInfo, m_fence));
    NVVK_CHECK(vkQueueWaitIdle(m_queue));
  }
};

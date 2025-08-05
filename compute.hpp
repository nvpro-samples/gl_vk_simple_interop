/*
 * Copyright (c) 2023-2025, NVIDIA CORPORATION.  All rights reserved.
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
 * SPDX-FileCopyrightText: Copyright (c) 2023-2025, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */


#pragma once

#include <chrono>
#include <vulkan/vulkan_core.h>

#include <nvutils/file_operations.hpp>
#include <nvvk/command_pools.hpp>
#include <nvvk/commands.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/resource_allocator.hpp>
#include <nvvkglsl/glsl.hpp>

#include "gl_vk.hpp"


//-----------------------------------------------------------------------------------------------------
// This class create a Vulkan compute shader and writes an image to the target texture.
// The texture, been shared between Vulkan and OpenGL, the OpenGL renderer will display the
// image created by this compute shader.

class ComputeImageVk
{
public:
  ComputeImageVk() = default;
  void setup(nvvk::ResourceAllocatorExport& allocator, uint32_t queueIdxGraphic)
  {
    m_allocator       = &allocator;
    m_device          = m_allocator->getDevice();
    m_physicalDevice  = m_allocator->getPhysicalDevice();
    m_queueIdxGraphic = queueIdxGraphic;

    VkPipelineCacheCreateInfo pipelineCacheInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    NVVK_CHECK(vkCreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &m_pipelineCache));

    VkFenceCreateInfo finfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    NVVK_CHECK(vkCreateFence(m_device, &finfo, nullptr, &m_fence));

    // Create a compute capable device queue
    vkGetDeviceQueue(m_device, m_queueIdxGraphic, 0, &m_queue);

    // Separate command pool as queue family for compute may be different than graphics
    VkCommandPoolCreateInfo commandPoolInfo{.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = m_queueIdxGraphic};
    NVVK_CHECK(vkCreateCommandPool(m_device, &commandPoolInfo, nullptr, &m_commandPool));

    // Create the command buffer for the execution of the compute shader
    VkCommandBufferAllocateInfo commandBufferInfo{.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool        = m_commandPool,
                                                  .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    NVVK_CHECK(vkAllocateCommandBuffers(m_device, &commandBufferInfo, &m_commandBuffer));

    // Create the semaphores for synchronization between OpenGL and Vulkan
    m_semaphores.create(m_device);

    createDescriptors();  // Shader parameters
    createPipelines();    // Shader pipelines
  }


  void destroy()
  {
    vkQueueWaitIdle(m_queue);  // Wait that the queue is idle before destroying resources

    m_textureTarget.destroy(*m_allocator);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffer);
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_semaphores.destroy(m_device);
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    vkDestroyFence(m_device, m_fence, nullptr);

    // Clean up used Vulkan resources
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
  }


  //-----------------------------------------------------------------------------------------------------
  // When the size of the texture changes (UI), a new Vulkan texture is re-created and the OpenGL version is imported.
  void update(VkExtent2D extent)
  {
    m_textureTarget.destroy(*m_allocator);
    m_textureTarget = prepareTextureTarget(VK_IMAGE_LAYOUT_GENERAL, extent, VK_FORMAT_R8G8B8A8_UNORM);
    createTextureGL(*m_allocator, m_textureTarget, GL_RGBA8, GL_LINEAR, GL_LINEAR, GL_REPEAT);

    updateDescriptors();
  }

  //-----------------------------------------------------------------------------------------------------
  // Shader descriptors, which is defining where to write the image (storage image)
  void createDescriptors()
  {
    // Create the descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
        // Binding 0 : Sampled image (write)
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                            .bindingCount = uint32_t(setLayoutBindings.size()),
                                                            .pBindings    = setLayoutBindings.data()};
    NVVK_CHECK(vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutInfo, nullptr, &m_descriptorSetLayout));

    // Pool for the descriptor set
    std::vector<VkDescriptorPoolSize> poolSizes{{.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1}};
    VkDescriptorPoolCreateInfo        descriptorPoolInfo{.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                         .maxSets       = 3,
                                                         .poolSizeCount = uint32_t(poolSizes.size()),
                                                         .pPoolSizes    = poolSizes.data()};
    NVVK_CHECK(vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr, &m_descriptorPool));

    // Allocate the descriptor set
    VkDescriptorSetAllocateInfo allocInfo{.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool     = m_descriptorPool,
                                          .descriptorSetCount = 1,
                                          .pSetLayouts        = &m_descriptorSetLayout};
    NVVK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet));
  }

  //-----------------------------------------------------------------------------------------------------
  // Update the values to the descriptor set
  void updateDescriptors() const
  {
    VkDescriptorImageInfo computeTexDescriptor{.imageView   = m_textureTarget.imageExportVk.descriptor.imageView,
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

  //-----------------------------------------------------------------------------------------------------
  // Create the compute shader pipeline
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

    // Search paths for the shader files
    std::filesystem::path                    exePath     = nvutils::getExecutablePath().parent_path();
    const std::vector<std::filesystem::path> shaderPaths = {exePath / TARGET_EXE_TO_SOURCE_DIRECTORY / "shaders",
                                                            exePath / TARGET_NAME "_files" / "shaders", exePath};

    // Compile the GLSL compute shader to SPIR-V
    nvvkglsl::GlslCompiler glslCompiler;
    glslCompiler.addSearchPaths(shaderPaths);
    glslCompiler.defaultOptions();
    glslCompiler.defaultTarget();
    glslCompiler.options().SetGenerateDebugInfo();
    glslCompiler.options().SetOptimizationLevel(shaderc_optimization_level_zero);
    shaderc::SpvCompilationResult compResult =
        glslCompiler.compileFile("shader.comp.glsl", shaderc_shader_kind::shaderc_glsl_compute_shader);
    assert(compResult.GetCompilationStatus() == shaderc_compilation_status_success);

    // Create the shader pipeline from the compiled SPIR-V code
    const VkShaderModuleCreateInfo createInfo{.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                              .codeSize = glslCompiler.getSpirvSize(compResult),
                                              .pCode    = glslCompiler.getSpirv(compResult)};
    VkShaderModule                 shaderModule{};
    NVVK_CHECK(vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule));

    VkPipelineShaderStageCreateInfo shaderStage{.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = shaderModule,
                                                .pName  = "main"};

    VkComputePipelineCreateInfo computePipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = shaderStage, .layout = m_pipelineLayout};
    NVVK_CHECK(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &computePipelineInfo, nullptr, &m_pipeline));
    vkDestroyShaderModule(m_device, computePipelineInfo.stage.module, nullptr);
  }

  //-----------------------------------------------------------------------------------------------------
  // Build the list of commands to execute the compute shader.
  // The commands will be submitted for execution in the submit() function.
  void buildCommandBuffers() const
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

    vkCmdDispatch(m_commandBuffer, (m_textureTarget.imageExportVk.extent.width + 15) / 16,
                  (m_textureTarget.imageExportVk.extent.height + 15) / 16, 1);
    NVVK_CHECK(vkEndCommandBuffer(m_commandBuffer));
  }


  //-----------------------------------------------------------------------------------------------------
  // Creates the Vulkan texture target that will be used as a storage image in the compute shader.
  // It also adds export information to import the texture as an OpenGL texture.
  nvvk::Texture2DVkGL prepareTextureTarget(VkImageLayout targetLayout, const VkExtent2D& extent, VkFormat format)
  {
    // Get device properties for the requested texture format
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
    // Check if requested image format supports image storage operations
    assert(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);

    // Create the texture from the image and add a default sampler
    nvvk::Texture2DVkGL texture;

    // Structure for image creation with export flag capability
    const VkExternalMemoryImageCreateInfo externalMemCreateInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
#ifdef _WIN32
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif  // _WIN32
    };

    VkImageCreateInfo imageCreateInfo{.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .pNext       = &externalMemCreateInfo,
                                      .imageType   = VK_IMAGE_TYPE_2D,
                                      .format      = format,
                                      .extent      = VkExtent3D{extent.width, extent.height, 1},
                                      .mipLevels   = 1,
                                      .arrayLayers = 1,
                                      .samples     = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling      = VK_IMAGE_TILING_OPTIMAL,
                                      // VkImage will be sampled in the fragment shader and used as storage target in the compute shader
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};

    VkImageViewCreateInfo imageViewCreateInfo = DEFAULT_VkImageViewCreateInfo;
    imageViewCreateInfo.format                = imageCreateInfo.format;

    m_allocator->createImageExport(texture.imageExportVk, imageCreateInfo, imageViewCreateInfo);

    {
      VkCommandBuffer cmd{};
      NVVK_CHECK(nvvk::beginSingleTimeCommands(cmd, m_device, m_commandPool));
      nvvk::cmdImageMemoryBarrier(cmd, {texture.imageExportVk.image, VK_IMAGE_LAYOUT_UNDEFINED, targetLayout});
      NVVK_CHECK(nvvk::endSingleTimeCommands(cmd, m_device, m_commandPool, m_queue));
    }

    return texture;
  }

  //-----------------------------------------------------------------------------------------------------
  // Submit the compute commands to the queue.
  // This will signal the semaphores to notify OpenGL that the compute shader has completed.
  void submit() const
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

  // Getters
  const nvvk::Texture2DVkGL&  textureTarget() const { return m_textureTarget; }
  const nvvk::SemaphoresVkGL& semaphores() const { return m_semaphores; }

private:
  nvvk::ResourceAllocatorExport* m_allocator{};    // Resource allocator to create Vulkan resources
  nvvk::SemaphoresVkGL           m_semaphores{};   // Semaphores for synchronization between OpenGL and Vulkan
  nvvk::Texture2DVkGL            m_textureTarget;  // Texture target for the compute shader output

  VkDevice         m_device{};          // Vulkan device
  VkPhysicalDevice m_physicalDevice{};  // Vulkan physical device, used to get information about the device capabilities

  VkCommandPool   m_commandPool{};    // Command pool for the compute queue
  VkCommandBuffer m_commandBuffer{};  // Command buffer for the compute commands

  VkFence m_fence{};  // Fence to wait for the compute commands to complete [Since we have a vkWaitIdle in submit(), this is not strictly necessary]

  uint32_t m_queueIdxGraphic{};  // Queue index for the graphics queue, which is used to submit the compute commands
  VkQueue  m_queue{};            // Queue for the compute commands

  VkDescriptorPool      m_descriptorPool{};       // Descriptor pool for the compute shader
  VkDescriptorSet       m_descriptorSet{};        // Descriptor set for the compute shader
  VkDescriptorSetLayout m_descriptorSetLayout{};  // Layout of the descriptor set

  VkPipelineLayout m_pipelineLayout{};  // Pipeline layout for the compute shader
  VkPipeline       m_pipeline{};        // Pipeline for the compute shader
  VkPipelineCache m_pipelineCache{};  // Pipeline cache to store the compiled shader pipelines [to speed up subsequent runs, but not strictly necessary]
};

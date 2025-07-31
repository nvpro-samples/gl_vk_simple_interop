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

#include <nvvk/check_error.hpp>
#include <nvvk/resource_allocator.hpp>
#include <nvgl/extensions.hpp>
#include <unordered_map>
#include <atomic>

#ifdef WIN32
#include <handleapi.h>
#else
#include <unistd.h>
#endif

namespace nvvk {

//------------------------------------------------------------------------------------------------
// Memory object manager for Vulkan-OpenGL interop with reference counting
// Manages OpenGL memory objects with automatic cleanup when reference count reaches zero
class MemoryObjectManager
{
public:
  ~MemoryObjectManager() { assert(m_importedMemoryObjects.empty() && "Missing to call clear()"); }

  // Acquire a memory object for the given VMA allocation
  // Returns the OpenGL memory object handle
  GLuint acquireMemoryObject(VmaAllocation allocation, nvvk::ResourceAllocatorExport& allocator)
  {
    VmaAllocationInfo2 allocationInfo2;
    vmaGetAllocationInfo2(allocator, allocation, &allocationInfo2);

    VkDeviceMemory deviceMemory = allocationInfo2.allocationInfo.deviceMemory;

    // Check if already imported
    auto it = m_importedMemoryObjects.find(deviceMemory);
    if(it != m_importedMemoryObjects.end())
    {
      // Increment reference count
      auto refIt = m_refCounts.find(it->second);
      if(refIt != m_refCounts.end())
      {
        refIt->second.fetch_add(1);
      }
      return it->second;
    }

    // Create new memory object
    GLuint memoryObject;
    glCreateMemoryObjectsEXT(1, &memoryObject);

#ifdef WIN32
    HANDLE handle;
    NVVK_CHECK(vmaGetMemoryWin32Handle(allocator, allocation, nullptr, &handle));
    glImportMemoryWin32HandleEXT(memoryObject, allocationInfo2.blockSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    // Store the handle for later cleanup
    m_win32Handles[memoryObject] = handle;
#else
    VkMemoryGetFdInfoKHR getInfo = {.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                    .memory     = deviceMemory,
                                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR};
    int                  fd{-1};
    NVVK_CHECK(vkGetMemoryFdKHR(allocator.getDevice(), &getInfo, &fd));
    glImportMemoryFdEXT(memoryObject, allocationInfo2.blockSize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
#endif

    // Store the mapping and initialize reference count
    m_importedMemoryObjects[deviceMemory] = memoryObject;
    m_refCounts[memoryObject]             = 1;

    return memoryObject;
  }

  // Release a memory object (decrement reference count)
  void releaseMemoryObject(GLuint memObject)
  {
    auto it = m_refCounts.find(memObject);
    if(it != m_refCounts.end())
    {
      uint64_t newCount = it->second.fetch_sub(1) - 1;
      if(newCount == 0)
      {
        // Reference count reached zero, delete the OpenGL memory object
        glDeleteMemoryObjectsEXT(1, &memObject);

#ifdef WIN32
        // Close the Windows handle if it exists
        auto handleIt = m_win32Handles.find(memObject);
        if(handleIt != m_win32Handles.end())
        {
          CloseHandle(handleIt->second);
          m_win32Handles.erase(handleIt);
        }
#endif

        // Remove from both maps
        m_refCounts.erase(it);

        // Find and remove from importedMemoryObjects map
        for(auto memIt = m_importedMemoryObjects.begin(); memIt != m_importedMemoryObjects.end(); ++memIt)
        {
          if(memIt->second == memObject)
          {
            m_importedMemoryObjects.erase(memIt);
            break;
          }
        }
      }
    }
  }

  // Clear all memory objects (useful for cleanup)
  void clear()
  {
    for(auto& [memoryObject, refCount] : m_refCounts)
    {
      if(memoryObject != 0)
      {
        glDeleteMemoryObjectsEXT(1, &memoryObject);
      }
    }

#ifdef WIN32
    // Close all Windows handles
    for(auto& [memoryObject, handle] : m_win32Handles)
    {
      if(handle != nullptr)
      {
        CloseHandle(handle);
      }
    }
    m_win32Handles.clear();
#endif

    m_importedMemoryObjects.clear();
    m_refCounts.clear();
  }

  // Remove a specific memory object by device memory
  void remove(VkDeviceMemory deviceMemory)
  {
    auto it = m_importedMemoryObjects.find(deviceMemory);
    if(it != m_importedMemoryObjects.end())
    {
      releaseMemoryObject(it->second);
    }
  }

private:
  std::unordered_map<VkDeviceMemory, GLuint>       m_importedMemoryObjects;
  std::unordered_map<GLuint, std::atomic_uint64_t> m_refCounts;
#ifdef WIN32
  std::unordered_map<GLuint, HANDLE>               m_win32Handles;
#endif
};

// Global manager instance
static MemoryObjectManager g_memoryObjectManager;

// Utility function to clear the global memory object manager
// Call this when cleaning up the application or when you want to free all cached memory objects
inline void clearMemoryObjectManager()
{
  g_memoryObjectManager.clear();
}

//------------------------------------------------------------------------------------------------
// Vulkan-OpenGL synchronization semaphores
// Manages paired Vulkan and OpenGL semaphores for cross-API synchronization
// during Vulkan-OpenGL interop operations
//
struct SemaphoresVkGL
{
  VkSemaphore vkReady;
  VkSemaphore vkComplete;
  GLuint      glReady;
  GLuint      glComplete;

  void create(VkDevice device)
  {
    glGenSemaphoresEXT(1, &glReady);
    glGenSemaphoresEXT(1, &glComplete);

    // Create semaphores
#ifdef WIN32
    const auto handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    const auto handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkExportSemaphoreCreateInfo esci{.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = handleType};
    VkSemaphoreCreateInfo       sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci};
    NVVK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &vkReady));
    NVVK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &vkComplete));

    // Import semaphores
#ifdef WIN32
    {
      HANDLE                           hglReady{};
      VkSemaphoreGetWin32HandleInfoKHR handleInfo{.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
                                                  .semaphore  = vkReady,
                                                  .handleType = handleType};
      NVVK_CHECK(vkGetSemaphoreWin32HandleKHR(device, &handleInfo, &hglReady));

      HANDLE hglComplete{};
      handleInfo.semaphore = vkComplete;
      NVVK_CHECK(vkGetSemaphoreWin32HandleKHR(device, &handleInfo, &hglComplete));

      glImportSemaphoreWin32HandleEXT(glReady, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglReady);
      glImportSemaphoreWin32HandleEXT(glComplete, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglComplete);
    }
#else
    {
      int fdReady{};
      VkSemaphoreGetFdInfoKHR handleInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = vkReady, .handleType = handleType};
      NVVK_CHECK(vkGetSemaphoreFdKHR(device, &handleInfo, &fdReady));

      int fdComplete{};
      handleInfo.semaphore = vkComplete;
      NVVK_CHECK(vkGetSemaphoreFdKHR(device, &handleInfo, &fdComplete));

      glImportSemaphoreFdEXT(glReady, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdReady);
      glImportSemaphoreFdEXT(glComplete, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdComplete);
    }
#endif
  }

  void destroy(VkDevice device)
  {
    vkDestroySemaphore(device, vkReady, nullptr);
    vkDestroySemaphore(device, vkComplete, nullptr);
    glDeleteSemaphoresEXT(1, &glReady);
    glDeleteSemaphoresEXT(1, &glComplete);
  }
};

//------------------------------------------------------------------------------------------------
// Vulkan-OpenGL shared buffer resource
// Encapsulates a buffer that can be accessed by both Vulkan and OpenGL APIs
// through shared memory allocation and cross-API handle management
//
struct BufferVkGL
{
  nvvk::Buffer bufVk;  // The allocated buffer
  GLuint memoryObject = 0;  // OpenGL memory object
  GLuint oglId        = 0;  // OpenGL object ID

  void destroy(nvvk::ResourceAllocatorExport& alloc)
  {
    alloc.destroyBuffer(bufVk);
    glDeleteBuffers(1, &oglId);
    // Release the memory object reference
    if(memoryObject != 0)
    {
      g_memoryObjectManager.releaseMemoryObject(memoryObject);
      memoryObject = 0;
    }
  }
};

//------------------------------------------------------------------------------------------------
// Vulkan-OpenGL shared 2D texture resource
// Encapsulates a 2D texture that can be accessed by both Vulkan and OpenGL APIs
// through shared memory allocation and cross-API handle management
//
struct Texture2DVkGL
{
  nvvk::Image imageExportVk;

  uint32_t mipLevels{1};
  GLuint memoryObject{0};  // OpenGL memory object
  GLuint oglId{0};         // OpenGL object ID

  void destroy(nvvk::ResourceAllocatorExport& alloc)
  {
    glDeleteTextures(1, &oglId);
    // Release the memory object reference
    if(memoryObject != 0)
    {
      g_memoryObjectManager.releaseMemoryObject(memoryObject);
      memoryObject = 0;
    }
    alloc.destroyImage(imageExportVk);
  }
};

//------------------------------------------------------------------------------------------------
// Creates an OpenGL buffer that shares memory with a Vulkan buffer
// Uses the reference-counted memory object system for efficient cross-API resource sharing
//
inline void createBufferGL(nvvk::ResourceAllocatorExport& allocator, BufferVkGL& bufGl)
{
  VkDevice device = allocator.getDevice();

  VmaAllocationInfo2 allocationInfo2;
  vmaGetAllocationInfo2(allocator, bufGl.bufVk.allocation, &allocationInfo2);

  glCreateBuffers(1, &bufGl.oglId);

  // Use reference-counted memory object manager
  bufGl.memoryObject = g_memoryObjectManager.acquireMemoryObject(bufGl.bufVk.allocation, allocator);

  glNamedBufferStorageMemEXT(bufGl.oglId, allocationInfo2.allocationInfo.size, bufGl.memoryObject,
                             allocationInfo2.allocationInfo.offset);
}

//------------------------------------------------------------------------------------------------
// Creates an OpenGL texture that shares memory with a Vulkan image
// Uses the reference-counted memory object system for efficient cross-API resource sharing
//
inline void createTextureGL(nvvk::ResourceAllocatorExport& allocator, Texture2DVkGL& texGl, int format, int minFilter, int magFilter, int wrap)
{
  VkDevice device = allocator.getDevice();

  VmaAllocationInfo2 allocationInfo2;
  vmaGetAllocationInfo2(allocator, texGl.imageExportVk.allocation, &allocationInfo2);

  // Use reference-counted memory object manager
  texGl.memoryObject = g_memoryObjectManager.acquireMemoryObject(texGl.imageExportVk.allocation, allocator);

  glCreateTextures(GL_TEXTURE_2D, 1, &texGl.oglId);
  glTextureStorageMem2DEXT(texGl.oglId, texGl.mipLevels, format, texGl.imageExportVk.extent.width,
                           texGl.imageExportVk.extent.height, texGl.memoryObject, allocationInfo2.allocationInfo.offset);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MIN_FILTER, minFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MAG_FILTER, magFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_S, wrap);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_T, wrap);
}

}  // namespace nvvk

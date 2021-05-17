/*
 * Copyright (c) 2019-2021, NVIDIA CORPORATION.  All rights reserved.
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
 * SPDX-FileCopyrightText: Copyright (c) 2019-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


#pragma once

#include <vulkan/vulkan.hpp>

#include "nvvk/resourceallocator_vk.hpp"
#include <nvgl/extensions_gl.hpp>

#ifdef WIN32
#include <handleapi.h>
#else
#include <unistd.h>
#endif

namespace nvvkpp {

// #VKGL Extra for Interop
struct BufferVkGL
{
  nvvk::Buffer bufVk;  // The allocated buffer

#ifdef WIN32
  HANDLE handle = nullptr;  // The Win32 handle
#else
  int fd = -1;
#endif
  GLuint memoryObject = 0;  // OpenGL memory object
  GLuint oglId        = 0;  // OpenGL object ID

  void destroy(nvvk::ResourceAllocator& alloc)
  {
    alloc.destroy(bufVk);
#ifdef WIN32
    CloseHandle(handle);
#else
    if(fd != -1)
    {
      close(fd);
      fd = -1;
    }
#endif
    glDeleteBuffers(1, &oglId);
    glDeleteMemoryObjectsEXT(1, &memoryObject);
  }
};

// #VKGL Extra for Interop
struct Texture2DVkGL
{
  nvvk::Texture texVk;

  uint32_t     mipLevels{1};
  vk::Extent2D imgSize{0, 0};
#ifdef WIN32
  HANDLE handle{nullptr};  // The Win32 handle
#else
  int fd{-1};
#endif
  GLuint memoryObject{0};  // OpenGL memory object
  GLuint oglId{0};         // OpenGL object ID

  void destroy(nvvk::ResourceAllocator& alloc)
  {
    alloc.destroy(texVk);

#ifdef WIN32
    CloseHandle(handle);
#else
    if(fd != -1)
    {
      close(fd);
      fd = -1;
    }
#endif
    glDeleteBuffers(1, &oglId);
    glDeleteMemoryObjectsEXT(1, &memoryObject);
  }
};

// Get the Vulkan buffer and create the OpenGL equivalent using the memory allocated in Vulkan
inline void createBufferGL(nvvk::ResourceAllocator& alloc, BufferVkGL& bufGl)
{
  vk::Device device = alloc.getDevice();
  nvvk::MemAllocator::MemInfo info = alloc.getMemoryAllocator()->getMemoryInfo(bufGl.bufVk.memHandle);
#ifdef WIN32
  bufGl.handle = device.getMemoryWin32HandleKHR({info.memory, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
#else
  bufGl.fd = device.getMemoryFdKHR({info.memory, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd});
#endif
  auto req = device.getBufferMemoryRequirements(bufGl.bufVk.buffer);

  glCreateBuffers(1, &bufGl.oglId);
  glCreateMemoryObjectsEXT(1, &bufGl.memoryObject);
#ifdef WIN32
  glImportMemoryWin32HandleEXT(bufGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, bufGl.handle);
#else
  glImportMemoryFdEXT(bufGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, bufGl.fd);
  // fd got consumed
  bufGl.fd = -1;
#endif
  glNamedBufferStorageMemEXT(bufGl.oglId, req.size, bufGl.memoryObject, info.offset);
}

// Get the Vulkan texture and create the OpenGL equivalent using the memory allocated in Vulkan
inline void createTextureGL(nvvk::ResourceAllocator& alloc, Texture2DVkGL& texGl, int format, int minFilter, int magFilter, int wrap)
{
  vk::Device                  device = alloc.getDevice();
  nvvk::MemAllocator::MemInfo info   = alloc.getMemoryAllocator()->getMemoryInfo(texGl.texVk.memHandle);
#ifdef WIN32
  texGl.handle = device.getMemoryWin32HandleKHR({info.memory, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
#else
  texGl.fd = device.getMemoryFdKHR({info.memory, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd});
#endif
  auto req = device.getImageMemoryRequirements(texGl.texVk.image);

  // Create a 'memory object' in OpenGL, and associate it with the memory allocated in Vulkan
  glCreateMemoryObjectsEXT(1, &texGl.memoryObject);
#ifdef WIN32
  glImportMemoryWin32HandleEXT(texGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, texGl.handle);
#else
  glImportMemoryFdEXT(texGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, texGl.fd);
  // fd got consumed
  texGl.fd = -1;
#endif
  glCreateTextures(GL_TEXTURE_2D, 1, &texGl.oglId);
  glTextureStorageMem2DEXT(texGl.oglId, texGl.mipLevels, format, texGl.imgSize.width, texGl.imgSize.height, texGl.memoryObject, info.offset);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MIN_FILTER, minFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MAG_FILTER, magFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_S, wrap);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_T, wrap);
}


}  // namespace nvvkpp

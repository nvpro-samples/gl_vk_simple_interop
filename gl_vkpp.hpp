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

#include <vulkan/vulkan.hpp>

#include "nvvk/allocator_dedicated_vk.hpp"
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
  nvvk::BufferDedicated bufVk;  // The allocated buffer

#ifdef WIN32
  HANDLE handle = nullptr;  // The Win32 handle
#else
  int fd = -1;
#endif
  GLuint memoryObject = 0;  // OpenGL memory object
  GLuint oglId        = 0;  // OpenGL object ID

  void destroy(nvvk::AllocatorDedicated& alloc)
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
  nvvk::TextureDedicated texVk;

  uint32_t     mipLevels{1};
  vk::Extent2D imgSize{0, 0};
#ifdef WIN32
  HANDLE handle{nullptr};  // The Win32 handle
#else
  int fd{-1};
#endif
  GLuint memoryObject{0};  // OpenGL memory object
  GLuint oglId{0};         // OpenGL object ID

  void destroy(nvvk::AllocatorDedicated& alloc)
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
inline void createBufferGL(const vk::Device& device, BufferVkGL& bufGl)
{

#ifdef WIN32
  bufGl.handle = device.getMemoryWin32HandleKHR({bufGl.bufVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
#else
  bufGl.fd = device.getMemoryFdKHR({bufGl.bufVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd});
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
  glNamedBufferStorageMemEXT(bufGl.oglId, req.size, bufGl.memoryObject, 0);
}

// Get the Vulkan texture and create the OpenGL equivalent using the memory allocated in Vulkan
inline void createTextureGL(const vk::Device& device, Texture2DVkGL& texGl, int format, int minFilter, int magFilter, int wrap)
{
#ifdef WIN32
  texGl.handle = device.getMemoryWin32HandleKHR({texGl.texVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
#else
  texGl.fd = device.getMemoryFdKHR({texGl.texVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd});
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
  glTextureStorageMem2DEXT(texGl.oglId, texGl.mipLevels, format, texGl.imgSize.width, texGl.imgSize.height, texGl.memoryObject, 0);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MIN_FILTER, minFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MAG_FILTER, magFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_S, wrap);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_T, wrap);
}


}  // namespace nvvkpp

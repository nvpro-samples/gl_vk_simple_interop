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

#include "nvvkpp/allocator_dedicated_vkpp.hpp"
#include <handleapi.h>
#include <nvgl/extensions_gl.hpp>


namespace nvvkpp {

// #VKGL Extra for Interop
struct BufferVkGL
{
  nvvkpp::BufferDedicated bufVk;  // The allocated buffer

  HANDLE handle       = nullptr;  // The Win32 handle
  GLuint memoryObject = 0;        // OpenGL memory object
  GLuint oglId        = 0;        // OpenGL object ID

  void destroy(nvvkpp::AllocatorDedicated& alloc)
  {
    alloc.destroy(bufVk);
    CloseHandle(handle);
    glDeleteBuffers(1, &oglId);
    glDeleteMemoryObjectsEXT(1, &memoryObject);
  }
};

// #VKGL Extra for Interop
struct Texture2DVkGL
{
  nvvkpp::TextureDedicated texVk;

  uint32_t     mipLevels{1};
  vk::Extent2D imgSize{0, 0};

  HANDLE handle{nullptr};  // The Win32 handle
  GLuint memoryObject{0};  // OpenGL memory object
  GLuint oglId{0};         // OpenGL object ID


  void destroy(nvvkpp::AllocatorDedicated& alloc)
  {
    alloc.destroy(texVk);

    CloseHandle(handle);
    glDeleteBuffers(1, &oglId);
    glDeleteMemoryObjectsEXT(1, &memoryObject);
  }
};

// Get the Vulkan buffer and create the OpenGL equivalent using the memory allocated in Vulkan
inline void createBufferGL(const vk::Device& device, BufferVkGL& bufGl)
{

  bufGl.handle = device.getMemoryWin32HandleKHR({bufGl.bufVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
  auto req = device.getBufferMemoryRequirements(bufGl.bufVk.buffer);


  glCreateBuffers(1, &bufGl.oglId);
  glCreateMemoryObjectsEXT(1, &bufGl.memoryObject);
  glImportMemoryWin32HandleEXT(bufGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, bufGl.handle);
  glNamedBufferStorageMemEXT(bufGl.oglId, req.size, bufGl.memoryObject, 0);
}

// Get the Vulkan texture and create the OpenGL equivalent using the memory allocated in Vulkan
inline void createTextureGL(const vk::Device& device, Texture2DVkGL& texGl, int format, int minFilter, int magFilter, int wrap)
{
  texGl.handle = device.getMemoryWin32HandleKHR({texGl.texVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
  auto req = device.getImageMemoryRequirements(texGl.texVk.image);

  // Create a 'memory object' in OpenGL, and associate it with the memory allocated in Vulkan
  glCreateMemoryObjectsEXT(1, &texGl.memoryObject);
  glImportMemoryWin32HandleEXT(texGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, texGl.handle);
  glCreateTextures(GL_TEXTURE_2D, 1, &texGl.oglId);
  glTextureStorageMem2DEXT(texGl.oglId, texGl.mipLevels, format, texGl.imgSize.width, texGl.imgSize.height, texGl.memoryObject, 0);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MIN_FILTER, minFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_MAG_FILTER, magFilter);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_S, wrap);
  glTextureParameteri(texGl.oglId, GL_TEXTURE_WRAP_T, wrap);
}


}  // namespace nvvkpp

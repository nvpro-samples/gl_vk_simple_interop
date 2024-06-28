# OpenGL Interop


By [Martin-Karl Lefrançois](https://devblogs.nvidia.com/author/mlefrancois/)



This blog is an introduction to OpenGL and Vulkan interop. The goal is to explain how to mix Vulkan and 
OpenGL in the same application. In a nutshell, to achieve this, all objects are allocated in Vulkan, 
but rendered with OpenGL.

Topics covered:
- Managing OpenGL memory from Vulkan
- Interoperability OGL <==> VK
- Semaphores

![Screenshot](doc/screenshot.png)

# Interop Paradigm

For OpenGL to work with Vulkan, it is important that all memory objects (buffers) are allocated in Vulkan. 
A handle of that memory needs to be retrieved to create the OpenGL element. This new 
OpenGL object points to the exact same memory location as the Vulkan one, meaning that changes through 
either API are visible on both sides.

In the current example, we will deal with two memory objects:

- Vertices: a buffer of a triangle's vertex positions
- Image: the pixels of the image

Another important aspect is the synchronization between OpenGL and Vulkan. This topic will be discussed in detail
in the Semaphores section.


![Screenshot](doc/interop_api.png )

# Prerequisite

## Vulkan Instance and Device 

A Vulkan Instance and Device must be created in order to create and allocate memory buffers on a physical device. 

Main.cpp in the example creates a Vulkan instance and device using `nvvk::Context::init()`.  To create the Vulkan Device, we do not need a 
surface since we will not draw anything using Vulkan.



## Vulkan Extensions

To successfully export objects to other APIs, we need to enable several Vulkan extensions.

Specifically, we need these instance extensions:

- **VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME**
- **VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME**

We specify each of these extensions using `nvvk::ContextCreateInfo::addInstanceExtension()`. If the instance doesn't support both of these, the sample will exit.

In addition, we'll need these device extensions:

- **VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME**
- **VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME**

And two platform-specific device extensions, depending on whether the app's running on Windows or Linux:

**Windows:**

* **VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME**
* **VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME**

**Linux:**

* **VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME**
* **VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME**

We specify each of these extensions using `nvvk::ContextCreateInfo::addDeviceExtension()`.  Whether a GPU supports an extension depends both on the GPU and the GPU's driver. `nvvk::Context` will choose the first GPU that supports all the extensions we need; if none exist, the sample will exit.

## OpenGL
We use OpenGL 4.5 and need the extensions [EXT_external_objects](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_external_objects.txt) 
and [GL_EXT_semaphore](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_external_objects.txt).

Here are the functions from these extensions we use:
* `glCreateMemoryObjectsEXT`
* `glImportMemoryWin32HandleEXT`
* `glNamedBufferStorageMemEXT`
* `glTextureStorageMem2DEXT`
* `glSignalSemaphoreEXT`
* `glWaitSemaphoreEXT`




# Vulkan Allocation

In order to allocate a Vulkan buffer or image that can be exported to other APIs, we must use the [VK_KHR_external_memory](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VK_KHR_external_memory.html) extension.

In this example, we use a simple Vulkan memory allocator, `nvvk::ExportResourceAllocatorDedicated`. This allocator uses dedicated allocation — one memory allocation per buffer. This is not the recommended way; it would be better to allocate larger memory blocks and suballocate by binding buffers to some memory sections (see [this article](https://developer.nvidia.com/vulkan-memory-management)), but it is fine for the purpose of this example. (One way of doing this using NVVK is to pass a different memory allocator, such as `DMAMemoryAllocator`, to `ExportResourceAllocator`'s constructor.)

Normally, memory allocation is done like this:

~~~~C++
virtual VkDeviceMemory AllocateMemory(const VkMemoryAllocateInfo& allocateInfo)
{
  VkDeviceMemory memory;
  vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory);
  return memory;
}
~~~~

But `ExportResourceAllocator` flags all its memory allocations as exportable, like this:
~~~C++
VkDeviceMemory AllocateMemory(const VkMemoryAllocateInfo& allocateInfo) override
{
  // Enable export to either a Win32 handle or a POSIX file descriptor,
  // depending on the OS:
  VkExportMemoryAllocateInfo exportInfo {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
#ifdef WIN32
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_OPAQUE_WIN32_BIT;
#else // POSIX
  exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
  VkMemoryAllocateInfo modifiedInfo = allocateInfo;
  modifiedInfo.pNext = &exportInfo;
    
  VkDeviceMemory memory;
  vkAllocateMemory(m_device, &modifiedInfo, nullptr, &memory);
  return memory;
}
~~~




# OpenGL Handle and Memory Object

To retrieve the memory object for OpenGL, we must get the object's handle -- a `HANDLE`
 on Windows, and a POSIX file descriptor (`int`) on POSIX-compatible systems. See file: `gl_vkpp.hpp`

~~~~C++
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
};
~~~~


~~~~ C++
// #VKGL:  Get the shared handle between Vulkan and OpenGL
#ifdef WIN32
  VkMemoryGetWin32HandleInfoKHR getInfo = {
    .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
    .memory     = info.memory,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR};
  NVVK_CHECK(vkGetMemoryWin32HandleKHR(device, &getInfo, &bufGl.handle));
#else
  VkMemoryGetFdInfoKHR getInfo = {
    .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
    .memory     = info.memory,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR};
  NVVK_CHECK(vkGetMemoryFdKHR(device, &getInfo, &bufGl.fd));
#endif
~~~~

Using the handle, we can retrieve the equivalent OpenGL memory object.

~~~~ C++
  glCreateMemoryObjectsEXT(1, &bufGl.memoryObject);
#ifdef WIN32
  glImportMemoryWin32HandleEXT(bufGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, bufGl.handle);
#else
  glImportMemoryFdEXT(bufGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, bufGl.fd);
  // fd got consumed
  bufGl.fd = -1;
#endif
~~~~



# OpenGL Memory Binding

To use the retrieved OpenGL memory object, you must create the buffer then _link it_ using the 
[External Memory Object](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_external_objects.txt) extension.

In Vulkan we bind memory to our resources, in OpenGL we can create new resources from a range within imported memory, 
or we can attach existing resources to use that memory via [NV_memory_attachment](https://www.khronos.org/registry/OpenGL/extensions/NV/NV_memory_attachment.txt).

~~~~C++
  glCreateBuffers(1, &bufGl.oglId);
  glNamedBufferStorageMemEXT(bufGl.oglId, req.size, bufGl.memoryObject, info.offset);
~~~~

`bufGl.oglId` now shares data with the buffer that was created in Vulkan.



# OpenGL Images

For images, everything is done the same way as for buffers. We create an image using Vulkan, making sure to flag it as exportable using `ExportResourceAllocatorDedicated`. Then we create an OpenGL image using the same underlying memory in the `reateTextureGL()` function.

We retrieve the image's memory handle the same way we retrieved the buffer's handle:
~~~~C++
#ifdef WIN32
  VkMemoryGetWin32HandleInfoKHR getInfo = {
    .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
    .memory     = info.memory,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR};
  NVVK_CHECK(vkGetMemoryWin32HandleKHR(device, &getInfo, &texGl.handle));
#else
  VkMemoryGetFdInfoKHR getInfo = {
    .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
    .memory     = info.memory,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR};
  NVVK_CHECK(vkGetMemoryFdKHR(device, &getInfo, &texGl.fd));
#endif
~~~~

Then we import the Vulkan handle into an OpenGL memory object, like we did for the vertex buffer:
~~~~ C++
  // Create a 'memory object' in OpenGL, and associate it with the memory allocated in Vulkan
  glCreateMemoryObjectsEXT(1, &texGl.memoryObject);
#ifdef WIN32
  glImportMemoryWin32HandleEXT(texGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, texGl.handle);
#else
  glImportMemoryFdEXT(texGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, texGl.fd);
  // fd got consumed
  texGl.fd = -1;
#endif
~~~~

Finally, the texture will be created using the memory object 

~~~~C++
  glCreateTextures(GL_TEXTURE_2D, 1, &texGl.oglId);
  glTextureStorageMem2DEXT(texGl.oglId, texGl.mipLevels, format, texGl.imgSize.width, texGl.imgSize.height, texGl.memoryObject, info.offset);
~~~~



# Semaphores

As we are creating an image through Vulkan and displaying it with OpenGL,
it is necessary to synchronize the two environments. We'll use a semaphore to make Vulkan wait for OpenGL to tell it when it can start rendering, and a second semaphore to make OpenGL wait for Vulkan to tell it when the image is ready.

~~~~ batch
                                                           
  +------------+                             +------------+
  | GL Context | signal               wait   | GL Context |
  +------------+     |                  ^    +------------+
                     v  +-----------+   |                  
                   wait |Vk Context | signal               
                        +-----------+                      
~~~~

Like images and buffers, we'll create semaphores using Vulkan, and then retrieve an OpenGL object. This part of the code is in `compute.hpp`:

~~~~ C++
struct Semaphores
{
  VkSemaphore vkReady;
  VkSemaphore vkComplete;
  GLuint      glReady;
  GLuint      glComplete;
} m_semaphores;
~~~~~

Exported semaphore handles use the `HANDLE` type on Windows, and the type for a POSIX file descriptor (`int`) on POSIX-compatible platforms.
~~~~C++
#ifdef WIN32
  const auto handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
  const auto handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
~~~~

When creating semaphores, we need to flag them as exportable by adding a `VkExportSemaphoreCreateInfo` struct to the `pNext` chain of `VkSemaphoreCreateInfo`:
~~~~C++
VkExportSemaphoreCreateInfo esci{
  .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
  .handleTypes = handleType
};
VkSemaphoreCreateInfo sci{
  .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  .pNext = &esci
};
NVVK_CHECK(vkCreateSemaphore(m_device, &sci, nullptr, &m_semaphores.vkReady));
NVVK_CHECK(vkCreateSemaphore(m_device, &sci, nullptr, &m_semaphores.vkComplete));
~~~~

We then retrieve each semaphore's handle using one of the `vkGetSemaphore*KHR` functions, and import it into OpenGL using one of the `glImportSemaphore*EXT` functions:
~~~~C++
// Import semaphores
#ifdef WIN32
  HANDLE hglReady{};
  VkSemaphoreGetWin32HandleInfoKHR handleInfo{
    .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
    .semaphore  = m_semaphores.vkReady,
    .handleType = handleType
  };
  NVVK_CHECK(vkGetSemaphoreWin32HandleKHR(m_device, &handleInfo, &hglReady));

  HANDLE hglComplete{};
  handleInfo.semaphore = m_semaphores.vkComplete;
  NVVK_CHECK(vkGetSemaphoreWin32HandleKHR(m_device, &handleInfo, &hglComplete));
  
  glImportSemaphoreWin32HandleEXT(m_semaphores.glReady, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglReady);
  glImportSemaphoreWin32HandleEXT(m_semaphores.glComplete, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglComplete);
#else
  int                     fdReady{};
  VkSemaphoreGetFdInfoKHR handleInfo{
    .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
    .semaphore  = m_semaphores.vkReady,
    .handleType = handleType
  };
  NVVK_CHECK(vkGetSemaphoreFdKHR(m_device, &handleInfo, &fdReady));
  
  int fdComplete{};
  handleInfo.semaphore = m_semaphores.vkComplete;
  NVVK_CHECK(vkGetSemaphoreFdKHR(m_device, &handleInfo, &fdComplete));
  
  glImportSemaphoreFdEXT(m_semaphores.glReady, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdReady);
  glImportSemaphoreFdEXT(m_semaphores.glComplete, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fdComplete);
#endif
~~~~



# Animation

Since the Vulkan memory for the vertex buffer was allocated using
the`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` flags, we can easily update the buffer by doing the following:

~~~~C++
g_vertexDataVK[0].pos.x = sin(t);
g_vertexDataVK[1].pos.y = cos(t);
g_vertexDataVK[2].pos.x = -sin(t);
memcpy(m_vkBuffer.mapped, g_vertexDataVK.data(), g_vertexDataVK.size() * sizeof(Vertex));
~~~~

Note that we use a host-visible buffer for the sake of simplicity, at the expense of efficiency. For best performance the geometry
would need to be uploaded to device-local memory through a staging buffer.

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
A handle of that memory needs to be retrieved which is used to create the OpenGL element. This new 
OpenGL object is pointing to the exact same memory location as the Vulkan one, meaning that changes through 
either API are visible on both sides.

In the current example, we will deal with two memory objects:

- Vertices: holding the triangle objects
- Image: the pixels of the image

Another important aspect is the synchronization between OpenGL and Vulkan. This topic will be discussed in detail
in the section Semaphores.


![Screenshot](doc/interop_api.png )

# Prerequisite

## Vulkan Instance and Device 

A Vulkan Instance and a Device must be created to be able to create and allocate memory buffers on a physical device. 

In the example (main.cpp), Vulkan Instance is created calling `createInstance()`. To create the Vulkan Device, we do not need a 
surface since we will not draw anything using Vulkan. We are creating using `createDevice()` and using the first device(GPU) 
on the computer.



## Vulkan Extensions

Before being able to start allocating Vulkan buffers and using semaphores, Vulkan needs 
to have extensions enabled to be able to make the export of objects working.

Instance extensions through `requireExtensions`:
- **VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME**
- **VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME**

For the creation of the Device through, extensions are set with `requireDeviceExtensions`:
- **VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME**
- **VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME**
- **VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME**
- **VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME**

## OpenGL
For OpenGL we are using OpenGL 4.5 and need the extensions [EXT_external_objects](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_external_objects.txt) 
and [GL_EXT_semaphore](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_external_objects.txt) 

Here are the extra functions we are using:
* `glCreateMemoryObjectsEXT`
* `glImportMemoryWin32HandleEXT`
* `glNamedBufferStorageMemEXT`
* `glTextureStorageMem2DEXT`
* `glSignalSemaphoreEXT`
* `glWaitSemaphoreEXT`




# Vulkan Allocation

When allocating a Vulkan buffer, it is required to use the [ExportMemoryAllocation](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VK_KHR_external_memory.html) extension.

In this example, we are using a simple Vulkan memory allocator. This allocator is doing decitated allocation, one memroy allocation per buffer. This is not the recommended way, it would be better to allocate larger memory block and bind buffers to some memroy sections, but it is fine for the purpose of this example.

Form this dedicated vulkan memory allocator(`AllocatorDedicated`), we have derived it (`AllocatorVkExport`) to export all memory allocation.
See (`nvpro-samples\shared_sources\nvvkpp\allocator_dedicated_vkpp.hpp`)

Normally, the memory allocation is done like this:
~~~~C++
  virtual vk::DeviceMemory AllocateMemory(vk::MemoryAllocateInfo& allocateInfo)
  {
    return m_device.allocateMemory(allocateInfo);
  }
~~~~

But since we want to flag this to memroy be exported, we have overriden the function and setting to the pNext, the required information.
~~~C++
  vk::DeviceMemory AllocateMemory(vk::MemoryAllocateInfo& allocateInfo) override
  {
    vk::ExportMemoryAllocateInfo memoryHandleEx(vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32);
    allocateInfo.setPNext(&memoryHandleEx);  // <-- Enabling Export
    return m_device.allocateMemory(allocateInfo);
  }
~~~

Having this done, we will have an exportable handle type for a device memory object.


**!!! note**
    This must be done for all memory objects that need to be visible for both Vulkan and OpenGL.

**!!! warn Best Memory Usage Practice**
    We have used a very simplistic approach, for better usage of memory, see this [blog](https://developer.nvidia.com/vulkan-memory-management).




# OpenGL Handle and Memory Object

To retrieve the memory object for OpenGL, we must get the memory `HANDLE`. 
See file: `gl_vkpp.hpp`

Note: the Vulkan buffer structure was extended to hold the OpenGL information

~~~~C++
// #VKGL Extra for Interop
struct BufferVkGL : public Buffer
{
  HANDLE handle       = nullptr;  // The Win32 handle
  GLuint memoryObject = 0;        // OpenGL memory object
  GLuint oglId        = 0;        // OpenGL object ID
};
~~~~


~~~~ C++
  // #VKGL:  Get the share Win32 handle between Vulkan and OpenGL
  bufGl.handle = device.getMemoryWin32HandleKHR(
					{bufGl.bufVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
~~~~

With the `HANDLE` we can retrieve the equivalent OpenGL memory object.

~~~~ C++
  // Get the OpenGL Memory object
  glCreateMemoryObjectsEXT(1, &bufGl.memoryObject);
  auto req     = device.getBufferMemoryRequirements(bufGl.bufVk.buffer);
  glImportMemoryWin32HandleEXT(bufGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, bufGl.handle);
~~~~



# OpenGL Memory Binding

To use the retrieved OpenGL memory object, you must create the buffer then _link it_ using the 
[External Memory Object](https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_external_objects.txt) extension.

In Vulkan we bind memory to our resources, in OpenGL we can create new resources from a range within imported memory, 
or we can attach existing resources to use that memory via [NV_memory_attachment](https://www.khronos.org/registry/OpenGL/extensions/NV/NV_memory_attachment.txt).

~~~~C++
  glCreateBuffers(1, &bufGl.oglId);
  glNamedBufferStorageMemEXT(bufGl.oglId, req.size, bufGl.memoryObject, 0);
~~~~

At this point, `m_bufferVk` is sharing the data that was allocated in Vulkan.



# OpenGL Images

For images, everything is done the same way as for buffers. The memory 
allocation information needs to know to export the object, therefore the allocation is 
also adding the `memoryHandleEx` to `memAllocInfo.pNext`.

In this example, a compute shader in Vulkan is creating an image. That image
is converted to OpenGL in the function `createTextureGL`. 

The handle for the texture is retrived with: 
~~~~C++
  // Retrieving the memory handle
  texGl.handle = device.getMemoryWin32HandleKHR({texGl.texVk.allocation, vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32}, d);
~~~~ 

The buffer containing the image will done like for buffers
~~~~ C++
  // Create a 'memory object' in OpenGL, and associate it with the memory allocated in Vulkan
  glCreateMemoryObjectsEXT(1, &texGl.memoryObject);
  auto req = device.getImageMemoryRequirements(texGl.texVk.image);
  glImportMemoryWin32HandleEXT(texGl.memoryObject, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, texGl.handle);
~~~~ 

Finally, the texture will be created using the memory object 

~~~~C++
  glCreateTextures(GL_TEXTURE_2D, 1, &texGl.oglId);
  glTextureStorageMem2DEXT(texGl.oglId, texGl.mipLevels, format, texGl.imgSize.width, texGl.imgSize.height, texGl.memoryObject, 0);
~~~~



# Semaphores

As we are creating an image through Vulkan and displaying it with OpenGL,
it is necessary to synchronize the two environments. Semaphores will be 
used in Vulkan to wait for OpenGL that it can start generating the image,
then it will signal OpenGL when the image is ready. OpenGL is signaling 
Vulkan and waiting for its signal before displaying the image. 



~~~~ batch
                                                           
  +------------+                             +------------+
  | GL Context | signal               wait   | GL Context |
  +------------+     |                  ^    +------------+
                     v  +-----------+   |                  
                   wait |Vk Context | signal               
                        +-----------+                      
~~~~

Those semaphores are created in Vulkan, and as previously, the OpenGL 
version will be retrieved.

~~~~ C++
struct Semaphores
{
  vk::Semaphore vkReady;
  vk::Semaphore vkComplete;
  GLuint        glReady;
  GLuint        glComplete;
} m_semaphores;
~~~~~

This is the handle informing the creation of the semaphore to get exported.
~~~~C++
auto handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32;
~~~~ 

The creation of the semaphores needs to have the export object informaton. 
~~~~C++
vk::ExportSemaphoreCreateInfo esci{ handleType };
vk::SemaphoreCreateInfo       sci;
sci.pNext = &esci;
m_semaphores.vkReady = m_device.createSemaphore (sci);
m_semaphores.vkComplete = m_device.createSemaphore (sci);
~~~~ 

The convertion to OpenGL will be done the following way:
~~~~C++
// Import semaphores
HANDLE hglReady = m_device.getSemaphoreWin32HandleKHR({ m_semaphores.vkReady, handleType }, 
                                                       m_dynamicDispatch);
HANDLE hglComplete = m_device.getSemaphoreWin32HandleKHR({ m_semaphores.vkComplete, handleType }, 
                                                         m_dynamicDispatch);
glGenSemaphoresEXT (1, &m_semaphores.glReady);
glGenSemaphoresEXT (1, &m_semaphores.glComplete);
glImportSemaphoreWin32HandleEXT (m_semaphores.glReady, 
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglReady);
glImportSemaphoreWin32HandleEXT (m_semaphores.glComplete, 
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, hglComplete);
~~~~



# Animation

Since the Vulkan memory for the vertex buffer was allocated using
the flags: 

`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`

`vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent`

We can easily update the buffer doing the following:

~~~~C++
g_vertexDataVK[0].pos.x = sin(t);
g_vertexDataVK[1].pos.y = cos(t);
g_vertexDataVK[2].pos.x = -sin(t);
memcpy(m_vkBuffer.mapped, g_vertexDataVK.data(), g_vertexDataVK.size() * sizeof(Vertex));
~~~~
 
Note we use a host-visible buffer for the sake of simplicity, at the expense of efficiency. For best performance the geometry
would need to be uploaded to device-local memory through a staging buffer.

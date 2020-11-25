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


//--------------------------------------------------------------------------------------------------
// Very simple Vulkan-OpenGL example:
// - The vertex buffer is allocated with Vulkan, but used by OpenGL to render
// - The animation is updating the buffer allocated by Vulkan, and the changes are
//   reflected in the OGL render.
//

#ifdef WIN32
#include <accctrl.h>
#include <aclapi.h>
#endif

#include <array>
#include <chrono>
#include <iostream>
#include <vulkan/vulkan.hpp>

#define STB_IMAGE_IMPLEMENTATION

#include "imgui.h"
#include "imgui_impl_glfw.h"

#include "compute.hpp"
#include "fileformats/stb_image.h"
#include "imgui/imgui_impl_gl.h"
#include "nvgl/contextwindow_gl.hpp"
#include "nvgl/extensions_gl.hpp"
#include "nvpsystem.hpp"
#include "nvvk/appbase_vkpp.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/context_vk.hpp"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

int const SAMPLE_SIZE_WIDTH  = 1200;
int const SAMPLE_SIZE_HEIGHT = 900;

// Default search path for shaders
std::vector<std::string> defaultSearchPaths{
    "./",
    "../",
    std::string(PROJECT_NAME),
    std::string("SPV_" PROJECT_NAME),
    PROJECT_ABSDIRECTORY,
    NVPSystem::exePath() + std::string(PROJECT_RELDIRECTORY),
};

// Getting the system time use for animation
inline double getSysTime()
{
  auto now(std::chrono::system_clock::now());
  auto duration = now.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() / 1000.0;
}

// An array of 3 vectors which represents 3 vertices
struct Vertex
{
  nvmath::vec3 pos;
  nvmath::vec2 uv;
};

// The triangle
static std::vector<Vertex> g_vertexDataVK = {{{-1.0f, -1.0f, 0.0f}, {0, 0}},
                                             {{1.0f, -0.0f, 0.0f}, {1, 0}},
                                             {{0.0f, 1.0f, 0.0f}, {0.5, 1}}};


//--------------------------------------------------------------------------------------------------
//
//
class InteropExample : public nvvk::AppBase
{
public:
  void prepare(uint32_t queueIdxCompute)
  {
    m_alloc.init(m_device, m_physicalDevice);

    createShaders();   // Create the GLSL shaders
    createBufferVK();  // Create the vertex buffer

    // Initialize the Vulkan compute shader
    m_compute.setup(m_device, m_physicalDevice, m_graphicsQueueIndex, queueIdxCompute);
    m_compute.prepare();
    createTextureGL(m_device, m_compute.m_textureTarget, GL_RGBA8, GL_LINEAR, GL_LINEAR, GL_REPEAT);
  }

  void destroy() override
  {
    m_device.waitIdle();
    m_bufferVk.destroy(m_alloc);
    m_compute.destroy();

    AppBase::destroy();
  }

  //--------------------------------------------------------------------------------------------------
  // Create the vertex buffer with Vulkan
  //
  void createBufferVK()
  {
    m_bufferVk.bufVk = m_alloc.createBuffer(g_vertexDataVK.size() * sizeof(Vertex), vk::BufferUsageFlagBits::eVertexBuffer,
                                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    createBufferGL(m_device, m_bufferVk);

    // Same as usual
    int pos_loc = 0;
    int uv_loc  = 1;
    glCreateVertexArrays(1, &m_vertexArray);
    glEnableVertexArrayAttrib(m_vertexArray, pos_loc);
    glEnableVertexArrayAttrib(m_vertexArray, uv_loc);

    glVertexArrayAttribFormat(m_vertexArray, pos_loc, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribBinding(m_vertexArray, pos_loc, 0);
    glVertexArrayAttribFormat(m_vertexArray, uv_loc, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(m_vertexArray, uv_loc, 0);

    glVertexArrayVertexBuffer(m_vertexArray, 0, m_bufferVk.oglId, 0, sizeof(Vertex));
  }

  //--------------------------------------------------------------------------------------------------
  //
  //
  void onWindowRefresh()
  {
    glViewport(0, 0, m_size.width, m_size.height);

    // Signal Vulkan it can use the texture
    GLenum dstLayout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glSignalSemaphoreEXT(m_compute.m_semaphores.glReady, 0, nullptr, 1, &m_compute.m_textureTarget.oglId, &dstLayout);

    // Invoking Vulkan
    m_compute.buildCommandBuffers();
    m_compute.submit();

    // Wait (on the GPU side) for the Vulkan semaphore to be signaled (finished compute)
    GLenum srcLayout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    glWaitSemaphoreEXT(m_compute.m_semaphores.glComplete, 0, nullptr, 1, &m_compute.m_textureTarget.oglId, &srcLayout);

    glBindVertexArray(m_vertexArray);
    glBindTextureUnit(0, m_compute.m_textureTarget.oglId);
    glUseProgram(m_programID);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Printing stats
    {
      static float frameNumber{0};
      static auto  tStart = std::chrono::high_resolution_clock::now();
      auto         tEnd   = std::chrono::high_resolution_clock::now();
      auto         tDiff  = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
      frameNumber++;
      if(tDiff > 1000)
      {
        tStart   = tEnd;
        auto fps = frameNumber / tDiff * 1000.f;
        std::cout << "FPS: " << fps << std::endl;
        frameNumber = 0;
      }
    }
  }

  //--------------------------------------------------------------------------------------------------
  //
  //
  void animate()
  {
    double t = getSysTime() / 2.0;
    // Modify the buffer and upload it in the Vulkan allocated buffer
    g_vertexDataVK[0].pos.x = sin(t);
    g_vertexDataVK[1].pos.y = cos(t);
    g_vertexDataVK[2].pos.x = -sin(t);

    void* mapped = m_alloc.map(m_bufferVk.bufVk);
    // This works because the buffer was created with vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
    memcpy(mapped, g_vertexDataVK.data(), g_vertexDataVK.size() * sizeof(Vertex));
    m_alloc.unmap(m_bufferVk.bufVk);
  }

  //--------------------------------------------------------------------------------------------------
  // Creating the OpenGL shaders
  //
  GLuint createShaders()
  {
    // OpenGL - Create shaders
    char buf[512];
    int  len = 0;

    // OpenGL 4.2 Core
    GLuint        vs  = glCreateShader(GL_VERTEX_SHADER);
    GLchar const* vss = {R"(
      #version 450
      layout(location = 0) in vec3 inVertex;
      layout(location = 1) in vec2 inUV;
      layout (location = 0) out vec2 outUV;

      void main()
      {
        outUV = inUV;
        gl_Position = vec4(inVertex, 1.0f);
      }
    )"};

    glShaderSource(vs, 1, &vss, nullptr);
    glCompileShader(vs);
    glGetShaderInfoLog(vs, 512, (GLsizei*)&len, buf);

    GLuint        fs  = glCreateShader(GL_FRAGMENT_SHADER);
    GLchar const* fss = {R"(
      #version 450
      layout (location = 0) in vec2 inUV;
      layout(location = 0) out vec4 fragColor;
            
      uniform sampler2D myTextureSampler;

      void main()
      {
        vec3 color = texture( myTextureSampler, inUV ).rgb;
        fragColor = vec4(color,1);
      }
            
    )"};

    glShaderSource(fs, 1, &fss, nullptr);
    glCompileShader(fs);
    glGetShaderInfoLog(fs, 512, (GLsizei*)&len, buf);

    GLuint mSH2D = glCreateProgram();
    glAttachShader(mSH2D, vs);
    glAttachShader(mSH2D, fs);
    glLinkProgram(mSH2D);

    m_programID = mSH2D;
    return mSH2D;
  }

  //--------------------------------------------------------------------------------------------------
  // Initialization of the GUI
  // - Need to be call after the device creation
  //
  void initUI(int width, int height)
  {
    m_size.width  = width;
    m_size.height = height;

    // UI
    ImGui::CreateContext();
    ImGui::InitGL();
    ImGui::GetIO().IniFilename = nullptr;  // Avoiding the INI file
  }

  //- Override the default resize
  void onFramebufferSize(int w, int h) override
  {
    m_size.width  = w;
    m_size.height = h;
  }


private:
  nvvkpp::BufferVkGL      m_bufferVk;
  nvvk::AllocatorVkExport m_alloc;

  GLuint m_vertexArray = 0;  // VAO
  GLuint m_programID   = 0;  // Shader program

  ComputeImageVk m_compute;  // Compute in Vulkan
};

//--------------------------------------------------------------------------------------------------
//
//
int main(int argc, char** argv)
{
  // setup some basic things for the sample, logging file for example
  NVPSystem system(argv[0], PROJECT_NAME);

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  // Create window with graphics context
  GLFWwindow* window = glfwCreateWindow(SAMPLE_SIZE_WIDTH, SAMPLE_SIZE_HEIGHT, PROJECT_NAME, NULL, NULL);
  if(window == nullptr)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);  // Enable vsync

  nvvk::ContextCreateInfo deviceInfo;
  deviceInfo.addInstanceExtension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
  deviceInfo.addInstanceExtension(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
  deviceInfo.addDeviceExtension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  deviceInfo.addDeviceExtension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
#ifdef WIN32
  deviceInfo.addDeviceExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
  deviceInfo.addDeviceExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
  deviceInfo.addDeviceExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
  deviceInfo.addDeviceExtension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif

  // Creating the Vulkan instance and device
  nvvk::Context vkctx;
  vkctx.init(deviceInfo);


  InteropExample      example;
  nvgl::ContextWindow contextWindowGL;

  // Loading all OpenGL symbols
  load_GL(nvgl::ContextWindow::sysGetProcAddress);
  if(!has_GL_EXT_semaphore)
  {
    LOGE("GL_EXT_semaphore Not Available !\n");
    return 1;
  }

  example.setup(vkctx.m_instance, vkctx.m_device, vkctx.m_physicalDevice, vkctx.m_queueGCT.familyIndex);

  // Printing which GPU we are using for Vulkan
  LOGI("using %s", example.getPhysicalDevice().getProperties().deviceName);

  // Initialize the window, UI ..
  example.initUI(SAMPLE_SIZE_WIDTH, SAMPLE_SIZE_HEIGHT);

  // Prepare the example
  example.prepare(vkctx.m_queueGCT.familyIndex);


  // GLFW Callback
  example.setupGlfwCallbacks(window);
  ImGui_ImplGlfw_InitForOpenGL(window, false);

  // Main loop
  while(!glfwWindowShouldClose(window))
  {
    glfwPollEvents();
    int w, h;
    glfwGetWindowSize(window, &w, &h);
    if(w == 0 || h == 0)
      continue;

    glClearColor(0.5f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    example.animate();
    example.onWindowRefresh();

    //    contextWindowGL.swapBuffers();
    glfwSwapBuffers(window);
  }
  example.destroy();
  vkctx.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}

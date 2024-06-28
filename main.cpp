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
#include <vulkan/vulkan_core.h>

#define IMGUI_DEFINE_MATH_OPERATORS

#include "backends/imgui_impl_glfw.h"
#include "imgui/imgui_helper.h"
#include "imgui/backends/imgui_impl_gl.h"

#include "compute.hpp"
#include "nvgl/contextwindow_gl.hpp"
#include "nvgl/extensions_gl.hpp"
#include "nvpsystem.hpp"
#include "nvvkhl/appbase_vkpp.hpp"
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
    NVPSystem::exePath() + PROJECT_RELDIRECTORY,
    NVPSystem::exePath() + std::string(PROJECT_RELDIRECTORY),
};

// An array of 3 vectors which represents 3 vertices
struct Vertex
{
  glm::vec3 pos;
  glm::vec2 uv;
};

// The triangle
static std::vector<Vertex> g_vertexDataVK = {{{-1.0f, -1.0f, 0.0f}, {0, 0}},
                                             {{1.0f, -0.0f, 0.0f}, {1, 0}},
                                             {{0.0f, 1.0f, 0.0f}, {0.5, 1}}};


//--------------------------------------------------------------------------------------------------
//
//
class InteropExample : public nvvkhl::AppBase
{
public:
  void prepare(uint32_t queueIdxCompute)
  {
    m_alloc.init(m_device, m_physicalDevice);

    createShaders();   // Create the GLSL shaders
    createBufferVK();  // Create the vertex buffer

    // Initialize the Vulkan compute shader
    m_compute.setup(m_device, m_physicalDevice, m_graphicsQueueIndex, queueIdxCompute, m_alloc);
    m_compute.update({1024, 1024});  // Initial size
  }

  void destroy() override
  {
    m_device.waitIdle();
    m_bufferVk.destroy(m_alloc);
    m_compute.destroy();

    ImGui_ImplGlfw_Shutdown();
    ImGui::ShutdownGL();
    AppBase::destroy();
  }

  //--------------------------------------------------------------------------------------------------
  // Create the vertex buffer with Vulkan
  //
  void createBufferVK()
  {
    m_bufferVk.bufVk = m_alloc.createBuffer(g_vertexDataVK.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    createBufferGL(m_alloc, m_bufferVk);

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
    // Compute FPS
    static float fps = 0.f;
    {
      static float frameNumber{0};
      static auto  tStart = std::chrono::high_resolution_clock::now();
      auto         tEnd   = std::chrono::high_resolution_clock::now();
      auto         tDiff  = std::chrono::duration<float>(tEnd - tStart).count();
      frameNumber++;
      if(tDiff > 1.f)
      {
        tStart = tEnd;
        fps    = frameNumber / tDiff;
        LOGI("FPS: %f\n", fps);
        frameNumber = 0;
      }
    }

    // Input GUI
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImGuiH::dpiScaled(350, 0), ImGuiCond_FirstUseEver);
    if(ImGui::Begin("gl_vk_simple_interop"))
    {
      ImGui::Text("FPS: %.3f", fps);

      int textureWidth  = int(m_compute.m_textureTarget.imgSize.width);
      int textureHeight = int(m_compute.m_textureTarget.imgSize.height);
      // The slider max of 16384 here is somewhat arbitrary; Ctrl-click to set
      // it to a larger value. It's set to 16K so that casually sliding the
      // sliders won't run out of memory on most GPUs.
      // Use glGetIntegerv(GL_MAX_TEXTURE_SIZE) to get the maximum texture size.
      ImGui::SliderInt("Texture Width", &textureWidth, 1, 16384, "%d", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderInt("Texture Height", &textureHeight, 1, 16384, "%d", ImGuiSliderFlags_Logarithmic);
      const VkExtent2D newSize = {uint32_t(textureWidth), uint32_t(textureHeight)};
      // Did the size change?
      if(0 != memcmp(&newSize, &m_compute.m_textureTarget.imgSize, sizeof(VkExtent2D)))
      {
        // Recreate the interop texture:
        m_compute.update(newSize);
      }
    }
    ImGui::End();

    glViewport(0, 0, m_size.width, m_size.height);

    // Signal Vulkan it can use the texture
    GLenum dstLayout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glSignalSemaphoreEXT(m_compute.m_semaphores.glReady, 0, nullptr, 1, &m_compute.m_textureTarget.oglId, &dstLayout);

    // Invoke Vulkan
    m_compute.buildCommandBuffers();
    m_compute.submit();

    // Wait (on the GPU side) for the Vulkan semaphore to be signaled (finished compute)
    GLenum srcLayout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    glWaitSemaphoreEXT(m_compute.m_semaphores.glComplete, 0, nullptr, 1, &m_compute.m_textureTarget.oglId, &srcLayout);

    // Issue OpenGL commands to draw a triangle using this texture
    glBindVertexArray(m_vertexArray);
    glBindTextureUnit(0, m_compute.m_textureTarget.oglId);
    glUseProgram(m_programID);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTextureUnit(0, 0);

    // Draw GUI
    ImGui::Render();
    ImGui::RenderDrawDataGL(ImGui::GetDrawData());
    ImGui::EndFrame();
  }

  //--------------------------------------------------------------------------------------------------
  //
  //
  void animate()
  {
    static auto startTime   = std::chrono::high_resolution_clock::now();
    auto        currentTime = std::chrono::high_resolution_clock::now();
    float       t           = std::chrono::duration<float>(currentTime - startTime).count() * 0.5f;
    // Modify the buffer and upload it in the Vulkan allocated buffer
    g_vertexDataVK[0].pos.x = sinf(t);
    g_vertexDataVK[1].pos.y = cosf(t);
    g_vertexDataVK[2].pos.x = -sinf(t);

    void* mapped = m_alloc.map(m_bufferVk.bufVk);
    // This works because the buffer was created with
    // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
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
    ImGuiH::Init(width, height, this, ImGuiH::FONT_PROPORTIONAL_SCALED);
    ImGui::InitGL();
  }

  //- Override the default resize
  void onFramebufferSize(int w, int h) override
  {
    m_size.width               = w;
    m_size.height              = h;
    ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));
  }

  virtual void onMouseMotion(int x, int y) override { ImGuiH::mouse_pos(x, y); }
  virtual void onMouseButton(int button, int action, int mods) override { ImGuiH::mouse_button(button, action); }
  virtual void onMouseWheel(int delta) override { ImGuiH::mouse_wheel(delta); }
  virtual void onKeyboard(int key, int /*scancode*/, int action, int mods) override
  {
    ImGuiH::key_button(key, action, mods);
  }
  virtual void onKeyboardChar(unsigned char key) override { ImGuiH::key_char(key); }

private:
  nvvk::BufferVkGL                       m_bufferVk;
  nvvk::ExportResourceAllocatorDedicated m_alloc;

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
  NVPSystem system(PROJECT_NAME);

  nvprintSetBreakpoints(true);  // DEBUG
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
  if(!vkctx.init(deviceInfo))
  {
    LOGE("Could not initialize the Vulkan instance and device! See the above messages for more info.\n");
    return EXIT_FAILURE;
  }


  InteropExample      example;
  nvgl::ContextWindow contextWindowGL;

  // Loading all OpenGL symbols
  load_GL(nvgl::ContextWindow::sysGetProcAddress);
  if(!has_GL_EXT_semaphore)
  {
    LOGE("GL_EXT_semaphore Not Available !\n");
    return EXIT_FAILURE;
  }

  example.setup(vkctx.m_instance, vkctx.m_device, vkctx.m_physicalDevice, vkctx.m_queueGCT.familyIndex);

  // Printing which GPU we are using for Vulkan
  LOGI("using %s\n", example.getPhysicalDevice().getProperties().deviceName.data());

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

    glfwSwapBuffers(window);
  }

  example.destroy();
  vkctx.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return EXIT_SUCCESS;
}

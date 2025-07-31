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

#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION

#include <volk.h>

#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_glfw.h>
#include <imgui/backends/imgui_impl_opengl3.h>

#include <nvgl/contextwindow.hpp>
#include <nvvk/context.hpp>
#include <nvvk/resources.hpp>
#include <nvutils/logger.hpp>

#include "compute.hpp"

#include <GL/gl.h>
#include <glm/glm.hpp>

VkExtent2D g_windowSize{.width = 1200, .height = 900};

// Position and texture coordinates
struct Vertex
{
  glm::vec3 pos;
  glm::vec2 uv;
};

// The triangle's position and texture coordinates
static std::vector<Vertex> g_vertexDataVK = {{{-1.0f, -1.0f, 0.0f}, {0, 0}},
                                             {{1.0f, -0.0f, 0.0f}, {1, 0}},
                                             {{0.0f, 1.0f, 0.0f}, {0.5, 1}}};


//--------------------------------------------------------------------------------------------------
//
//
class InteropSample
{
public:
  void init(nvvk::Context& vkContext)
  {
    m_instance  = vkContext.getInstance();
    m_queueInfo = vkContext.getQueueInfo(0);
    setSize({.width = g_windowSize.width, .height = g_windowSize.height});

    // Create the allocator
    VmaAllocatorCreateInfo allocatorInfo = {
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = vkContext.getPhysicalDevice(),
        .device           = vkContext.getDevice(),
        .instance         = m_instance,
        .vulkanApiVersion = VK_API_VERSION_1_4,
    };
    m_allocator.init(allocatorInfo);

    createShaders();   // Create the GLSL shaders
    createBufferVK();  // Create the vertex buffer

    // Initialize the Vulkan compute shader
    m_compute.setup(m_allocator, m_queueInfo.familyIndex);
    m_compute.update({1024, 1024});  // Initial size
  }

  void deinit()
  {
    // Create a compute capable device queue
    NVVK_CHECK(vkQueueWaitIdle(m_queueInfo.queue));

    m_bufferVk.destroy(m_allocator);
    m_compute.destroy();
    nvvk::clearMemoryObjectManager();
    m_allocator.deinit();
  }

  //--------------------------------------------------------------------------------------------------
  // Create the vertex buffer with Vulkan
  //
  void createBufferVK()
  {
    NVVK_CHECK(m_allocator.createBufferExport(
        m_bufferVk.bufVk, g_vertexDataVK.size() * sizeof(Vertex), VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT));

    assert(m_bufferVk.bufVk.mapping != 0);

    // Import to OpenGL the Vulkan buffer
    createBufferGL(m_allocator, m_bufferVk);

    // Set up vertex array and attribute bindings for position and UV coordinates
    const int positionIndex = 0;
    const int uvIndex       = 1;
    glCreateVertexArrays(1, &m_vertexArray);
    glEnableVertexArrayAttrib(m_vertexArray, positionIndex);
    glEnableVertexArrayAttrib(m_vertexArray, uvIndex);

    glVertexArrayAttribFormat(m_vertexArray, positionIndex, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribBinding(m_vertexArray, positionIndex, 0);
    glVertexArrayAttribFormat(m_vertexArray, uvIndex, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(m_vertexArray, uvIndex, 0);

    glVertexArrayVertexBuffer(m_vertexArray, 0, m_bufferVk.oglId, 0, sizeof(Vertex));
  }

  void setSize(VkExtent2D size) { m_size = size; }

  void render()
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
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    if(ImGui::Begin("gl_vk_simple_interop"))
    {
      ImGui::Text("FPS: %.3f", fps);

      const VkExtent3D& textureSize   = m_compute.textureTarget().imageExportVk.extent;
      int               textureWidth  = int(textureSize.width);
      int               textureHeight = int(textureSize.height);

      int maxTextureSize = 16384;
      glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

      ImGui::SliderInt("Texture Width", &textureWidth, 1, maxTextureSize, "%d", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderInt("Texture Height", &textureHeight, 1, maxTextureSize, "%d", ImGuiSliderFlags_Logarithmic);
      // Did the size change?
      if(textureSize.width != textureWidth || textureSize.height != textureHeight)
      {
        // Recreate the interop texture:
        m_compute.update({uint32_t(textureWidth), uint32_t(textureHeight)});
      }
    }
    ImGui::End();

    glViewport(0, 0, m_size.width, m_size.height);

    // Signal Vulkan it can use the texture
    GLenum dstLayout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glSignalSemaphoreEXT(m_compute.semaphores().glReady, 0, nullptr, 1, &m_compute.textureTarget().oglId, &dstLayout);

    // Invoke Vulkan
    m_compute.buildCommandBuffers();
    m_compute.submit();

    // Wait (on the GPU side) for the Vulkan semaphore to be signaled (finished compute)
    GLenum srcLayout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    glWaitSemaphoreEXT(m_compute.semaphores().glComplete, 0, nullptr, 1, &m_compute.textureTarget().oglId, &srcLayout);

    // Issue OpenGL commands to draw a triangle using this texture
    glBindVertexArray(m_vertexArray);
    glBindTextureUnit(0, m_compute.textureTarget().oglId);
    glUseProgram(m_programID);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTextureUnit(0, 0);

    // Draw GUI
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGui::EndFrame();
  }

  //--------------------------------------------------------------------------------------------------
  // Update the vertex position data based on clock time
  //
  void animate() const
  {
    static auto startTime   = std::chrono::high_resolution_clock::now();
    auto        currentTime = std::chrono::high_resolution_clock::now();
    float       t           = std::chrono::duration<float>(currentTime - startTime).count() * 0.5f;

    // Modify the buffer and upload it in the Vulkan allocated buffer
    g_vertexDataVK[0].pos.x = sinf(t);
    g_vertexDataVK[1].pos.y = cosf(t);
    g_vertexDataVK[2].pos.x = -sinf(t);

    // This works because the buffer was created with
    // VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
    memcpy(reinterpret_cast<Vertex*>(m_bufferVk.bufVk.mapping), g_vertexDataVK.data(), g_vertexDataVK.size() * sizeof(Vertex));
  }

  //--------------------------------------------------------------------------------------------------
  // Create the OpenGL shaders
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
      layout(location = 0) out vec2 outUV;

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
      layout(location = 0) in vec2 inUV;
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

private:
  VkInstance                    m_instance{};
  VkExtent2D                    m_size{};
  nvvk::QueueInfo               m_queueInfo;
  nvvk::BufferVkGL              m_bufferVk;
  nvvk::ResourceAllocatorExport m_allocator;

  GLuint m_vertexArray{};  // VAO
  GLuint m_programID{};    // Shader program

  ComputeImageVk m_compute;  // Compute in Vulkan
};


int main(int argc, char** argv)
{
  if(!glfwInit())
  {
    return -1;
  }

  // This sample will work for OpenGL 4.5
  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);

  GLFWwindow* window = glfwCreateWindow(g_windowSize.width, g_windowSize.height, "Sample OpenGL/Vulkan Interop", NULL, NULL);

  nvgl::ContextWindow           contextWindow;
  nvgl::ContextWindowCreateInfo contextInfo;

  contextInfo.robust = false;
  contextInfo.core   = false;
#ifdef NDEBUG
  contextInfo.debug = false;
#else
  contextInfo.debug = true;
#endif
  contextInfo.share = NULL;
  contextInfo.major = 4;
  contextInfo.minor = 5;

  contextWindow.init(&contextInfo, window, "nvgl::ContextWindow");
  contextWindow.makeContextCurrent();
  contextWindow.swapInterval(1);

  nvvk::ContextInitInfo vkSetup{.instanceExtensions = {VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                                                       VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
                                                       VK_EXT_DEBUG_UTILS_EXTENSION_NAME},
                                .deviceExtensions   = {{VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME},
                                                       {VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME},
#ifdef WIN32
                                                     {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME},
                                                     {VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME}
#else
                                                     {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME},
                                                     {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}
#endif
                                },
                                .queues = {VK_QUEUE_GRAPHICS_BIT}};
  vkSetup.enableValidationLayers = true;

  // Creating the Vulkan instance and device
  nvvk::Context vkContext;
  if(vkContext.init(vkSetup) != VK_SUCCESS)
  {
    LOGE("Could not initialize the Vulkan instance and device! See the above messages for more info.\n");
    return EXIT_FAILURE;
  }

  glDebugMessageCallback(
      [](GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) mutable {
        if(type == GL_DEBUG_TYPE_ERROR)
        {
          LOGE("OpenGL Error: %s\n", message);
        }
      },
      nullptr);

  InteropSample interopSample;
  interopSample.init(vkContext);

  glfwSetWindowUserPointer(window, &interopSample);

  glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) mutable {
    InteropSample* interopSample = (InteropSample*)glfwGetWindowUserPointer(window);
    interopSample->setSize({.width = (uint32_t)width, .height = (uint32_t)height});
    ImGui::GetIO().DisplaySize = ImVec2(float(width), float(height));
  });

  ImGui::CreateContext();
  auto& imgui_io       = ImGui::GetIO();
  imgui_io.IniFilename = nullptr;
  imgui_io.UserData    = window;
  imgui_io.DisplaySize = ImVec2(float(g_windowSize.width), float(g_windowSize.height));
  imgui_io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;  // Disable keyboard controls

  ImGui_ImplOpenGL3_Init();
  ImGui_ImplGlfw_InitForOpenGL(window, true);

  while(!glfwWindowShouldClose(window))
  {
    ImGui_ImplOpenGL3_NewFrame();

    glClearColor(0.0, 1.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    interopSample.animate();
    interopSample.render();

    contextWindow.swapBuffers();

    glfwPollEvents();
  }

  ImGui_ImplGlfw_Shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui::DestroyContext(nullptr);

  interopSample.deinit();
  vkContext.deinit();
  contextWindow.deinit();
  glfwDestroyWindow(window);

  glfwTerminate();

  return 0;
}

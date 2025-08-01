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

#version 450


layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform image2D resultImage;


layout(push_constant) uniform PushConstants
{
  float iTime;
}
pushc;

const float M_PI   = 3.14159265359;
const vec2  center = vec2(0.5, 0.3);

void main()
{
  const ivec2 iResolution = imageSize(resultImage);
  if(gl_GlobalInvocationID.x >= iResolution.x || gl_GlobalInvocationID.y >= iResolution.y)
    return;
  const vec2  fragCoord = vec2(gl_GlobalInvocationID);
  const float iTime     = pushc.iTime;
  vec4        fragColor = vec4(0);

  // Center
  vec2 uv = fragCoord / vec2(iResolution);
  uv -= center;
  uv *= 5.0;

  float d = abs(fract(dot(uv, uv) - iTime * 0.5) - 0.5) + 0.3;
  float a = abs(fract(atan(uv.x, uv.y) / (M_PI * 1.75) * 3.) - 0.5) + 0.2;

  vec3 col = vec4(abs(uv), 0.5 + 0.5 * sin(iTime), 1.0).xyz;

  if(a < d)
  {
    fragColor = vec4(d * col.gbr, 1.0);
  }
  else
  {
    fragColor = vec4(a * col, 1.0);
  }

  imageStore(resultImage, ivec2(gl_GlobalInvocationID.xy), fragColor);
}

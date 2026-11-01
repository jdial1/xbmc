/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "guishader_common.hlsl"

// CGUITexture quads, clipped, transformed and rounded on the GPU like the CPU path in
// CGUITexture::Render(const Segment&) (port of the GLES m_snap path in gles_shader.vert).
// corner is this corner and opposite the opposite corner of the quad, each as a fraction of the
// texture's rectangle quadRect (x, y, width, height) in xy plus an offset in zw. tex.xy / diffuse.xy
// hold the texture coordinates at this corner and .zw at the opposite one; texSwap flags textures
// whose coordinates run along the other axis.
struct VS_QUAD_INPUT
{
  float4 corner : POSITION;
  float4 opposite : TEXCOORD2;
  float4 tex : TEXCOORD0;
  float4 diffuse : TEXCOORD1;
};

cbuffer cbQuad : register(b2)
{
  float4 quadRect;
  float4 quadClip; // x1, y1, x2, y2
  float4 guiRow0;  // GUI (final) transform, rows of the 3x4 matrix
  float4 guiRow1;
  float4 guiRow2;
  float4 quadColor;
  float2 texSwap;
  float snap;      // 1 = round to whole pixels, 2 = keep subpixel positions (moving texture)
  float quadPad;
};

PS_INPUT VS(VS_QUAD_INPUT input)
{
  PS_INPUT output = (PS_INPUT)0;

  float2 p = quadRect.xy + input.corner.xy * quadRect.zw + input.corner.zw;
  float2 q = quadRect.xy + input.opposite.xy * quadRect.zw + input.opposite.zw;
  float2 pc = clamp(p, quadClip.xy, quadClip.zw);
  float2 qc = clamp(q, quadClip.xy, quadClip.zw);
  float2 t = (pc - p) / (q - p);
  output.tex = input.tex.xy + (texSwap.x > 0.0 ? t.yx : t) * (input.tex.zw - input.tex.xy);
  output.tex2 = input.diffuse.xy + (texSwap.y > 0.0 ? t.yx : t) * (input.diffuse.zw - input.diffuse.xy);

  // A bottom corner is pushed one pixel away from the opposite corner if no pixel centre lies
  // between them on that axis, so that thin quads never vanish.
  float push = (p.y > q.y && all(pc != qc)) ? 1.0 : 0.0;
  float4 pc4 = float4(pc, 0.0, 1.0);
  float4 qc4 = float4(qc, 0.0, 1.0);
  float4 pos = float4(dot(guiRow0, pc4), dot(guiRow1, pc4), dot(guiRow2, pc4), 1.0);
  float2 opp = float2(dot(guiRow0, qc4), dot(guiRow1, qc4));
  if (snap < 1.5)
  {
    pos.xy = floor(pos.xy + 0.5);
    opp = floor(opp + 0.5);
  }
  pos.z = floor(pos.z + 0.5);
  pos.xy += float2(ceil(pos.xy - 0.5) == ceil(opp - 0.5)) * push;

  output.pos = mul(pos, worldViewProj);
  output.pos.z = depth * output.pos.w;
  output.color = quadColor;
  return output;
}

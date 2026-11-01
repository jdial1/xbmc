/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "guishader_common.hlsl"

// Batched CGUITexture quads: guishader_quad_vert.hlsl with its per-texture constants (cbQuad) moved
// into per-instance data, so that consecutive textures with the same pixel shader and views share
// one instanced draw. Built for vs_4_0_level_9_3 (instancing).
// corner (per vertex) selects this vertex's edges, 0 = x1 or y1 and 1 = x2 or y2 of the instance's
// rectangles (x1, y1, x2, y2 each); the opposite corner takes the other ones.
struct VS_BATCH_INPUT
{
  float2 corner : POSITION;
  float4 anchor : TEXCOORD0;   // edges as fractions of quadRect
  float4 offset : TEXCOORD1;   // added to the anchored edges
  float4 tex : TEXCOORD2;
  float4 diffuse : TEXCOORD3;
  float4 quadRect : TEXCOORD4; // x, y, width, height of the texture rectangle
  float4 quadClip : TEXCOORD5; // x1, y1, x2, y2
  float4 guiRow0 : TEXCOORD6;  // GUI (final) transform, rows of the 3x4 matrix
  float4 guiRow1 : TEXCOORD7;
  float4 guiRow2 : TEXCOORD8;
  float4 quadColor : TEXCOORD9;
  float4 flags : TEXCOORD10;   // texSwap.xy, snap (1 = whole pixels, 2 = subpixel), depth (0 far, 1 near)
};

// the x1/y1 (select 0) or x2/y2 (select 1) components of r
float2 Edges(float4 r, float2 select)
{
  return float2(select.x > 0.5 ? r.z : r.x, select.y > 0.5 ? r.w : r.y);
}

PS_INPUT VS(VS_BATCH_INPUT input)
{
  PS_INPUT output = (PS_INPUT)0;

  float2 s = input.corner;
  float2 o = 1.0 - s;
  // swapped texture coordinates run along the other axis (TexCoordCorners in GUITextureD3D.cpp)
  float2 texSelect = input.flags.x > 0.0 ? s.yx : s;
  float2 diffuseSelect = input.flags.y > 0.0 ? s.yx : s;
  float2 tex = Edges(input.tex, texSelect);
  float2 texOpposite = Edges(input.tex, 1.0 - texSelect);
  float2 diffuse = Edges(input.diffuse, diffuseSelect);
  float2 diffuseOpposite = Edges(input.diffuse, 1.0 - diffuseSelect);

  // from here as guishader_quad_vert.hlsl
  float2 p = input.quadRect.xy + Edges(input.anchor, s) * input.quadRect.zw + Edges(input.offset, s);
  float2 q = input.quadRect.xy + Edges(input.anchor, o) * input.quadRect.zw + Edges(input.offset, o);
  float2 pc = clamp(p, input.quadClip.xy, input.quadClip.zw);
  float2 qc = clamp(q, input.quadClip.xy, input.quadClip.zw);
  float2 t = (pc - p) / (q - p);
  output.tex = tex + (input.flags.x > 0.0 ? t.yx : t) * (texOpposite - tex);
  output.tex2 = diffuse + (input.flags.y > 0.0 ? t.yx : t) * (diffuseOpposite - diffuse);

  // A bottom corner is pushed one pixel away from the opposite corner if no pixel centre lies
  // between them on that axis, so that thin quads never vanish.
  float push = (p.y > q.y && all(pc != qc)) ? 1.0 : 0.0;
  float4 pc4 = float4(pc, 0.0, 1.0);
  float4 qc4 = float4(qc, 0.0, 1.0);
  float4 pos = float4(dot(input.guiRow0, pc4), dot(input.guiRow1, pc4), dot(input.guiRow2, pc4), 1.0);
  float2 opp = float2(dot(input.guiRow0, qc4), dot(input.guiRow1, qc4));
  if (input.flags.z < 1.5)
  {
    pos.xy = floor(pos.xy + 0.5);
    opp = floor(opp + 0.5);
  }
  pos.z = floor(pos.z + 0.5);
  pos.xy += float2(ceil(pos.xy - 0.5) == ceil(opp - 0.5)) * push;

  output.pos = mul(pos, worldViewProj);
  // the control depth, as the cbWorld depth of the per-texture path (depth testing and focus rely on it)
  output.pos.z = input.flags.w * output.pos.w;
  output.color = input.quadColor;
  return output;
}

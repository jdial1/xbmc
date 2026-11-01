/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "guishader_common.hlsl"

// Batched images and text (guishader_quad_batch_vert.hlsl): guishader_texture.hlsl for both, the
// image from t0 (an atlas page) and the glyphs from t1 (a font's glyph cache). The fonts draw with
// SHADER_METHOD_RENDER_FONT_RGBA, which is guishader_texture.hlsl as well. Both textures are sampled
// and one is selected, which needs no flow control (ps_4_0_level_9_1). Glyphs are clipped here to
// the rectangle that was their scissor rectangle.
struct PS_BATCH_INPUT
{
  float4 pos : SV_POSITION;
  float4 color : COLOR0;
  float2 tex : TEXCOORD0;
  float2 tex2 : TEXCOORD1;
  float4 clipDist : TEXCOORD2;
  float glyph : TEXCOORD3;
};

Texture2D texMain : register(t0);
Texture2D texGlyphs : register(t1);

float4 PS(PS_BATCH_INPUT input) : SV_TARGET
{
  clip(input.clipDist);
  float4 image = texMain.Sample(LinearSampler, input.tex);
  float4 glyph = texGlyphs.Sample(LinearSampler, input.tex);
  return tonemapHDR(adjustColorRange(input.color * (input.glyph > 0.5 ? glyph : image)));
}

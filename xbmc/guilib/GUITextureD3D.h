/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GUIShaderDX.h"
#include "GUITexture.h"
#include "utils/ColorUtils.h"

#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

class CGUITextureD3D : public CGUITexture
{
public:
  static void Register();
  static CGUITexture* CreateTexture(
      float posX, float posY, float width, float height, const CTextureInfo& texture);

  static void DrawQuad(const CRect& coords,
                       KODI::UTILS::COLOR::Color color,
                       CTexture* texture = nullptr,
                       const CRect* texCoords = nullptr,
                       const float depth = 1.0,
                       const bool blending = true);

  CGUITextureD3D(float posX, float posY, float width, float height, const CTextureInfo& texture);
  ~CGUITextureD3D() override = default;

  CGUITextureD3D* Clone() const override;

protected:
  void Begin(KODI::UTILS::COLOR::Color color);
  void Draw(float *x, float *y, float *z, const CRect &texture, const CRect &diffuse, int orientation);
  void End();
  bool DrawQuads(const std::vector<Quad>& quads, unsigned int version, const CRect& rect) override;
  void Free() override;

private:
  CGUITextureD3D(const CGUITextureD3D& texture) = default;

  unsigned int ShaderMethod() const;
  bool QueueQuads(CGUIShaderDX& shader,
                  const std::vector<Quad>& quads,
                  unsigned int version,
                  const CRect& rect);

  KODI::UTILS::COLOR::Color m_col;

  // immutable GPU copy of the quads, rebuilt when their version (or the device) changes; clones share it
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_quadBuffer;
  unsigned int m_quadBufferVersion{0};
  unsigned int m_quadCount{0};

  // the quads in the layout of the batched path, rebuilt when their version changes
  std::vector<CGUIShaderDX::QuadGeometry> m_batchQuads;
  unsigned int m_batchQuadsVersion{0};
  // bounds (x1, y1, x2, y2) of their texture and diffuse coordinates, and the quads mapped into the
  // atlas pages of the textures
  DirectX::XMFLOAT4 m_batchTextureBounds{};
  DirectX::XMFLOAT4 m_batchDiffuseBounds{};
  std::vector<CGUIShaderDX::QuadGeometry> m_atlasQuads;
};


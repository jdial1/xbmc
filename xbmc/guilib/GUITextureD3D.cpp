/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUITextureD3D.h"

#include "D3DResource.h"
#include "GUIShaderDX.h"
#include "TextureDX.h"
#include "ServiceBroker.h"
#include "rendering/dx/DeviceResources.h"
#include "rendering/dx/RenderContext.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <array>
#include <limits>

#include <DirectXMath.h>

using namespace DirectX;

void CGUITextureD3D::Register()
{
  CGUITexture::Register(CGUITextureD3D::CreateTexture, CGUITextureD3D::DrawQuad);
}

CGUITexture* CGUITextureD3D::CreateTexture(
    float posX, float posY, float width, float height, const CTextureInfo& texture)
{
  return new CGUITextureD3D(posX, posY, width, height, texture);
}

CGUITextureD3D::CGUITextureD3D(
    float posX, float posY, float width, float height, const CTextureInfo& texture)
  : CGUITexture(posX, posY, width, height, texture)
{
}

CGUITextureD3D* CGUITextureD3D::Clone() const
{
  return new CGUITextureD3D(*this);
}

void CGUITextureD3D::Begin(KODI::UTILS::COLOR::Color color)
{
  CTexture* texture = m_texture.m_textures[m_currentFrame].get();
  texture->LoadToGPU();

  if (m_diffuse.size())
	  m_diffuse.m_textures[0]->LoadToGPU();

  m_col = color;

  DX::Windowing()->SetAlphaBlendEnable(true);
}

void CGUITextureD3D::End()
{
}

void CGUITextureD3D::Draw(float *x, float *y, float *z, const CRect &texture, const CRect &diffuse, int orientation)
{
  XMFLOAT4 xcolor;
  CD3DHelper::XMStoreColor(&xcolor, m_col);

  Vertex verts[4];
  verts[0].pos.x = x[0]; verts[0].pos.y = y[0]; verts[0].pos.z = z[0];
  verts[0].texCoord.x = texture.x1;   verts[0].texCoord.y = texture.y1;
  verts[0].texCoord2.x = diffuse.x1;  verts[0].texCoord2.y = diffuse.y1;
  verts[0].color = xcolor;

  verts[1].pos.x = x[1]; verts[1].pos.y = y[1]; verts[1].pos.z = z[1];
  if (orientation & 4)
  {
    verts[1].texCoord.x = texture.x1;
    verts[1].texCoord.y = texture.y2;
  }
  else
  {
    verts[1].texCoord.x = texture.x2;
    verts[1].texCoord.y = texture.y1;
  }
  if (m_info.orientation & 4)
  {
    verts[1].texCoord2.x = diffuse.x1;
    verts[1].texCoord2.y = diffuse.y2;
  }
  else
  {
    verts[1].texCoord2.x = diffuse.x2;
    verts[1].texCoord2.y = diffuse.y1;
  }
  verts[1].color = xcolor;

  verts[2].pos.x = x[2]; verts[2].pos.y = y[2]; verts[2].pos.z = z[2];
  verts[2].texCoord.x = texture.x2;   verts[2].texCoord.y = texture.y2;
  verts[2].texCoord2.x = diffuse.x2;  verts[2].texCoord2.y = diffuse.y2;
  verts[2].color = xcolor;

  verts[3].pos.x = x[3]; verts[3].pos.y = y[3]; verts[3].pos.z = z[3];
  if (orientation & 4)
  {
    verts[3].texCoord.x = texture.x2;
    verts[3].texCoord.y = texture.y1;
  }
  else
  {
    verts[3].texCoord.x = texture.x1;
    verts[3].texCoord.y = texture.y2;
  }
  if (m_info.orientation & 4)
  {
    verts[3].texCoord2.x = diffuse.x2;
    verts[3].texCoord2.y = diffuse.y1;
  }
  else
  {
    verts[3].texCoord2.x = diffuse.x1;
    verts[3].texCoord2.y = diffuse.y2;
  }
  verts[3].color = xcolor;

  CDXTexture* tex = static_cast<CDXTexture*>(m_texture.m_textures[m_currentFrame].get());
  CGUIShaderDX* pGUIShader = DX::Windowing()->GetGUIShader();

  pGUIShader->Begin(ShaderMethod());

  if (m_diffuse.size())
  {
    CDXTexture* diff = static_cast<CDXTexture*>(m_diffuse.m_textures[0].get());
    ID3D11ShaderResourceView* resource[] = { tex->GetShaderResource(), diff->GetShaderResource() };
    pGUIShader->SetShaderViews(ARRAYSIZE(resource), resource);
  }
  else
  {
    ID3D11ShaderResourceView* resource = tex->GetShaderResource();
    pGUIShader->SetShaderViews(1, &resource);
  }
  pGUIShader->SetDepth(m_depth);
  pGUIShader->DrawQuad(verts[0], verts[1], verts[2], verts[3]);
}

unsigned int CGUITextureD3D::ShaderMethod() const
{
  return m_diffuse.size() > 0 ? (m_scalingMethod == TEXTURE_SCALING::NEAREST
                                     ? SHADER_METHOD_RENDER_MULTI_TEXTURE_BLEND_NEAREST
                                     : SHADER_METHOD_RENDER_MULTI_TEXTURE_BLEND)
                              : (m_scalingMethod == TEXTURE_SCALING::NEAREST
                                     ? SHADER_METHOD_RENDER_TEXTURE_BLEND_NEAREST
                                     : SHADER_METHOD_RENDER_TEXTURE_BLEND);
}

namespace
{
std::array<CPoint, 4> TexCoordCorners(const CRect& rect, bool swapXY)
{
  if (swapXY)
    return {{{rect.x1, rect.y1}, {rect.x1, rect.y2}, {rect.x2, rect.y2}, {rect.x2, rect.y1}}};

  return {{{rect.x1, rect.y1}, {rect.x2, rect.y1}, {rect.x2, rect.y2}, {rect.x1, rect.y2}}};
}
} // namespace

void CGUITextureD3D::Free()
{
  m_quadBuffer = nullptr;
  m_quadBufferVersion = 0;
  m_quadCount = 0;
}

bool CGUITextureD3D::DrawQuads(const std::vector<Quad>& quads,
                               unsigned int version,
                               const CRect& rect)
{
  CGUIShaderDX* pGUIShader = DX::Windowing()->GetGUIShader();
  ID3D11Device* device = DX::DeviceResources::Get()->GetD3DDevice();
  if (!pGUIShader || !pGUIShader->HasQuadPath() || !device ||
      quads.size() > CGUIShaderDX::MAX_GUI_QUADS)
    return false;

  // a buffer holds a reference to the device it was created on, so this also catches device loss
  Microsoft::WRL::ComPtr<ID3D11Device> bufferDevice;
  if (m_quadBuffer)
    m_quadBuffer->GetDevice(bufferDevice.GetAddressOf());
  if (m_quadBufferVersion != version || (m_quadBuffer && bufferDevice.Get() != device))
  {
    std::vector<CGUIShaderDX::QuadVertex> vertices;
    vertices.reserve(quads.size() * 4);
    for (const Quad& quad : quads)
    {
      const CRect& a = quad.anchor;
      const CRect& o = quad.offset;
      const std::array<std::array<float, 4>, 4> corners{{{a.x1, a.y1, o.x1, o.y1},
                                                         {a.x2, a.y1, o.x2, o.y1},
                                                         {a.x2, a.y2, o.x2, o.y2},
                                                         {a.x1, a.y2, o.x1, o.y2}}};
      const std::array<CPoint, 4> texCoords = TexCoordCorners(quad.texture, GetOrientation() & 4);
      const std::array<CPoint, 4> diffuseCoords =
          TexCoordCorners(quad.diffuse, m_info.orientation & 4);
      for (std::size_t i = 0; i < 4; i++)
      {
        const std::size_t opposite = (i + 2) % 4;
        vertices.push_back({{corners[i][0], corners[i][1], corners[i][2], corners[i][3]},
                            {corners[opposite][0], corners[opposite][1], corners[opposite][2],
                             corners[opposite][3]},
                            {texCoords[i].x, texCoords[i].y, texCoords[opposite].x,
                             texCoords[opposite].y},
                            {diffuseCoords[i].x, diffuseCoords[i].y, diffuseCoords[opposite].x,
                             diffuseCoords[opposite].y}});
      }
    }

    m_quadBuffer = nullptr;
    m_quadCount = 0;
    if (!vertices.empty())
    {
      CD3D11_BUFFER_DESC desc(static_cast<UINT>(vertices.size() * sizeof(vertices[0])),
                              D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE);
      D3D11_SUBRESOURCE_DATA data = {vertices.data(), 0, 0};
      if (FAILED(device->CreateBuffer(&desc, &data, m_quadBuffer.ReleaseAndGetAddressOf())))
        return false;
    }
    m_quadBufferVersion = version;
    m_quadCount = static_cast<unsigned int>(quads.size());
  }

  if (m_quadCount == 0)
    return true;

  CGraphicContext& context = CServiceBroker::GetWinSystem()->GetGfxContext();
  constexpr float unbounded = std::numeric_limits<float>::max();
  const CRect clip = context.HasClipRegion() ? context.GetClipRegion()
                                             : CRect(-unbounded, -unbounded, unbounded, unbounded);
  const TransformMatrix& gui = context.GetGUIMatrix();

  CGUIShaderDX::QuadConstants constants;
  constants.rect = {rect.x1, rect.y1, rect.Width(), rect.Height()};
  constants.clip = {clip.x1, clip.y1, clip.x2, clip.y2};
  for (int row = 0; row < 3; row++)
    constants.gui[row] = {gui.m[row][0], gui.m[row][1], gui.m[row][2], gui.m[row][3]};
  CD3DHelper::XMStoreColor(&constants.color, m_col);
  constants.texSwap = {(GetOrientation() & 4) ? 1.0f : 0.0f, (m_info.orientation & 4) ? 1.0f : 0.0f};
  constants.snap = m_moving ? 2.0f : 1.0f;
  constants.pad = 0.0f;

  CDXTexture* tex = static_cast<CDXTexture*>(m_texture.m_textures[m_currentFrame].get());
  pGUIShader->Begin(ShaderMethod());
  if (m_diffuse.size())
  {
    CDXTexture* diff = static_cast<CDXTexture*>(m_diffuse.m_textures[0].get());
    ID3D11ShaderResourceView* resource[] = {tex->GetShaderResource(), diff->GetShaderResource()};
    pGUIShader->SetShaderViews(ARRAYSIZE(resource), resource);
  }
  else
  {
    ID3D11ShaderResourceView* resource = tex->GetShaderResource();
    pGUIShader->SetShaderViews(1, &resource);
  }
  pGUIShader->SetDepth(m_depth);
  pGUIShader->DrawGUIQuads(m_quadBuffer.Get(), m_quadCount, constants);
  return true;
}

void CGUITextureD3D::DrawQuad(const CRect& rect,
                              KODI::UTILS::COLOR::Color color,
                              CTexture* texture,
                              const CRect* texCoords,
                              const float depth,
                              const bool blending)
{
  unsigned numViews = 0;
  ID3D11ShaderResourceView* views = nullptr;

  if (texture)
  {
    texture->LoadToGPU();
    numViews = 1;
    views = ((CDXTexture*)texture)->GetShaderResource();
  }

  CD3DTexture::DrawQuad(rect, color, numViews, &views, texCoords,
                        texture ? SHADER_METHOD_RENDER_TEXTURE_BLEND : SHADER_METHOD_RENDER_DEFAULT,
                        depth);
}

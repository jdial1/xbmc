/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "D3DResource.h"
#include "Texture.h"
#include "TextureAtlasDX.h"

/************************************************************************/
/*    CDXTexture                                                       */
/************************************************************************/
class CDXTexture : public CTexture
{
public:
  CDXTexture(unsigned int width = 0, unsigned int height = 0, XB_FMT format = XB_FMT_UNKNOWN);
  virtual ~CDXTexture();

  void CreateTextureObject();
  void DestroyTextureObject();
  virtual void LoadToGPU();
  bool LoadToGPUOffThread() override;
  void BindToUnit(unsigned int unit);

  ID3D11Texture2D* GetTextureObject()
  {
    if (m_atlasRegion && !m_texture.Get())
      CopyFromAtlas();
    return m_texture.Get();
  };

  ID3D11ShaderResourceView* GetShaderResource()
  {
    if (m_atlasRegion && !m_texture.Get())
      CopyFromAtlas();
    return m_texture.GetShaderResource();
  };

  /*!
   * \brief The texture's texels lie in an atlas page (see CTextureAtlasDX). Batched GUI quads draw it
   * from there; GetShaderResource() and GetTextureObject() still work, they give the texture a copy
   * of its own on first use.
   */
  bool IsInAtlas() const { return static_cast<bool>(m_atlasRegion); }
  const CTextureAtlasDX::Region& GetAtlasRegion() const { return m_atlasRegion; }

private:
  bool LoadToAtlas();
  void CopyFromAtlas();

  CD3DTexture m_texture;
  CTextureAtlasDX::Region m_atlasRegion;
  DXGI_FORMAT GetFormat();
};

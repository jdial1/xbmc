/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TextureDX.h"

#include "rendering/dx/DeviceResources.h"

#include "utils/MemUtils.h"
#include "utils/log.h"

#include <memory>

std::unique_ptr<CTexture> CTexture::CreateTexture(unsigned int width,
                                                  unsigned int height,
                                                  XB_FMT format)
{
  return std::make_unique<CDXTexture>(width, height, format);
}

CDXTexture::CDXTexture(unsigned int width, unsigned int height, XB_FMT format)
  : CTexture(width, height, format)
{
}

CDXTexture::~CDXTexture()
{
  DestroyTextureObject();
}

void CDXTexture::CreateTextureObject()
{
  m_texture.Create(m_textureWidth, m_textureHeight, 1, D3D11_USAGE_DEFAULT, GetFormat());
}

DXGI_FORMAT CDXTexture::GetFormat()
{
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // DXGI_FORMAT_UNKNOWN

  switch (m_format)
  {
  case XB_FMT_DXT1:
    format = DXGI_FORMAT_BC1_UNORM; // D3DFMT_DXT1 -> DXGI_FORMAT_BC1_UNORM & DXGI_FORMAT_BC1_UNORM_SRGB
    break;
  case XB_FMT_DXT3:
    format = DXGI_FORMAT_BC2_UNORM; // D3DFMT_DXT3 -> DXGI_FORMAT_BC2_UNORM & DXGI_FORMAT_BC2_UNORM_SRGB
    break;
  case XB_FMT_DXT5:
  case XB_FMT_DXT5_YCoCg:
    format = DXGI_FORMAT_BC3_UNORM; // XB_FMT_DXT5 -> DXGI_FORMAT_BC3_UNORM & DXGI_FORMAT_BC3_UNORM_SRGB
    break;
  case XB_FMT_RGB8:
  case XB_FMT_A8R8G8B8:
    format = DXGI_FORMAT_B8G8R8A8_UNORM; // D3DFMT_A8R8G8B8 -> DXGI_FORMAT_B8G8R8A8_UNORM | DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    break;
  case XB_FMT_A8:
    format = DXGI_FORMAT_R8_UNORM; // XB_FMT_A8 -> DXGI_FORMAT_A8_UNORM
    break;
  }

  return format;
}

void CDXTexture::DestroyTextureObject()
{
  m_texture.Release();
  CTextureAtlasDX::Get().Free(m_atlasRegion);
}

bool CDXTexture::LoadToAtlas()
{
  // the atlas pages are BGRA8 without mipmaps
  if (m_format != XB_FMT_A8R8G8B8 || IsMipmapped() ||
      !CTextureAtlasDX::Get().Accepts(m_textureWidth, m_textureHeight))
    return false;

  m_atlasRegion =
      CTextureAtlasDX::Get().Insert(m_pixels, GetPitch(), m_textureWidth, m_textureHeight);
  return static_cast<bool>(m_atlasRegion);
}

void CDXTexture::CopyFromAtlas()
{
  ID3D11Texture2D* page = CTextureAtlasDX::Get().GetTexture(m_atlasRegion);
  ID3D11DeviceContext* pContext = DX::DeviceResources::Get()->GetD3DContext();
  if (!page || !pContext ||
      !m_texture.Create(m_textureWidth, m_textureHeight, 1, D3D11_USAGE_DEFAULT, GetFormat()))
    return;

  // on the GUI context, in order with the draws that use it
  const CD3D11_BOX box(m_atlasRegion.x, m_atlasRegion.y, 0, m_atlasRegion.x + m_textureWidth,
                       m_atlasRegion.y + m_textureHeight, 1);
  pContext->CopySubresourceRegion(m_texture.Get(), 0, 0, 0, 0, page, 0, &box);
  CTextureAtlasDX::Get().CountCopy();
}

bool CDXTexture::LoadToGPUOffThread()
{
  // only the device path: CreateTexture2D with initial data. Mipmapped textures need the context
  // (UpdateSubresource + GenerateMips) and RGB8 needs a CPU map, so those stay on the GUI thread.
  if (!m_pixels || m_texture.Get() || m_format == XB_FMT_RGB8 || IsMipmapped() ||
      m_atlasCandidate || m_atlasRegion)
    return false;

  auto deviceLock = DX::DeviceResources::Get()->TryLockDeviceForWorker();
  if (!deviceLock.owns_lock() || !DX::DeviceResources::Get()->HasValidDevice())
    return false;

  m_texture.Create(m_textureWidth, m_textureHeight, 1, D3D11_USAGE_DEFAULT, GetFormat(), m_pixels,
                   GetPitch());
  if (m_texture.Get() == nullptr)
    return false; // the GUI thread will try again (and log)

  if (!m_bCacheMemory)
  {
    KODI::MEMORY::AlignedFree(m_pixels);
    m_pixels = nullptr;
  }
  m_loadedToGPU = true;
  return true;
}

void CDXTexture::LoadToGPU()
{
  if (!m_pixels)
  {
    // nothing to load - probably same image (no change)
    return;
  }

  if (m_atlasRegion)
  {
    // uploaded again: from now on the texture has a texture of its own
    CTextureAtlasDX::Get().Free(m_atlasRegion);
    m_atlasCandidate = false;
  }
  else if (m_atlasCandidate && !m_texture.Get() && LoadToAtlas())
  {
    if (!m_bCacheMemory)
    {
      KODI::MEMORY::AlignedFree(m_pixels);
      m_pixels = nullptr;
    }
    m_loadedToGPU = true;
    return;
  }

  bool needUpdate = true;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
  if (m_format == XB_FMT_RGB8)
    usage = D3D11_USAGE_DYNAMIC; // fallback to dynamic to allow CPU write to texture

  if (m_texture.Get() == nullptr)
  {
    // creates texture with initial data
    if (m_format != XB_FMT_RGB8)
    {
      // this is faster way to create texture with initial data instead of create empty and then copy to it
      m_texture.Create(m_textureWidth, m_textureHeight, IsMipmapped() ? 0 : 1, usage, GetFormat(), m_pixels, GetPitch());
      if (m_texture.Get() != nullptr)
        needUpdate = false;
    }
    else
      m_texture.Create(m_textureWidth, m_textureHeight, IsMipmapped() ? 0 : 1, usage, GetFormat());

    if (m_texture.Get() == nullptr)
    {
      CLog::Log(LOGDEBUG, "CDXTexture::CDXTexture: Error creating new texture for size {} x {}.",
                m_textureWidth, m_textureHeight);
      return;
    }
  }
  else
  {
    // need to update texture, check usage first
    D3D11_TEXTURE2D_DESC texDesc;
    m_texture.GetDesc(&texDesc);
    usage = texDesc.Usage;

    // if usage is not dynamic re-create texture with dynamic usage for future updates
    if (usage != D3D11_USAGE_DYNAMIC && usage != D3D11_USAGE_STAGING)
    {
      m_texture.Release();
      usage = D3D11_USAGE_DYNAMIC;

      m_texture.Create(m_textureWidth, m_textureHeight, IsMipmapped() ? 0 : 1, usage, GetFormat(), m_pixels, GetPitch());
      if (m_texture.Get() == nullptr)
      {
        CLog::Log(LOGDEBUG, "CDXTexture::CDXTexture: Error creating new texture for size {} x {}.",
                  m_textureWidth, m_textureHeight);
        return;
      }

      needUpdate = false;
    }
  }

  if (needUpdate)
  {
    D3D11_MAP mapType = (usage == D3D11_USAGE_STAGING) ? D3D11_MAP_WRITE : D3D11_MAP_WRITE_DISCARD;
    D3D11_MAPPED_SUBRESOURCE lr;
    if (m_texture.LockRect(0, &lr, mapType))
    {
      unsigned char *dst = (unsigned char *)lr.pData;
      unsigned char *src = m_pixels;
      unsigned int dstPitch = lr.RowPitch;
      unsigned int srcPitch = GetPitch();
      unsigned int minPitch = std::min(srcPitch, dstPitch);

      unsigned int rows = GetRows();
      if (m_format == XB_FMT_RGB8)
      {
        for (unsigned int y = 0; y < rows; y++)
        {
          unsigned char *dst2 = dst;
          unsigned char *src2 = src;
          for (unsigned int x = 0; x < srcPitch / 3; x++, dst2 += 4, src2 += 3)
          {
            dst2[0] = src2[2];
            dst2[1] = src2[1];
            dst2[2] = src2[0];
            dst2[3] = 0xff;
          }
          src += srcPitch;
          dst += dstPitch;
        }
      }
      else if (srcPitch == dstPitch)
      {
        memcpy(dst, src, srcPitch * rows);
      }
      else
      {
        for (unsigned int y = 0; y < rows; y++)
        {
          memcpy(dst, src, minPitch);
          src += srcPitch;
          dst += dstPitch;
        }
      }
    }
    else
    {
      CLog::LogF(LOGERROR, "failed to lock texture.");
    }
    m_texture.UnlockRect(0);
    if (usage != D3D11_USAGE_STAGING && IsMipmapped())
      m_texture.GenerateMipmaps();
  }

  if (!m_bCacheMemory)
  {
    KODI::MEMORY::AlignedFree(m_pixels);
    m_pixels = nullptr;
  }

  m_loadedToGPU = true;
}

void CDXTexture::BindToUnit(unsigned int unit)
{
}

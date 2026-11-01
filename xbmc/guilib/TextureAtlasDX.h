/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "D3DResource.h"
#include "threads/CriticalSection.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <DirectXMath.h>

/*!
 \ingroup textures
 \brief Small GUI textures packed into shared BGRA8 pages, so that the batched quads of different
 textures share one view and one draw (CGUIShaderDX::QueueGUIQuads).

 Each region is surrounded by a 1-texel gutter that repeats its edge texels: samples inside the
 texture's [0, 1] coordinates, and up to half a texel beyond, read what the CLAMP sampler reads from
 a texture of its own. A freed region is quarantined for QUARANTINE_FRAMES frames, as command lists
 recorded before the free may still sample it; an empty page is released after that.
 */
class CTextureAtlasDX
{
  struct Page;

public:
  //! A texture's place in a page: its texels, without the gutter around them
  struct Region
  {
    Page* page = nullptr;
    unsigned int x = 0;
    unsigned int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    // texture coordinates (0..1 over width and height) to page coordinates
    double scaleU = 1.0;
    double scaleV = 1.0;
    double offsetU = 0.0;
    double offsetV = 0.0;

    explicit operator bool() const { return page != nullptr; }
  };

  struct Stats
  {
    unsigned int pages = 0;
    uint64_t bytes = 0;
    uint64_t usedTexels = 0; // regions with their gutters
    unsigned int regions = 0;
    uint64_t fallbacks = 0; // candidates that got a texture of their own (too large, no space, failed)
    uint64_t copies = 0; // atlas textures that needed a texture of their own (GetShaderResource)
    uint64_t rangeFallbacks = 0; // draws of atlas textures from their own texture (coordinates)
  };

  static CTextureAtlasDX& Get();

  /*!
   * \brief Whether a texture of this size goes into the atlas: <gui><atlastextures> is on, the GUI
   * quads are batched (an atlas texture drawn per texture would gain nothing) and both sides are at
   * most <gui><atlasmaxsize>. A size that is too large counts as a fallback.
   */
  bool Accepts(unsigned int width, unsigned int height);
  /*!
   * \brief Copy width x height BGRA8 texels into a page, with the gutter.
   * \return the region, empty if the texels fit no page (counted as a fallback)
   */
  Region Insert(const uint8_t* pixels, unsigned int pitch, unsigned int width, unsigned int height);
  //! Give the region back after QUARANTINE_FRAMES frames; region is empty afterwards
  void Free(Region& region);

  //! The page's view and texture, nullptr while the device is gone
  ID3D11ShaderResourceView* GetView(const Region& region) const;
  ID3D11Texture2D* GetTexture(const Region& region) const;
  //! Map the texture rectangle (x1, y1, x2, y2, texture coordinates) of a region into its page
  static void ToPage(const Region& region, DirectX::XMFLOAT4& rect)
  {
    rect.x = static_cast<float>(static_cast<double>(rect.x) * region.scaleU + region.offsetU);
    rect.y = static_cast<float>(static_cast<double>(rect.y) * region.scaleV + region.offsetV);
    rect.z = static_cast<float>(static_cast<double>(rect.z) * region.scaleU + region.offsetU);
    rect.w = static_cast<float>(static_cast<double>(rect.w) * region.scaleV + region.offsetV);
  }

  /*! \brief Called once per rendered frame: releases quarantined regions and empty pages. */
  void BeginFrame();
  void CountCopy();
  void CountRangeFallback() { ++m_rangeFallbacks; }
  Stats GetStats() const;

  static constexpr unsigned int GUTTER = 1;
  static constexpr unsigned int QUARANTINE_FRAMES = 2;
  static constexpr unsigned int MAX_PAGES = 8;

private:
  CTextureAtlasDX() = default;

  struct Span
  {
    unsigned int x;
    unsigned int width;
  };
  //! a row of regions of (about) the same height; free spans sorted by x
  struct Shelf
  {
    unsigned int y;
    unsigned int height;
    std::vector<Span> free;
    unsigned int regions = 0;
  };
  struct Page
  {
    CD3DTexture texture;
    unsigned int size = 0;
    std::vector<Shelf> shelves;
    unsigned int top = 0; // first row above the shelves
    unsigned int regions = 0; // including the quarantined ones
    uint64_t usedTexels = 0;
  };

  static bool Allocate(Page& page, unsigned int width, unsigned int height, unsigned int& x,
                       unsigned int& y);
  static void Release(Page& page, unsigned int x, unsigned int y, unsigned int width);
  static bool Upload(Page& page,
                     unsigned int x,
                     unsigned int y,
                     const uint8_t* pixels,
                     unsigned int pitch,
                     unsigned int width,
                     unsigned int height);

  mutable CCriticalSection m_section;
  std::vector<std::unique_ptr<Page>> m_pages;
  std::vector<std::pair<Region, uint64_t>> m_quarantine; // freed region, frame of the free
  uint64_t m_frame = 0;
  uint64_t m_fallbacks = 0;
  uint64_t m_copies = 0;
  uint64_t m_rangeFallbacks = 0; // GUI thread only
};

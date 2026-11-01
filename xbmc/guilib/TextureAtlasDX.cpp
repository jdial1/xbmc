/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TextureAtlasDX.h"

#include "GUIShaderDX.h"
#include "ServiceBroker.h"
#include "rendering/RenderSystem.h"
#include "rendering/dx/DeviceResources.h"
#include "rendering/dx/RenderContext.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

namespace
{
constexpr unsigned int PAGE_SIZE = 2048;
constexpr unsigned int SHELF_ALIGN = 4; // shelf heights are rounded up to this
} // namespace

CTextureAtlasDX& CTextureAtlasDX::Get()
{
  // never destroyed: textures may outlive any owner, and a page outliving the device is harmless
  static CTextureAtlasDX* atlas = new CTextureAtlasDX();
  return *atlas;
}

bool CTextureAtlasDX::Accepts(unsigned int width, unsigned int height)
{
  CRenderSystemBase* renderSystem = CServiceBroker::GetRenderSystem();
  CGUIShaderDX* shader = DX::Windowing()->GetGUIShader();
  if (!renderSystem || !shader || !shader->CanBatch() || !renderSystem->GetEnabledGUIBatching() ||
      !renderSystem->GetEnabledAtlasTextures() || width == 0 || height == 0)
    return false;

  const unsigned int maxSize = renderSystem->GetAtlasMaxSize();
  if (width > maxSize || height > maxSize)
  {
    std::unique_lock lock(m_section);
    ++m_fallbacks;
    return false;
  }
  return true;
}

CTextureAtlasDX::Region CTextureAtlasDX::Insert(const uint8_t* pixels,
                                                unsigned int pitch,
                                                unsigned int width,
                                                unsigned int height)
{
  std::unique_lock lock(m_section);

  const unsigned int cellWidth = width + 2 * GUTTER;
  const unsigned int cellHeight = height + 2 * GUTTER;
  Page* page = nullptr;
  unsigned int x = 0;
  unsigned int y = 0;
  for (const auto& candidate : m_pages)
  {
    // a page whose texture went with the device takes no new regions
    if (candidate->texture.Get() && Allocate(*candidate, cellWidth, cellHeight, x, y))
    {
      page = candidate.get();
      break;
    }
  }

  if (!page && m_pages.size() < MAX_PAGES)
  {
    const unsigned int size =
        std::min(PAGE_SIZE, CServiceBroker::GetRenderSystem()->GetMaxTextureSize());
    auto newPage = std::make_unique<Page>();
    newPage->size = size;
    if (cellWidth <= size && cellHeight <= size &&
        newPage->texture.Create(size, size, 1, D3D11_USAGE_DEFAULT, DXGI_FORMAT_B8G8R8A8_UNORM) &&
        Allocate(*newPage, cellWidth, cellHeight, x, y))
    {
      page = newPage.get();
      m_pages.push_back(std::move(newPage));
      CLog::Log(LOGDEBUG, "Texture atlas: page {} created ({}x{})", m_pages.size(), size, size);
    }
  }

  if (!page || !Upload(*page, x, y, pixels, pitch, width, height))
  {
    if (page)
      Release(*page, x, y, cellWidth);
    ++m_fallbacks;
    return {};
  }

  page->regions++;
  page->usedTexels += static_cast<uint64_t>(cellWidth) * cellHeight;

  Region region;
  region.page = page;
  region.x = x + GUTTER;
  region.y = y + GUTTER;
  region.width = width;
  region.height = height;
  const double size = page->size;
  region.scaleU = width / size;
  region.scaleV = height / size;
  region.offsetU = region.x / size;
  region.offsetV = region.y / size;
  return region;
}

void CTextureAtlasDX::Free(Region& region)
{
  if (!region)
    return;

  std::unique_lock lock(m_section);
  m_quarantine.emplace_back(region, m_frame);
  region = {};
}

ID3D11ShaderResourceView* CTextureAtlasDX::GetView(const Region& region) const
{
  return region ? region.page->texture.GetShaderResource() : nullptr;
}

ID3D11Texture2D* CTextureAtlasDX::GetTexture(const Region& region) const
{
  return region ? region.page->texture.Get() : nullptr;
}

void CTextureAtlasDX::BeginFrame()
{
  std::unique_lock lock(m_section);
  ++m_frame;

  if (m_quarantine.empty())
    return;

  bool released = false;
  for (auto it = m_quarantine.begin(); it != m_quarantine.end();)
  {
    if (it->second + QUARANTINE_FRAMES > m_frame)
    {
      ++it;
      continue;
    }
    Region& region = it->first;
    Page& page = *region.page;
    const unsigned int cellWidth = region.width + 2 * GUTTER;
    Release(page, region.x - GUTTER, region.y - GUTTER, cellWidth);
    page.regions--;
    page.usedTexels -= static_cast<uint64_t>(cellWidth) * (region.height + 2 * GUTTER);
    released = true;
    it = m_quarantine.erase(it);
  }

  if (released)
  {
    const size_t pages = m_pages.size();
    std::erase_if(m_pages, [](const auto& page) { return page->regions == 0; });
    if (m_pages.size() != pages)
      CLog::Log(LOGDEBUG, "Texture atlas: {} empty page(s) released, {} left", pages - m_pages.size(),
                m_pages.size());
  }
}

void CTextureAtlasDX::CountCopy()
{
  std::unique_lock lock(m_section);
  ++m_copies;
}

CTextureAtlasDX::Stats CTextureAtlasDX::GetStats() const
{
  std::unique_lock lock(m_section);
  Stats stats;
  stats.pages = static_cast<unsigned int>(m_pages.size());
  for (const auto& page : m_pages)
  {
    stats.bytes += static_cast<uint64_t>(page->size) * page->size * 4;
    stats.usedTexels += page->usedTexels;
    stats.regions += page->regions;
  }
  stats.regions -= static_cast<unsigned int>(m_quarantine.size());
  stats.fallbacks = m_fallbacks;
  stats.copies = m_copies;
  stats.rangeFallbacks = m_rangeFallbacks;
  return stats;
}

bool CTextureAtlasDX::Allocate(
    Page& page, unsigned int width, unsigned int height, unsigned int& x, unsigned int& y)
{
  // the lowest shelf that is tall enough and either empty or not much taller, else a new shelf on
  // top
  Shelf* best = nullptr;
  for (Shelf& shelf : page.shelves)
  {
    if (shelf.height < height || (shelf.height > height + height / 2 + SHELF_ALIGN && shelf.regions))
      continue;
    if (best && best->height <= shelf.height)
      continue;
    if (std::ranges::any_of(shelf.free, [width](const Span& s) { return s.width >= width; }))
      best = &shelf;
  }

  if (!best)
  {
    const unsigned int shelfHeight = (height + SHELF_ALIGN - 1) / SHELF_ALIGN * SHELF_ALIGN;
    if (width <= page.size && page.top + shelfHeight <= page.size)
    {
      page.shelves.push_back({page.top, shelfHeight, {{0, page.size}}});
      page.top += shelfHeight;
      best = &page.shelves.back();
    }
  }

  if (!best)
    return false;

  auto span = std::ranges::find_if(best->free, [width](const Span& s) { return s.width >= width; });
  x = span->x;
  y = best->y;
  span->x += width;
  span->width -= width;
  if (span->width == 0)
    best->free.erase(span);
  best->regions++;
  return true;
}

void CTextureAtlasDX::Release(Page& page, unsigned int x, unsigned int y, unsigned int width)
{
  auto shelf = std::ranges::find_if(page.shelves, [y](const Shelf& s) { return s.y == y; });
  if (shelf == page.shelves.end())
    return;

  // put the span back in order and merge it with its neighbours
  auto next = std::ranges::find_if(shelf->free, [x](const Span& s) { return s.x > x; });
  auto it = shelf->free.insert(next, Span{x, width});
  if (std::next(it) != shelf->free.end() && it->x + it->width == std::next(it)->x)
  {
    it->width += std::next(it)->width;
    shelf->free.erase(std::next(it));
  }
  if (it != shelf->free.begin() && std::prev(it)->x + std::prev(it)->width == it->x)
  {
    std::prev(it)->width += it->width;
    shelf->free.erase(it);
  }

  if (--shelf->regions == 0)
    shelf->free = {{0, page.size}};
  // empty shelves on top give their rows back
  while (!page.shelves.empty() && page.shelves.back().regions == 0)
  {
    page.top = page.shelves.back().y;
    page.shelves.pop_back();
  }
}

bool CTextureAtlasDX::Upload(Page& page,
                             unsigned int x,
                             unsigned int y,
                             const uint8_t* pixels,
                             unsigned int pitch,
                             unsigned int width,
                             unsigned int height)
{
  ID3D11DeviceContext* pContext = DX::DeviceResources::Get()->GetImmediateContext();
  if (!pContext || !page.texture.Get() || !pixels)
    return false;

  // the texels with their edge rows and columns repeated once around them
  const unsigned int cellWidth = width + 2 * GUTTER;
  const unsigned int cellHeight = height + 2 * GUTTER;
  std::vector<uint32_t> cell(static_cast<size_t>(cellWidth) * cellHeight);
  for (unsigned int row = 0; row < cellHeight; row++)
  {
    const unsigned int srcRow = std::clamp(row, GUTTER, height + GUTTER - 1) - GUTTER;
    const auto* src = reinterpret_cast<const uint32_t*>(pixels + static_cast<size_t>(srcRow) * pitch);
    uint32_t* dst = cell.data() + static_cast<size_t>(row) * cellWidth;
    std::fill_n(dst, GUTTER, src[0]);
    std::memcpy(dst + GUTTER, src, width * sizeof(uint32_t));
    std::fill_n(dst + GUTTER + width, GUTTER, src[width - 1]);
  }

  // the immediate context, as the font glyph cache: recorded command lists only sample regions in
  // use, a reused region has been out of use for QUARANTINE_FRAMES frames
  const CD3D11_BOX box(x, y, 0, x + cellWidth, y + cellHeight, 1);
  pContext->UpdateSubresource(page.texture.Get(), 0, &box, cell.data(), cellWidth * 4, 0);
  return true;
}

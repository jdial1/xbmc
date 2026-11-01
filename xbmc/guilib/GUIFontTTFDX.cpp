/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIFontTTFDX.h"

#include "rendering/RenderSystem.h"

#include "GUIFontManager.h"
#include "GUIShaderDX.h"
#include "TextureDX.h"
#include "rendering/dx/DeviceResources.h"
#include "rendering/dx/RenderContext.h"
#include "utils/MathUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

// stuff for freetype
#include <ft2build.h>

using namespace Microsoft::WRL;

#ifdef TARGET_WINDOWS_STORE
#define generic GenericFromFreeTypeLibrary
#endif

#include FT_FREETYPE_H
#include FT_GLYPH_H

namespace
{
constexpr size_t ELEMENT_ARRAY_MAX_CHAR_INDEX = 2000;

//! RenderCharacter writes each glyph as a rectangle of one colour, corners x1 y1, x2 y1, x2 y2, x1 y2
bool IsGlyphQuad(const SVertex* v)
{
  const auto sameColor = [v](int i)
  { return std::memcmp(&v[i].col, &v[0].col, sizeof(v[0].col)) == 0; };
  return v[1].x == v[2].x && v[3].x == v[0].x && v[1].y == v[0].y && v[3].y == v[2].y &&
         v[1].u == v[2].u && v[3].u == v[0].u && v[1].v == v[0].v && v[3].v == v[2].v &&
         v[0].z == 0 && v[1].z == 0 && v[2].z == 0 && v[3].z == 0 && sameColor(1) && sameColor(2) &&
         sameColor(3);
}
} /* namespace */

CGUIFontTTF* CGUIFontTTF::CreateGUIFontTTF(const std::string& fontIdent)
{
  return new CGUIFontTTFDX(fontIdent);
}

CGUIFontTTFDX::CGUIFontTTFDX(const std::string& fontIdent) : CGUIFontTTF(fontIdent)
{
  DX::Windowing()->Register(this);
}

CGUIFontTTFDX::~CGUIFontTTFDX(void)
{
  DX::Windowing()->Unregister(this);
}

bool CGUIFontTTFDX::FirstBegin()
{
  if (!DX::DeviceResources::Get()->GetD3DContext())
    return false;

  CGUIShaderDX* pGUIShader = DX::Windowing()->GetGUIShader();
  if (pGUIShader == nullptr)
    return false;

  // Batched text sets no shader method, which would end the GUI batch: only the clipping information
  // is recalculated. It needs scissor clipping, which the batch's pixel shader does instead.
  m_batchText = false;
  if (pGUIShader->HasTextBatchPath())
  {
    pGUIShader->UpdateClipParams();
    if (DX::Windowing()->ScissorsCanEffectClipping())
    {
      m_batchText = true;
      m_scissorClip = true;
      return true;
    }
  }

  // Set a shader first to trigger the internal clipping recalculations. The cached clipping data
  // contains stale information from the previous draw otherwise.
  // Mirrors the GL/GLES workaround
  pGUIShader->Begin(SHADER_METHOD_RENDER_FONT_RGBA);

  if (DX::Windowing()->ScissorsCanEffectClipping())
  {
    m_scissorClip = true;
    // SHADER_METHOD_RENDER_FONT_RGBA already activated
  }
  else
  {
    m_scissorClip = false;
    DX::Windowing()->ResetScissors();
    pGUIShader->Begin(SHADER_METHOD_RENDER_FONT_RGBA_SHADER_CLIP);
  }

  return true;
}

void CGUIFontTTFDX::LastEnd()
{
  // static vertex arrays are not supported anymore
  assert(m_vertex.empty());

  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();
  if (!pContext || !winSystem)
    return;

  typedef CGUIFontTTF::CTranslatedVertices trans;
  // no chars to render
  if (std::all_of(m_vertexTrans.begin(), m_vertexTrans.end(),
                  [](trans& _) { return _.m_vertexBuffer->size <= 0; }))
    return;

  CGUIShaderDX* pGUIShader = DX::Windowing()->GetGUIShader();

  if (pGUIShader == nullptr)
    return;

  // batched: the texts join the GUI batch; if it cannot take them the font draws them itself, with
  // the shader method FirstBegin() did not set
  if (m_batchText)
  {
    if (QueueText(*pGUIShader, winSystem->GetGfxContext()))
      return;
    pGUIShader->Begin(SHADER_METHOD_RENDER_FONT_RGBA);
  }

  CreateStaticIndexBuffer();
  if (m_staticIndexBuffer == nullptr)
    return;

  pGUIShader->SetDepth(CServiceBroker::GetWinSystem()->GetGfxContext().GetTransformDepth());

  // Set font texture as shader resource
  if (m_speedupTexture == nullptr)
    return;
  pGUIShader->SetShaderViews(1, m_speedupTexture->GetAddressOfSRV());
  // Enable alpha blend
  DX::Windowing()->SetAlphaBlendEnable(true);
  // Set our static index buffer
  pContext->IASetIndexBuffer(m_staticIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

  pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // Store current scissors
  CGraphicContext& context = winSystem->GetGfxContext();
  CRect scissor;
  if (m_scissorClip)
    scissor = context.StereoCorrection(context.GetScissors());

  for (size_t i = 0; i < m_vertexTrans.size(); i++)
  {
    // ignore empty buffers
    if (m_vertexTrans[i].m_vertexBuffer->size == 0)
      continue;

    CTextVertices* text =
        static_cast<CTextVertices*>(m_vertexTrans[i].m_vertexBuffer->bufferHandle);
    const CD3DBuffer* vBuffer = text ? GetBuffer(*text) : nullptr;
    if (vBuffer == nullptr)
      continue;

    if (m_scissorClip)
    {
      // Apply the clip rectangle
      CRect clip = DX::Windowing()->ClipRectToScissorRect(m_vertexTrans[i].m_clip);
      clip.Intersect(scissor);

      // skip empty clip, a little improvement to not render invisible text
      if (clip.IsEmpty())
        continue;
      DX::Windowing()->SetScissors(clip);
    }
    else
    {
      // clip using vertex shader
      const float scaleX = context.GetGUIScaleX();
      const float scaleY = context.GetGUIScaleY();

      if (scaleX == 0 || scaleY == 0)
      {
        CLog::LogF(LOGERROR, "Invalid GUI scaling ({}x{}).", scaleX, scaleY);
        continue;
      }

      const float x1 = (m_vertexTrans[i].m_clip.x1 - m_vertexTrans[i].m_translateX -
                        m_vertexTrans[i].m_offsetX) /
                       scaleX;
      const float y1 = (m_vertexTrans[i].m_clip.y1 - m_vertexTrans[i].m_translateY -
                        m_vertexTrans[i].m_offsetY) /
                       scaleY;
      const float x2 = (m_vertexTrans[i].m_clip.x2 - m_vertexTrans[i].m_translateX -
                        m_vertexTrans[i].m_offsetX) /
                       scaleX;
      const float y2 = (m_vertexTrans[i].m_clip.y2 - m_vertexTrans[i].m_translateY -
                        m_vertexTrans[i].m_offsetY) /
                       scaleY;

      pGUIShader->SetShaderClip(x1, y1, x2, y2);

      // Texture steps
      if (m_textureWidth == 0 || m_textureHeight == 0)
      {
        CLog::LogF(LOGERROR, "Invalid texture dimensions ({}x{}).", m_textureWidth,
                   m_textureHeight);
        continue;
      }

      const float stepX = 1.f / static_cast<float>(m_textureWidth);
      const float stepY = 1.f / static_cast<float>(m_textureHeight);

      pGUIShader->SetTexStep(stepX, stepY, 1, 1);
    }

    // calculate the fractional offset to the ideal position
    float fractX =
        context.ScaleFinalXCoord(m_vertexTrans[i].m_translateX, m_vertexTrans[i].m_translateY);
    float fractY =
        context.ScaleFinalYCoord(m_vertexTrans[i].m_translateX, m_vertexTrans[i].m_translateY);
    fractX = -fractX + std::round(fractX);
    fractY = -fractY + std::round(fractY);

    // The multiplication order below is important and the reverse of the GL/GLES chain because of
    // column-major vs row-major differences
    // proj * model * gui * scroll * translation * scaling * correction factor
    const XMMATRIX world = pGUIShader->GetWorld();

    const XMMATRIX correction = XMMatrixTranslation(fractX, fractY, 0.0f);
    const XMMATRIX scale = XMMatrixScaling(context.GetGUIScaleX(), context.GetGUIScaleY(), 1.0f);
    const XMMATRIX translation =
        XMMatrixTranslation(m_vertexTrans[i].m_translateX, m_vertexTrans[i].m_translateY, 0.0f);
    const XMMATRIX offset =
        XMMatrixTranslation(m_vertexTrans[i].m_offsetX, m_vertexTrans[i].m_offsetY, 0.0f);
    const XMMATRIX gui =
        XMLoadFloat3x4(reinterpret_cast<const XMFLOAT3X4*>(&context.GetGUIMatrix().m));

    XMMATRIX matrix = XMMatrixMultiply(world, correction);
    matrix = XMMatrixMultiply(matrix, scale);
    matrix = XMMatrixMultiply(matrix, translation);
    matrix = XMMatrixMultiply(matrix, offset);
    matrix = XMMatrixMultiply(matrix, gui);

    pGUIShader->SetWorld(matrix);

    // Set the static vertex buffer to active in the input assembler
    ID3D11Buffer* buffers[1] = {vBuffer->Get()};

    unsigned int offsets = 0;
    unsigned int strides = sizeof(SVertex);
    pContext->IASetVertexBuffers(0, 1, buffers, &strides, &offsets);

    // Do the actual drawing operation, split into groups of characters no
    // larger than the pre-determined size of the element array
    for (size_t character = 0; m_vertexTrans[i].m_vertexBuffer->size > character;
         character += ELEMENT_ARRAY_MAX_CHAR_INDEX)
    {
      size_t count = m_vertexTrans[i].m_vertexBuffer->size - character;
      count = std::min<size_t>(count, ELEMENT_ARRAY_MAX_CHAR_INDEX);

      // 6 indices and 4 vertices per character
      if (count > std::numeric_limits<unsigned int>::max() / 6 ||
          character > std::numeric_limits<unsigned int>::max() / 4)
      {
        CLog::LogF(LOGERROR, "Character index too large: {}", character);
        break;
      }
      pGUIShader->DrawIndexed(count * 6, 0, character * 4);
    }
    pGUIShader->SetWorld(world);
  }

  // restore scissor
  if (m_scissorClip)
    DX::Windowing()->SetScissors(scissor);

  pGUIShader->RestoreBuffers();
}

bool CGUIFontTTFDX::QueueText(CGUIShaderDX& shader, CGraphicContext& context)
{
  // all or nothing: a text the batch cannot take sends the whole block to the font's own draw
  if (!shader.HasTextBatchPath() ||
      !std::ranges::all_of(m_vertexTrans,
                           [](const CTranslatedVertices& trans)
                           {
                             const auto* text = static_cast<const CTextVertices*>(
                                 trans.m_vertexBuffer->bufferHandle);
                             return trans.m_vertexBuffer->size == 0 || !text ||
                                    text->glyphs.size() == trans.m_vertexBuffer->size;
                           }))
    return false;

  if (m_speedupTexture == nullptr)
    return true;
  ID3D11ShaderResourceView* view = m_speedupTexture->GetShaderResource();
  if (view == nullptr)
    return true;

  DX::Windowing()->SetAlphaBlendEnable(true);

  // the scissors LastEnd() sets for each text become the clip rectangles of their glyphs
  const CRect scissor = context.StereoCorrection(context.GetScissors());
  CGUIShaderDX::GlyphParams params;
  params.depth = context.GetTransformDepth();
  const XMMATRIX world = shader.GetWorld();
  for (const CTranslatedVertices& trans : m_vertexTrans)
  {
    const auto* text = static_cast<const CTextVertices*>(trans.m_vertexBuffer->bufferHandle);
    if (trans.m_vertexBuffer->size == 0 || !text)
      continue;

    CRect clip = DX::Windowing()->ClipRectToScissorRect(trans.m_clip);
    clip.Intersect(scissor);
    if (clip.IsEmpty())
      continue;
    // rounded as SetScissors rounds
    params.clip = {static_cast<float>(MathUtils::round_int(clip.x1)),
                   static_cast<float>(MathUtils::round_int(clip.y1)),
                   static_cast<float>(MathUtils::round_int(clip.x2)),
                   static_cast<float>(MathUtils::round_int(clip.y2))};

    // the matrix of LastEnd(), see there
    float fractX = context.ScaleFinalXCoord(trans.m_translateX, trans.m_translateY);
    float fractY = context.ScaleFinalYCoord(trans.m_translateX, trans.m_translateY);
    fractX = -fractX + std::round(fractX);
    fractY = -fractY + std::round(fractY);

    const XMMATRIX correction = XMMatrixTranslation(fractX, fractY, 0.0f);
    const XMMATRIX scale = XMMatrixScaling(context.GetGUIScaleX(), context.GetGUIScaleY(), 1.0f);
    const XMMATRIX translation = XMMatrixTranslation(trans.m_translateX, trans.m_translateY, 0.0f);
    const XMMATRIX offset = XMMatrixTranslation(trans.m_offsetX, trans.m_offsetY, 0.0f);
    const XMMATRIX gui =
        XMLoadFloat3x4(reinterpret_cast<const XMFLOAT3X4*>(&context.GetGUIMatrix().m));

    XMMATRIX matrix = XMMatrixMultiply(world, correction);
    matrix = XMMatrixMultiply(matrix, scale);
    matrix = XMMatrixMultiply(matrix, translation);
    matrix = XMMatrixMultiply(matrix, offset);
    matrix = XMMatrixMultiply(matrix, gui);

    XMStoreFloat4x4(&params.wvp, shader.GetWorldViewProj(matrix));
    shader.QueueGlyphs(view, text->glyphs.data(), static_cast<unsigned int>(text->glyphs.size()),
                       params);
  }

  // as LastEnd() leaves them
  DX::Windowing()->SetScissors(scissor);
  return true;
}

CVertexBuffer CGUIFontTTFDX::CreateVertexBuffer(const std::vector<SVertex>& vertices) const
{
  // do not create empty buffers, leave buffer as nullptr, it will be ignored on drawing stage
  if (vertices.empty())
    return CVertexBuffer(nullptr, 0, this);

  auto text = std::make_unique<CTextVertices>();
  CGUIShaderDX* shader = DX::Windowing()->GetGUIShader();
  if (shader && shader->HasTextBatchPath() && vertices.size() % 4 == 0)
  {
    // batched text needs the glyph quads; the vertex buffer waits until the font draws the text
    text->glyphs.reserve(vertices.size() / 4);
    for (size_t i = 0; i < vertices.size(); i += 4)
    {
      const SVertex* v = &vertices[i];
      if (!IsGlyphQuad(v))
      {
        text->glyphs.clear();
        break;
      }
      text->glyphs.push_back(
          {{v[0].x, v[0].y, v[2].x, v[2].y}, {v[0].u, v[0].v, v[2].u, v[2].v}, v[0].col});
    }
  }
  text->vertices = vertices;
  if (text->glyphs.empty() && !GetBuffer(*text))
    return CVertexBuffer(nullptr, 0, this);

  CTextVertices* handle = text.release();
  AddReference(const_cast<CGUIFontTTFDX*>(this), handle);
  return CVertexBuffer(reinterpret_cast<void*>(handle), vertices.size() / 4, this);
}

const CD3DBuffer* CGUIFontTTFDX::GetBuffer(CTextVertices& text)
{
  if (!text.buffer && !text.vertices.empty())
  {
    auto buffer = std::make_unique<CD3DBuffer>();
    if (buffer->Create(D3D11_BIND_VERTEX_BUFFER, static_cast<UINT>(text.vertices.size()),
                       sizeof(SVertex), DXGI_FORMAT_UNKNOWN, D3D11_USAGE_IMMUTABLE,
                       text.vertices.data()))
      text.buffer = std::move(buffer);
    else
      CLog::LogF(LOGERROR, "Failed to create vertex buffer.");
    // one attempt, as before: the vertices are not needed any more either way
    text.vertices = {};
  }
  return text.buffer.get();
}

void CGUIFontTTFDX::AddReference(CGUIFontTTFDX* font, CTextVertices* text)
{
  font->m_buffers.emplace_back(text);
}

void CGUIFontTTFDX::DestroyVertexBuffer(CVertexBuffer& buffer) const
{
  if (nullptr != buffer.bufferHandle)
  {
    CTextVertices* text = static_cast<CTextVertices*>(buffer.bufferHandle);
    ClearReference((CGUIFontTTFDX*)this, text);
    delete text;
    buffer.bufferHandle = 0;
  }
}

void CGUIFontTTFDX::ClearReference(CGUIFontTTFDX* font, CTextVertices* text)
{
  std::list<CTextVertices*>::iterator it =
      std::find(font->m_buffers.begin(), font->m_buffers.end(), text);
  if (it != font->m_buffers.end())
    font->m_buffers.erase(it);
}

std::unique_ptr<CTexture> CGUIFontTTFDX::ReallocTexture(unsigned int& newHeight)
{
  assert(newHeight != 0);
  assert(m_textureWidth != 0);

  // queued glyphs are drawn from the cache texture of their time: draw them before it changes
  if (CGUIShaderDX* shader = DX::Windowing()->GetGUIShader())
    shader->FlushBatch(CGUIShaderDX::BatchFlush::GLYPH_CACHE);

  // grow geometrically (as the GL/GLES font caches do): every reallocation copies the texture and
  // flushes the vertex caches, so growing one text line at a time is O(lines) reallocations
  // (the caller has checked newHeight against the max texture size)
  newHeight = std::min(CTexture::PadPow2(newHeight), m_renderSystem->GetMaxTextureSize());
  if (m_textureHeight == 0)
  {
    m_texture.reset();
    m_speedupTexture.reset();
  }
  m_staticCache.Flush();
  m_dynamicCache.Flush();

  std::unique_ptr<CDXTexture> pNewTexture =
      std::make_unique<CDXTexture>(m_textureWidth, newHeight, XB_FMT_A8);
  std::unique_ptr<CD3DTexture> newSpeedupTexture = std::make_unique<CD3DTexture>();
  if (!newSpeedupTexture->Create(m_textureWidth, newHeight, 1, D3D11_USAGE_DEFAULT,
                                 DXGI_FORMAT_R8G8B8A8_UNORM))
  {
    return nullptr;
  }

  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetImmediateContext();
  if (!pContext)
    return nullptr;

  // Clear the new texture on the GPU (white, alpha 0, like ClearTexturePixels), then copy the old
  // glyphs over it; fall back to a CPU fill if the format can't be a render target
  ID3D11RenderTargetView* clearTarget = newSpeedupTexture->GetRenderTarget();
  if (clearTarget)
  {
    const float clearColor[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    pContext->ClearRenderTargetView(clearTarget, clearColor);
  }

  // There might be data to copy from the previous texture
  if (m_speedupTexture)
  {
    CD3D11_BOX rect(0, 0, 0, m_textureWidth, m_textureHeight, 1);
    pContext->CopySubresourceRegion(newSpeedupTexture->Get(), 0, 0, 0, 0, m_speedupTexture->Get(),
                                    0, &rect);
  }

  // Clear the previously unused part of the resource (CPU fallback)
  if (!clearTarget && newHeight > m_textureHeight)
  {
    const unsigned int startRow = m_speedupTexture ? m_textureHeight : 0;
    CD3D11_BOX rect(0, startRow, 0, m_textureWidth, newHeight, 1);
    std::vector<uint8_t> clear(m_textureWidth * 4 * (newHeight - startRow));
    ClearTexturePixels(clear.data(), clear.size());
    pContext->UpdateSubresource(newSpeedupTexture->Get(), 0, &rect, clear.data(),
                                m_textureWidth * 4, 0);
  }
  m_texture.reset();

  m_textureHeight = newHeight;
  m_textureScaleY = 1.0f / m_textureHeight;
  m_speedupTexture = std::move(newSpeedupTexture);

  return pNewTexture;
}

bool CGUIFontTTFDX::CopyCharToTexture(const uint8_t* pixels,
                                      unsigned int pitch,
                                      unsigned int x1,
                                      unsigned int y1,
                                      unsigned int x2,
                                      unsigned int y2)
{
  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetImmediateContext();
  if (m_speedupTexture && m_speedupTexture->Get() && pContext && pixels)
  {
    CD3D11_BOX dstBox(x1, y1, 0, x2, y2, 1);
    pContext->UpdateSubresource(m_speedupTexture->Get(), 0, &dstBox, pixels, pitch, 0);
    return true;
  }

  return false;
}

void CGUIFontTTFDX::DeleteHardwareTexture()
{
}

void CGUIFontTTFDX::CreateStaticIndexBuffer(void)
{
  // Fast path no mutex
  if (m_staticIndexBufferCreated.load(std::memory_order_acquire))
    return;

  std::unique_lock lock(m_staticIndexBufferSection);

  // Second protected check for thread safety
  if (m_staticIndexBufferCreated.load(std::memory_order_relaxed))
    return;

  ComPtr<ID3D11Device> pDevice = DX::DeviceResources::Get()->GetD3DDevice();
  if (!pDevice)
    return;

  uint16_t index[ELEMENT_ARRAY_MAX_CHAR_INDEX][6];
  for (size_t i = 0; i < ELEMENT_ARRAY_MAX_CHAR_INDEX; i++)
  {
    index[i][0] = 4 * i;
    index[i][1] = 4 * i + 1;
    index[i][2] = 4 * i + 2;
    index[i][3] = 4 * i + 2;
    index[i][4] = 4 * i + 3;
    index[i][5] = 4 * i + 0;
  }

  CD3D11_BUFFER_DESC desc(sizeof(index), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE);
  D3D11_SUBRESOURCE_DATA initData = {};
  initData.pSysMem = index;

  if (SUCCEEDED(
          pDevice->CreateBuffer(&desc, &initData, m_staticIndexBuffer.ReleaseAndGetAddressOf())))
    m_staticIndexBufferCreated.store(true, std::memory_order_release);
}

CCriticalSection CGUIFontTTFDX::m_staticIndexBufferSection;
std::atomic<bool> CGUIFontTTFDX::m_staticIndexBufferCreated = false;
ComPtr<ID3D11Buffer> CGUIFontTTFDX::m_staticIndexBuffer = nullptr;

void CGUIFontTTFDX::OnDestroyDevice(bool fatal)
{
  std::unique_lock lock(m_staticIndexBufferSection);

  m_staticIndexBuffer = nullptr;
  m_staticIndexBufferCreated.store(false, std::memory_order_relaxed);
}

void CGUIFontTTFDX::OnCreateDevice(void)
{
}

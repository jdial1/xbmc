/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIShaderDX.h"
#include "TextureAtlasDX.h"
#include "windowing/GraphicContext.h"
#include "rendering/dx/DeviceResources.h"
#include "rendering/dx/RenderContext.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

// shaders bytecode includes
#include "guishader_checkerboard_left.h"
#include "guishader_checkerboard_right.h"
#include "guishader_clip_vert.h"
#include "guishader_default.h"
#include "guishader_fonts.h"
#include "guishader_interlaced_left.h"
#include "guishader_interlaced_right.h"
#include "guishader_multi_texture_blend.h"
#include "guishader_multi_texture_blend_nearest.h"
#include "guishader_quad_batch_vert.h"
#include "guishader_quad_vert.h"
#include "guishader_texture.h"
#include "guishader_texture_nearest.h"
#include "guishader_texture_noblend.h"
#include "guishader_vert.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <d3dcompiler.h>

using namespace DirectX;
using namespace Microsoft::WRL;

// shaders bytecode holder
// clang-format off
constexpr std::array<D3D_SHADER_DATA, SHADER_METHOD_RENDER_COUNT> shaderCode =
{{
  { guishader_default, sizeof(guishader_default) }, // SHADER_METHOD_RENDER_DEFAULT
  { guishader_texture_noblend, sizeof(guishader_texture_noblend) }, // SHADER_METHOD_RENDER_TEXTURE_NOBLEND
  { guishader_fonts, sizeof(guishader_fonts) }, // SHADER_METHOD_RENDER_FONT
  { guishader_fonts, sizeof(guishader_fonts) }, // SHADER_METHOD_RENDER_FONT_SHADER_CLIP
  { guishader_texture, sizeof(guishader_texture) }, // SHADER_METHOD_RENDER_TEXTURE_BLEND
  { guishader_texture_nearest, sizeof(guishader_texture_nearest) }, // SHADER_METHOD_RENDER_TEXTURE_BLEND_NEAREST
  { guishader_multi_texture_blend, sizeof(guishader_multi_texture_blend) }, // SHADER_METHOD_RENDER_MULTI_TEXTURE_BLEND
  { guishader_multi_texture_blend_nearest, sizeof(guishader_multi_texture_blend_nearest) }, // SHADER_METHOD_RENDER_MULTI_TEXTURE_BLEND_NEAREST
  { guishader_interlaced_left, sizeof(guishader_interlaced_left) }, // SHADER_METHOD_RENDER_STEREO_INTERLACED_LEFT
  { guishader_interlaced_right, sizeof(guishader_interlaced_right) }, // SHADER_METHOD_RENDER_STEREO_INTERLACED_RIGHT
  { guishader_checkerboard_left, sizeof(guishader_checkerboard_left) }, // SHADER_METHOD_RENDER_STEREO_CHECKERBOARD_LEFT
  { guishader_checkerboard_right, sizeof(guishader_checkerboard_right) }, // SHADER_METHOD_RENDER_STEREO_CHECKERBOARD_RIGHT
  { guishader_texture, sizeof(guishader_texture) }, // SHADER_METHOD_RENDER_FONT_RGBA
  { guishader_texture, sizeof(guishader_texture) }, // SHADER_METHOD_RENDER_FONT_RGBA_SHADER_CLIP
}};
// clang-format on

// Build-time check of the count of shader code entries versus the count of shader methods.
// The default initializer allows missing array initializer elements to slip without this.
static_assert(std::ranges::none_of(shaderCode,
                                   [](const auto& s)
                                   { return s.pBytecode == nullptr || s.BytecodeLength == 0; }));

static_assert(sizeof(CGUIShaderDX::QuadInstance) == 11 * sizeof(XMFLOAT4),
              "QuadInstance must match the instance layout of guishader_quad_batch_vert.hlsl");

namespace
{
#if defined(_DEBUG)
constexpr bool VALIDATE_BATCHES = true; // compare the state of each batch at its flush
#else
constexpr bool VALIDATE_BATCHES = false;
#endif

constexpr UINT BATCH_CORNER_STRIDE = 2 * sizeof(float);

bool SameMatrix(const XMMATRIX& a, const XMMATRIX& b)
{
  return memcmp(&a, &b, sizeof(XMMATRIX)) == 0;
}
} // namespace

void XM_CALLCONV CGUIShaderDX::CWorldViewProj::SetWVP(const XMMATRIX& w,
                                                      const XMMATRIX& v,
                                                      const XMMATRIX& p)
{
  m_isWorldDirty = true;
  m_world = w;
  m_isVPDirty = true;
  m_view = v;
  m_projection = p;
}

void CGUIShaderDX::CWorldViewProj::SetWorld(const XMMATRIX& value)
{
  m_isWorldDirty = true;
  m_world = value;
}

void CGUIShaderDX::CWorldViewProj::SetView(const XMMATRIX& value)
{
  m_isVPDirty = true;
  m_view = value;
}

void CGUIShaderDX::CWorldViewProj::SetProjection(const XMMATRIX& value)
{
  m_isVPDirty = true;
  m_projection = value;
}

DirectX::XMMATRIX XM_CALLCONV CGUIShaderDX::CWorldViewProj::GetWVP() const
{
  if (m_isVPDirty)
    m_cachedVP = XMMatrixMultiply(m_view, m_projection);

  if (m_isVPDirty || m_isWorldDirty)
    m_cachedWVP = XMMatrixMultiplyTranspose(m_world, m_cachedVP);

  m_isWorldDirty = false;
  m_isVPDirty = false;

  return m_cachedWVP;
}

CGUIShaderDX::~CGUIShaderDX()
{
  Release();
}

bool CGUIShaderDX::Initialize()
{
  // Create input layout
  D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };

  if (!m_vertexShader.Create(guishader_vert, sizeof(guishader_vert), layout, ARRAYSIZE(layout)) ||
      !m_vertexShaderClip.Create(guishader_clip_vert, sizeof(guishader_clip_vert), layout,
                                 ARRAYSIZE(layout)))
  {
    goto error;
  }

  {
    // GPU texture quads (optional: CGUITexture falls back to the CPU path without it)
    D3D11_INPUT_ELEMENT_DESC quadLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    m_hasQuadPath = m_vertexShaderQuad.Create(guishader_quad_vert, sizeof(guishader_quad_vert),
                                              quadLayout, ARRAYSIZE(quadLayout));
    if (!m_hasQuadPath)
      CLog::LogF(LOGWARNING, "GPU texture quad shader unavailable, using the CPU quad path.");
  }

  {
    // batched texture quads: slot 0 holds the corner selectors of the 4 vertices, slot 1 one
    // QuadInstance per quad. Instancing needs feature level 9_3, as does the shader profile.
    constexpr UINT instanceElements = static_cast<UINT>(sizeof(QuadInstance) / sizeof(XMFLOAT4));
    D3D11_INPUT_ELEMENT_DESC batchLayout[1 + instanceElements] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    for (UINT i = 0; i < instanceElements; i++)
      batchLayout[1 + i] = {"TEXCOORD",
                            i,
                            DXGI_FORMAT_R32G32B32A32_FLOAT,
                            1,
                            i * static_cast<UINT>(sizeof(XMFLOAT4)),
                            D3D11_INPUT_PER_INSTANCE_DATA,
                            1};
    const D3D_FEATURE_LEVEL featureLevel = DX::DeviceResources::Get()->GetDeviceFeatureLevel();
    m_hasBatchPath = featureLevel >= D3D_FEATURE_LEVEL_9_3 &&
                     m_vertexShaderBatch.Create(guishader_quad_batch_vert,
                                                sizeof(guishader_quad_batch_vert), batchLayout,
                                                ARRAYSIZE(batchLayout));
    if (!m_hasBatchPath)
      CLog::LogF(LOGWARNING,
                 "Batched GUI quads unavailable (feature level {:x}), drawing them per texture.",
                 static_cast<unsigned int>(featureLevel));
  }

  for (std::size_t i = 0; i < SHADER_METHOD_RENDER_COUNT; i++)
  {
    // All methods except the font ones with shader clipping share guishader_vert.hlsl
    if (i == SHADER_METHOD_RENDER_FONT_SHADER_CLIP ||
        i == SHADER_METHOD_RENDER_FONT_RGBA_SHADER_CLIP)
      m_shaders[i].m_vs = &m_vertexShaderClip;
    else
      m_shaders[i].m_vs = &m_vertexShader;

    if (!m_shaders[i].m_ps.Create(shaderCode[i].pBytecode, shaderCode[i].BytecodeLength))
      goto error;
  }

  if (!CreateBuffers() || !CreateSamplers())
    goto error;

  m_bCreated = true;
  if (m_hasBatchPath)
    CLog::LogF(LOGINFO, "GUI texture quads are batched (up to {} per instanced draw).",
               BATCH_CAPACITY);
  return true;

error:
  Release();

  std::ranges::for_each(m_shaders, [](auto& s) { s.m_ps.Release(); });

  m_vertexShader.Release();
  m_vertexShaderClip.Release();
  m_vertexShaderQuad.Release();
  m_vertexShaderBatch.Release();

  return false;
}

bool CGUIShaderDX::CreateBuffers()
{
  ComPtr<ID3D11Device> pDevice = DX::DeviceResources::Get()->GetD3DDevice();

  // create vertex buffer
  CD3D11_BUFFER_DESC bufferDesc(sizeof(Vertex) * 4, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  if (FAILED(pDevice->CreateBuffer(&bufferDesc, NULL, m_pVertexBuffer.ReleaseAndGetAddressOf())))
  {
    CLog::LogF(LOGERROR, "Failed to create GUI vertex buffer.");
    return false;
  }

  // Create the constant buffer for WVP
  size_t buffSize = (sizeof(cbWorld) + 15) & ~15;
  CD3D11_BUFFER_DESC cbbd(buffSize, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE); // it can change very frequently
  if (FAILED(pDevice->CreateBuffer(&cbbd, NULL, m_pWVPBuffer.ReleaseAndGetAddressOf())))
  {
    CLog::LogF(LOGERROR, "Failed to create the world constant buffer.");
    return false;
  }
  // class members have reasonable defaults for the initial buffer load
  m_bIsWVPDirty = true;

  if (m_hasQuadPath)
  {
    // quad constants and a shared index buffer for up to MAX_GUI_QUADS quads (0 1 2, 2 3 0 each)
    CD3D11_BUFFER_DESC quadCbDesc((sizeof(QuadConstants) + 15) & ~15, D3D11_BIND_CONSTANT_BUFFER,
                                  D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    uint16_t indices[MAX_GUI_QUADS * 6];
    for (unsigned int i = 0; i < MAX_GUI_QUADS; i++)
    {
      const uint16_t base = static_cast<uint16_t>(i * 4);
      const uint16_t pattern[6] = {0, 1, 2, 2, 3, 0};
      for (unsigned int j = 0; j < 6; j++)
        indices[i * 6 + j] = base + pattern[j];
    }
    CD3D11_BUFFER_DESC quadIbDesc(sizeof(indices), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE);
    D3D11_SUBRESOURCE_DATA ibData = {indices, 0, 0};
    if (FAILED(pDevice->CreateBuffer(&quadCbDesc, nullptr, m_pQuadCB.ReleaseAndGetAddressOf())) ||
        FAILED(pDevice->CreateBuffer(&quadIbDesc, &ibData,
                                     m_pQuadIndexBuffer.ReleaseAndGetAddressOf())))
    {
      CLog::LogF(LOGWARNING, "Failed to create GPU texture quad buffers, using the CPU quad path.");
      m_hasQuadPath = false;
    }
  }

  if (m_hasBatchPath)
  {
    // corner selectors (0 = x1 or y1, 1 = x2 or y2) of the 4 vertices, and the 2 triangles of a quad
    const float corners[] = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    const uint16_t indices[] = {0, 1, 2, 2, 3, 0};
    CD3D11_BUFFER_DESC cornerDesc(sizeof(corners), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE);
    CD3D11_BUFFER_DESC indexDesc(sizeof(indices), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE);
    CD3D11_BUFFER_DESC instanceDesc(static_cast<UINT>(BATCH_CAPACITY * sizeof(QuadInstance)),
                                    D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC,
                                    D3D11_CPU_ACCESS_WRITE);
    D3D11_SUBRESOURCE_DATA cornerData = {corners, 0, 0};
    D3D11_SUBRESOURCE_DATA indexData = {indices, 0, 0};
    if (FAILED(pDevice->CreateBuffer(&cornerDesc, &cornerData,
                                     m_pBatchCornerBuffer.ReleaseAndGetAddressOf())) ||
        FAILED(pDevice->CreateBuffer(&indexDesc, &indexData,
                                     m_pBatchIndexBuffer.ReleaseAndGetAddressOf())) ||
        FAILED(pDevice->CreateBuffer(&instanceDesc, nullptr,
                                     m_pBatchInstanceBuffer.ReleaseAndGetAddressOf())))
    {
      CLog::LogF(LOGWARNING, "Failed to create the GUI quad batch buffers, drawing quads per texture.");
      m_hasBatchPath = false;
    }
    m_batch.reserve(256);
    m_batchOffset = 0;
    m_batchDiscard = true;
    m_batchNoOverwrite = true;
  }

  CRect viewPort;
  DX::Windowing()->GetViewPort(viewPort);

  // initial data for viewport buffer
  m_cbViewPort.TopLeftX = viewPort.x1;
  m_cbViewPort.TopLeftY = viewPort.y1;
  m_cbViewPort.Width = viewPort.Width();
  m_cbViewPort.Height = viewPort.Height();

  cbbd.ByteWidth = sizeof(cbViewPort);
  D3D11_SUBRESOURCE_DATA initData = { &m_cbViewPort, 0, 0 };
  // create viewport buffer
  if (FAILED(pDevice->CreateBuffer(&cbbd, &initData, m_pVPBuffer.ReleaseAndGetAddressOf())))
  {
    CLog::LogF(LOGERROR, "Failed to create the viewport constant buffer.");
    return false;
  }
  m_bIsVPDirty = false;

  return true;
}

bool CGUIShaderDX::CreateSamplers()
{
  // Linear sampler
  D3D11_SAMPLER_DESC sampDesc = {};
  sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampDesc.MinLOD = 0;
  sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

  if (FAILED(DX::DeviceResources::Get()->GetD3DDevice()->CreateSamplerState(&sampDesc, m_pSampLinear.ReleaseAndGetAddressOf())))
    return false;

  // Nearest neighbor sampler
  sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

  if (FAILED(DX::DeviceResources::Get()->GetD3DDevice()->CreateSamplerState(
          &sampDesc, m_pSampNearestNeighbor.ReleaseAndGetAddressOf())))
    return false;

  SetSamplers();

  return true;
}

void CGUIShaderDX::SetSamplers()
{
  // Slot 0: linear sampler
  // Slot 1: nearest neighbor sampler
  ID3D11SamplerState* samplers[] = {m_pSampLinear.Get(), m_pSampNearestNeighbor.Get()};
  DX::DeviceResources::Get()->GetD3DContext()->PSSetSamplers(0, 2, samplers);
}

void CGUIShaderDX::ApplyStateBlock(void)
{
  if (!m_bCreated)
    return;

  FlushBatch(BatchFlush::STATE_BLOCK);

  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();

  m_shaders[m_currentShader].m_vs->BindShader();
  pContext->VSSetConstantBuffers(0, 1, m_pWVPBuffer.GetAddressOf());

  m_shaders[m_currentShader].m_ps.BindShader();
  pContext->PSSetConstantBuffers(0, 1, m_pWVPBuffer.GetAddressOf());
  pContext->PSSetConstantBuffers(1, 1, m_pVPBuffer.GetAddressOf());

  SetSamplers();

  RestoreInputAssembler();
}

void CGUIShaderDX::Begin(unsigned int flags)
{
  if (!m_bCreated)
    return;

  // fonts and the other draws set state of their own after this, some of it directly on the context
  FlushBatch(BatchFlush::BEGIN);

  if (flags >= SHADER_METHOD_RENDER_COUNT)
  {
    CLog::LogF(LOGERROR, "Invalid shader method count {}.", flags);
    return;
  }

  if (m_currentShader != flags)
  {
    m_currentShader = flags;
    m_shaders[m_currentShader].m_vs->BindShader();
    m_shaders[m_currentShader].m_ps.BindShader();
  }
  ClipToScissorParams();
}

void CGUIShaderDX::End()
{
  if (!m_bCreated)
    return;
}

void CGUIShaderDX::DrawQuad(Vertex& v1, Vertex& v2, Vertex& v3, Vertex& v4)
{
  if (!m_bCreated)
    return;

  FlushBatch(BatchFlush::DRAW);
  ApplyChanges();

  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();

  // update vertex buffer
  D3D11_MAPPED_SUBRESOURCE resource;
  if (SUCCEEDED(pContext->Map(m_pVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &resource)))
  {
    // we are using strip topology
    Vertex vertices[4] = { v2, v3, v1, v4 };
    memcpy(resource.pData, &vertices, sizeof(Vertex) * 4);
    pContext->Unmap(m_pVertexBuffer.Get(), 0);
    // Draw primitives
    pContext->Draw(4, 0);
    ++CRenderSystemBase::m_GUIElementCount;
    ++m_batchStats.otherDraws;
  }
}

void CGUIShaderDX::DrawGUIQuads(ID3D11Buffer* vertexBuffer,
                                unsigned int quadCount,
                                const QuadConstants& constants)
{
  if (!HasQuadPath() || !vertexBuffer || quadCount == 0 || quadCount > MAX_GUI_QUADS)
    return;

  FlushBatch(BatchFlush::DRAW);
  ApplyChanges();

  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();
  D3D11_MAPPED_SUBRESOURCE res;
  if (FAILED(pContext->Map(m_pQuadCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res)))
    return;
  memcpy(res.pData, &constants, sizeof(constants));
  pContext->Unmap(m_pQuadCB.Get(), 0);

  m_vertexShaderQuad.BindShader();
  pContext->VSSetConstantBuffers(2, 1, m_pQuadCB.GetAddressOf());
  const UINT stride = sizeof(QuadVertex);
  const UINT offset = 0;
  pContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
  pContext->IASetIndexBuffer(m_pQuadIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
  pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  pContext->DrawIndexed(quadCount * 6, 0, 0);
  ++CRenderSystemBase::m_GUIElementCount;
  ++m_batchStats.textures;
  m_batchStats.quads += quadCount;
  ++m_batchStats.draws;

  // restore what the other GUI draw paths expect
  m_shaders[m_currentShader].m_vs->BindShader();
  RestoreInputAssembler();
}

void CGUIShaderDX::BeginFrame(bool limitedColorRange, bool batchQuads)
{
  FlushBatch(BatchFlush::FRAME);
  m_batchEnabled = batchQuads;
  ++m_batchStats.frames;
  // the command lists of QUARANTINE_FRAMES frames ago, the last ones that could sample a freed
  // region, have been executed
  CTextureAtlasDX::Get().BeginFrame();
  const auto now = std::chrono::steady_clock::now();
  if (now - m_batchLogTime >= std::chrono::seconds(10))
  {
    LogBatchStats();
    m_batchLogTime = now;
  }

  const float blackLevel = limitedColorRange ? 16.f / 255.f : 0.f;
  const float colorRange = limitedColorRange ? (235.f - 16.f) / 255.f : 1.0f;
  const int PQ = DX::Windowing()->IsTransferPQ() ? 1 : 0;
  const float sdrPeakLum =
      PQ ? 10000.0f / std::max(1.0f, DX::Windowing()->GetGuiSdrPeakLuminance()) : m_sdrPeakLum;
  if (blackLevel != m_blackLevel || colorRange != m_colorRange || PQ != m_PQ ||
      sdrPeakLum != m_sdrPeakLum)
  {
    m_blackLevel = blackLevel;
    m_colorRange = colorRange;
    m_PQ = PQ;
    m_sdrPeakLum = sdrPeakLum;
    m_bIsWVPDirty = true;
  }
}

void CGUIShaderDX::DrawIndexed(unsigned int indexCount, unsigned int startIndex, unsigned int startVertex)
{
  if (!m_bCreated)
    return;

  FlushBatch(BatchFlush::DRAW);
  ApplyChanges();
  DX::DeviceResources::Get()->GetD3DContext()->DrawIndexed(indexCount, startIndex, startVertex);
  ++CRenderSystemBase::m_GUIElementCount;
  ++m_batchStats.otherDraws;
}

void CGUIShaderDX::Draw(unsigned int vertexCount, unsigned int startVertex)
{
  if (!m_bCreated)
    return;

  FlushBatch(BatchFlush::DRAW);
  ApplyChanges();
  DX::DeviceResources::Get()->GetD3DContext()->Draw(vertexCount, startVertex);
  ++CRenderSystemBase::m_GUIElementCount;
  ++m_batchStats.otherDraws;
}

void CGUIShaderDX::SetShaderViews(unsigned int numViews, ID3D11ShaderResourceView** views)
{
  if (!m_bCreated)
    return;

  FlushBatch(BatchFlush::VIEWS);
  DX::DeviceResources::Get()->GetD3DContext()->PSSetShaderResources(0, numViews, views);
}

void CGUIShaderDX::Release()
{
  // queued quads are dropped, not drawn: the context may be gone
  m_batch.clear();
  m_batchViews[0] = nullptr;
  m_batchViews[1] = nullptr;
  m_batchState = {};
  m_pBatchCornerBuffer = nullptr;
  m_pBatchIndexBuffer = nullptr;
  m_pBatchInstanceBuffer = nullptr;
  m_hasBatchPath = false;
  m_pQuadCB = nullptr;
  m_pQuadIndexBuffer = nullptr;
  m_hasQuadPath = false;
  m_pVertexBuffer = nullptr;
  m_pWVPBuffer = nullptr;
  m_pVPBuffer = nullptr;
  m_pSampLinear = nullptr;
  m_pSampNearestNeighbor = nullptr;
  m_bCreated = false;
}

void CGUIShaderDX::SetViewPort(D3D11_VIEWPORT viewPort)
{
  if (!m_pVPBuffer)
    return;

  if ( viewPort.TopLeftX != m_cbViewPort.TopLeftX
    || viewPort.TopLeftY != m_cbViewPort.TopLeftY
    || viewPort.Width    != m_cbViewPort.Width
    || viewPort.Height   != m_cbViewPort.Height)
  {
    FlushBatch(BatchFlush::VIEWPORT);
    m_cbViewPort.TopLeftX = viewPort.TopLeftX;
    m_cbViewPort.TopLeftY = viewPort.TopLeftY;
    m_cbViewPort.Width = viewPort.Width;
    m_cbViewPort.Height = viewPort.Height;
    m_bIsVPDirty = true;
  }
}

void CGUIShaderDX::Project(float& x, float& y, float& z) const
{
  const XMVECTOR V = XMVectorSet(x, y, z, .0f);

  const XMVECTOR screenCoord =
      XMVector3Project(V, m_cbViewPort.TopLeftX, m_cbViewPort.TopLeftY, m_cbViewPort.Width,
                       m_cbViewPort.Height, 0.0f, 1.0f, m_cbWorldViewProj.GetProjection(),
                       m_cbWorldViewProj.GetView(), m_cbWorldViewProj.GetWorld());

  x = XMVectorGetX(screenCoord);
  y = XMVectorGetY(screenCoord);
  z = .0f;
}

// The camera is set again for every window and camera control: an unchanged matrix keeps the batch
// (and cbWorld) as they are.
void XM_CALLCONV CGUIShaderDX::SetWVP(const XMMATRIX& w, const XMMATRIX& v, const XMMATRIX& p)
{
  if (SameMatrix(w, m_cbWorldViewProj.GetWorld()) && SameMatrix(v, m_cbWorldViewProj.GetView()) &&
      SameMatrix(p, m_cbWorldViewProj.GetProjection()))
    return;
  FlushBatch(BatchFlush::MATRIX);
  m_bIsWVPDirty = true;
  m_cbWorldViewProj.SetWVP(w, v, p);
}

void CGUIShaderDX::SetWorld(const XMMATRIX& value)
{
  if (SameMatrix(value, m_cbWorldViewProj.GetWorld()))
    return;
  FlushBatch(BatchFlush::MATRIX);
  m_bIsWVPDirty = true;
  m_cbWorldViewProj.SetWorld(value);
}

void CGUIShaderDX::SetView(const XMMATRIX& value)
{
  if (SameMatrix(value, m_cbWorldViewProj.GetView()))
    return;
  FlushBatch(BatchFlush::MATRIX);
  m_bIsWVPDirty = true;
  m_cbWorldViewProj.SetView(value);
}

void CGUIShaderDX::SetProjection(const XMMATRIX& value)
{
  if (SameMatrix(value, m_cbWorldViewProj.GetProjection()))
    return;
  FlushBatch(BatchFlush::MATRIX);
  m_bIsWVPDirty = true;
  m_cbWorldViewProj.SetProjection(value);
}

void CGUIShaderDX::SetDepth(const float depth)
{
  // batched quads carry their own depth: this is a draw outside the batch (fonts, DrawQuad)
  FlushBatch(BatchFlush::DEPTH);
  if (depth == m_depth)
    return;
  m_bIsWVPDirty = true;
  m_depth = depth;
}

void CGUIShaderDX::SetShaderClip(float x1, float y1, float x2, float y2)
{
  FlushBatch(BatchFlush::CLIP);
  if (m_shaderClip.x == x1 && m_shaderClip.y == y1 && m_shaderClip.z == x2 && m_shaderClip.w == y2)
    return;
  m_bIsWVPDirty = true;
  m_shaderClip.x = x1;
  m_shaderClip.y = y1;
  m_shaderClip.z = x2;
  m_shaderClip.w = y2;
}

void CGUIShaderDX::SetTexStep(float stepX, float stepY, float stepX2, float stepY2)
{
  FlushBatch(BatchFlush::CLIP);
  if (m_texStep.x == stepX && m_texStep.y == stepY && m_texStep2.x == stepX2 &&
      m_texStep2.y == stepY2)
    return;
  m_bIsWVPDirty = true;
  m_texStep.x = stepX;
  m_texStep.y = stepY;
  m_texStep2.x = stepX2;
  m_texStep2.y = stepY2;
}

void CGUIShaderDX::ApplyChanges(void)
{
  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();
  if (pContext == nullptr)
  {
    CLog::LogF(LOGERROR, "Unable to retrieve device context.");
    return;
  }

  D3D11_MAPPED_SUBRESOURCE res;

  if (m_bIsWVPDirty &&
      SUCCEEDED(pContext->Map(m_pWVPBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res)))
  {
    cbWorld* buffer = (cbWorld*)res.pData;

    // Vertex shader constants
    buffer->wvp = m_cbWorldViewProj.GetWVP();

    buffer->m_shaderClip = m_shaderClip;
    buffer->m_texStep = m_texStep;
    buffer->m_texStep2 = m_texStep2;

    // Translate from GL convention (-1 far 1 near) to D3D (0 far 1 near)
    buffer->depth = m_depth / 2.f + 0.5f;

    // Pixel shader constants
    // output constants are read once per frame in BeginFrame()
    buffer->blackLevel = m_blackLevel;
    buffer->colorRange = m_colorRange;
    buffer->sdrPeakLum = m_sdrPeakLum;
    buffer->PQ = m_PQ;

    pContext->Unmap(m_pWVPBuffer.Get(), 0);

    m_bIsWVPDirty = false;
  }

  // update view port buffer
  if (m_bIsVPDirty &&
      SUCCEEDED(pContext->Map(m_pVPBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res)))
  {
    *(cbViewPort*)res.pData = m_cbViewPort;
    pContext->Unmap(m_pVPBuffer.Get(), 0);
    m_bIsVPDirty = false;
  }
}

void CGUIShaderDX::RestoreBuffers(void)
{
  FlushBatch(BatchFlush::DRAW);
  RestoreInputAssembler();
}

void CGUIShaderDX::RestoreInputAssembler()
{
  const unsigned stride = sizeof(Vertex);
  const unsigned offset = 0;

  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();
  // Set the vertex buffer to active in the input assembler so it can be rendered.
  pContext->IASetVertexBuffers(0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset);
  // Set the type of primitive that should be rendered from this vertex buffer, in this case triangles.
  pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

void CGUIShaderDX::ClipToScissorParams(void)
{
  CRect viewPort; // absolute positions of corners
  DX::Windowing()->GetViewPort(viewPort);

  // get current GUI transform
  const TransformMatrix &guiMatrix = CServiceBroker::GetWinSystem()->GetGfxContext().GetGUIMatrix();
  // get current GPU transforms
  XMFLOAT4X4 world, view, projection;
  XMStoreFloat4x4(&world, m_cbWorldViewProj.GetWorld());
  XMStoreFloat4x4(&view, m_cbWorldViewProj.GetView());
  XMStoreFloat4x4(&projection, m_cbWorldViewProj.GetProjection());

  m_clipPossible = guiMatrix.m[0][1] == 0 &&
    guiMatrix.m[1][0]  == 0 &&
    guiMatrix.m[2][0]  == 0 &&
    guiMatrix.m[2][1]  == 0 &&
    view.m[0][1]       == 0 &&
    view.m[0][2]       == 0 &&
    view.m[1][0]       == 0 &&
    view.m[1][2]       == 0 &&
    view.m[2][0]       == 0 &&
    view.m[2][1]       == 0 &&
    projection.m[0][1] == 0 &&
    projection.m[0][2] == 0 &&
    projection.m[0][3] == 0 &&
    projection.m[1][0] == 0 &&
    projection.m[1][2] == 0 &&
    projection.m[1][3] == 0 &&
    projection.m[3][0] == 0 &&
    projection.m[3][1] == 0 &&
    projection.m[3][3] == 0;

  m_clipXFactor = 0.0f;
  m_clipXOffset = 0.0f;
  m_clipYFactor = 0.0f;
  m_clipYOffset = 0.0f;

  if (m_clipPossible)
  {
    m_clipXFactor = guiMatrix.m[0][0] * view.m[0][0] * projection.m[0][0];
    m_clipXOffset = (guiMatrix.m[0][3] * view.m[0][0] + view.m[3][0]) * projection.m[0][0];
    m_clipYFactor = guiMatrix.m[1][1] * view.m[1][1] * projection.m[1][1];
    m_clipYOffset = (guiMatrix.m[1][3] * view.m[1][1] + view.m[3][1]) * projection.m[1][1];

    // projection.m[2][3] is the projection type. 0 = orthographic, 1 or -1 = perspective
    // Orthographic projection: no perspective divide; projection.m[0][0]/m[1][1] already
    // encode the full NDC scale, apply only the viewport mapping.
    // Same treatment for broken transform stacks.
    const float clipW = (guiMatrix.m[2][3] * view.m[2][2] + view.m[3][2]) * projection.m[2][3];
    const float divisorW = (projection.m[2][3] == 0 || clipW == 0) ? 1.0f : clipW;
    const float xMult = (viewPort.x2 - viewPort.x1) / (2 * divisorW);
    const float yMult = (viewPort.y1 - viewPort.y2) /
                        (2 * divisorW); // correct for inverted window coordinate scheme

    m_clipXFactor = m_clipXFactor * xMult;
    m_clipXOffset = m_clipXOffset * xMult + (viewPort.x2 + viewPort.x1) / 2;
    m_clipYFactor = m_clipYFactor * yMult;
    m_clipYOffset = m_clipYOffset * yMult + (viewPort.y2 + viewPort.y1) / 2;
  }
}

bool CGUIShaderDX::QueueGUIQuads(unsigned int method,
                                 ID3D11ShaderResourceView* view0,
                                 ID3D11ShaderResourceView* view1,
                                 const QuadGeometry* quads,
                                 unsigned int count,
                                 const QuadParams& params)
{
  if (!HasBatchPath() || !quads || count == 0 || count > BATCH_CAPACITY ||
      method >= SHADER_METHOD_RENDER_COUNT)
    return false;

  // a run goes on while the method and the views stay; nothing is reordered
  if (!m_batch.empty() && (method != m_batchMethod || view0 != m_batchViews[0].Get() ||
                           view1 != m_batchViews[1].Get()))
    FlushBatch(BatchFlush::TEXTURE);
  else if (m_batch.size() + count > BATCH_CAPACITY)
    FlushBatch(BatchFlush::FULL);

  if (m_batch.empty())
  {
    m_batchMethod = method;
    m_batchViews[0] = view0;
    m_batchViews[1] = view1;
    if constexpr (VALIDATE_BATCHES)
      GetBatchState(m_batchState);
  }

  QuadInstance instance;
  instance.params = params;
  // Translate from GL convention (-1 far 1 near) to D3D (0 far 1 near) exactly as ApplyChanges() does
  // for cbWorld: the front-to-back pass and the focus overlays rely on the control depth
  instance.params.depth = params.depth / 2.f + 0.5f;
  for (unsigned int i = 0; i < count; i++)
  {
    instance.geometry = quads[i];
    m_batch.push_back(instance);
  }

  ++m_batchStats.textures;
  m_batchStats.quads += count;
  ++CRenderSystemBase::m_GUIElementCount;
  return true;
}

void CGUIShaderDX::FlushBatch(BatchFlush reason)
{
  if (m_batch.empty())
    return;

  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();
  if (!pContext)
  {
    m_batch.clear();
    m_batchViews[0] = nullptr;
    m_batchViews[1] = nullptr;
    return;
  }

  if constexpr (VALIDATE_BATCHES)
  {
    BatchState state;
    GetBatchState(state);
    if (state.blend != m_batchState.blend || state.depthStencil != m_batchState.depthStencil ||
        state.rasterizer != m_batchState.rasterizer || state.target != m_batchState.target ||
        state.depthView != m_batchState.depthView ||
        memcmp(&state.scissor, &m_batchState.scissor, sizeof(state.scissor)) != 0 ||
        memcmp(&state.viewport, &m_batchState.viewport, sizeof(state.viewport)) != 0)
      CLog::LogF(LOGERROR, "state changed under {} queued quads without a flush (flushed for {})",
                 m_batch.size(), GetBatchFlushName(reason));
    m_batchState = {};
  }

  ApplyChanges();

  m_vertexShaderBatch.BindShader();
  if (m_currentShader != m_batchMethod)
  {
    m_currentShader = m_batchMethod;
    m_shaders[m_currentShader].m_ps.BindShader();
  }
  // the multi-texture methods read the diffuse texture from t1
  const bool diffuse = m_batchMethod == SHADER_METHOD_RENDER_MULTI_TEXTURE_BLEND ||
                       m_batchMethod == SHADER_METHOD_RENDER_MULTI_TEXTURE_BLEND_NEAREST;
  ID3D11ShaderResourceView* views[] = {m_batchViews[0].Get(), m_batchViews[1].Get()};
  pContext->PSSetShaderResources(0, diffuse ? 2 : 1, views);
  pContext->IASetIndexBuffer(m_pBatchIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
  pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  ID3D11Buffer* buffers[] = {m_pBatchCornerBuffer.Get(), m_pBatchInstanceBuffer.Get()};
  const UINT strides[] = {BATCH_CORNER_STRIDE, sizeof(QuadInstance)};
  size_t done = 0;
  while (done < m_batch.size())
  {
    // The first map of a command list must discard (deferred contexts), later ones append behind
    // the instances already drawn; a full buffer starts over with a discard.
    const bool discard = m_batchDiscard || !m_batchNoOverwrite || m_batchOffset >= BATCH_CAPACITY;
    if (discard)
      m_batchOffset = 0;
    const UINT count = static_cast<UINT>(
        std::min<size_t>(m_batch.size() - done, BATCH_CAPACITY - m_batchOffset));
    D3D11_MAPPED_SUBRESOURCE res;
    const HRESULT hr =
        pContext->Map(m_pBatchInstanceBuffer.Get(), 0,
                      discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &res);
    if (FAILED(hr))
    {
      if (discard)
      {
        CLog::LogF(LOGERROR, "failed to map the GUI quad instance buffer ({:x})",
                   static_cast<uint32_t>(hr));
        break;
      }
      CLog::LogF(LOGWARNING, "NO_OVERWRITE maps unsupported ({:x}), every GUI quad batch discards.",
                 static_cast<uint32_t>(hr));
      m_batchNoOverwrite = false;
      continue;
    }
    memcpy(static_cast<QuadInstance*>(res.pData) + m_batchOffset, m_batch.data() + done,
           count * sizeof(QuadInstance));
    pContext->Unmap(m_pBatchInstanceBuffer.Get(), 0);
    m_batchDiscard = false;

    const UINT offsets[] = {0, m_batchOffset * static_cast<UINT>(sizeof(QuadInstance))};
    pContext->IASetVertexBuffers(0, 2, buffers, strides, offsets);
    pContext->DrawIndexedInstanced(6, count, 0, 0, 0);
    ++m_batchStats.draws;
    m_batchOffset += count;
    done += count;
  }
  ++m_batchStats.flushes;
  ++m_batchStats.reasons[static_cast<size_t>(reason)];

  m_batch.clear();
  m_batchViews[0] = nullptr;
  m_batchViews[1] = nullptr;

  // restore what the other GUI draw paths expect
  m_shaders[m_currentShader].m_vs->BindShader();
  RestoreInputAssembler();
}

void CGUIShaderDX::EndCommandList()
{
  FlushBatch(BatchFlush::COMMAND_LIST);
  // a deferred context needs a discard before the first NO_OVERWRITE map of each command list
  m_batchDiscard = true;
}

void CGUIShaderDX::GetBatchState(BatchState& state) const
{
  ComPtr<ID3D11DeviceContext> pContext = DX::DeviceResources::Get()->GetD3DContext();
  if (!pContext)
    return;

  FLOAT blendFactor[4];
  UINT sampleMask = 0;
  UINT stencilRef = 0;
  pContext->OMGetBlendState(state.blend.ReleaseAndGetAddressOf(), blendFactor, &sampleMask);
  pContext->OMGetDepthStencilState(state.depthStencil.ReleaseAndGetAddressOf(), &stencilRef);
  pContext->RSGetState(state.rasterizer.ReleaseAndGetAddressOf());
  pContext->OMGetRenderTargets(1, state.target.ReleaseAndGetAddressOf(),
                               state.depthView.ReleaseAndGetAddressOf());
  UINT count = 1;
  pContext->RSGetScissorRects(&count, &state.scissor);
  if (count == 0)
    state.scissor = {};
  count = 1;
  pContext->RSGetViewports(&count, &state.viewport);
  if (count == 0)
    state.viewport = {};
}

void CGUIShaderDX::LogBatchStats()
{
  const BatchStats& now = m_batchStats;
  const BatchStats& last = m_batchStatsLogged;
  std::string reasons;
  for (size_t i = 0; i < now.reasons.size(); i++)
  {
    if (now.reasons[i] != last.reasons[i])
      reasons += StringUtils::Format(" {}={}", GetBatchFlushName(static_cast<BatchFlush>(i)),
                                     now.reasons[i] - last.reasons[i]);
  }
  const uint64_t textures = now.textures - last.textures;
  const uint64_t draws = now.draws - last.draws;
  const uint64_t frames = now.frames - last.frames;
  const uint64_t otherDraws = now.otherDraws - last.otherDraws;
  const CTextureAtlasDX::Stats atlas = CTextureAtlasDX::Get().GetStats();
  CLog::Log(LOGDEBUG,
            "GUI quads ({}): {} frames, {} textures, {} quads, {} draws ({:.2f} textures per draw), "
            "{} other draws ({:.1f} draws per frame), {} flushes, flush reasons:{}; atlas: {} pages "
            "({} MiB, {:.0f}% used), {} regions, {} fallbacks, {} copies, {} range fallbacks",
            HasBatchPath() ? "batched" : "per texture", frames, textures, now.quads - last.quads,
            draws, draws ? static_cast<double>(textures) / static_cast<double>(draws) : 0.0,
            otherDraws,
            frames ? static_cast<double>(draws + otherDraws) / static_cast<double>(frames) : 0.0,
            now.flushes - last.flushes, reasons.empty() ? " none" : reasons, atlas.pages,
            atlas.bytes >> 20,
            atlas.bytes ? 400.0 * static_cast<double>(atlas.usedTexels) / atlas.bytes : 0.0,
            atlas.regions, atlas.fallbacks, atlas.copies, atlas.rangeFallbacks);
  m_batchStatsLogged = m_batchStats;
}

const char* CGUIShaderDX::GetBatchFlushName(BatchFlush reason)
{
  static constexpr std::array<const char*, static_cast<size_t>(BatchFlush::COUNT)> names = {
      "texture", "full", "begin", "views", "depth", "matrix", "clip", "viewport", "draw",
      "stateblock", "blend", "scissor", "depthculling", "target", "clear", "stereo", "present",
      "external", "commandlist", "frame"};
  static_assert(names.back() != nullptr, "every flush reason needs a name");
  const auto index = static_cast<size_t>(reason);
  return index < names.size() ? names[index] : "?";
}

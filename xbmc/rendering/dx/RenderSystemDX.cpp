/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "RenderSystemDX.h"

#include "ServiceBroker.h"
#include "application/Application.h"

#if defined(TARGET_WINDOWS_DESKTOP)
#include "cores/RetroPlayer/process/windows/RPProcessInfoWin.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPWinRenderer.h"
#endif
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DXVA.h"
#if defined(TARGET_WINDOWS_STORE)
#include "cores/VideoPlayer/Process/windows/ProcessInfoWin10.h"
#else
#include "cores/VideoPlayer/Process/windows/ProcessInfoWin.h"
#endif
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/WinRenderer.h"
#include "guilib/D3DResource.h"
#include "guilib/GUIShaderDX.h"
#include "guilib/GUITextureD3D.h"
#include "guilib/GUIWindowManager.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

#include <mutex>

#include <DirectXPackedVector.h>

extern "C"
{
#include <libavutil/pixfmt.h>
}

using namespace KODI;
using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace Microsoft::WRL;
using namespace std::chrono_literals;

CRenderSystemDX::CRenderSystemDX() : CRenderSystemBase()
{
  m_bVSync = true;

  memset(&m_viewPort, 0, sizeof m_viewPort);
  memset(&m_scissor, 0, sizeof m_scissor);
}

CRenderSystemDX::~CRenderSystemDX() = default;

bool CRenderSystemDX::InitRenderSystem()
{
  m_bVSync = true;

  // check various device capabilities
  CheckDeviceCaps();

  if (!CreateStates() || !InitGUIShader())
    return false;

  m_bRenderCreated = true;
  m_deviceResources->RegisterDeviceNotify(this);

  // register platform dependent objects
#if defined(TARGET_WINDOWS_STORE)
  VIDEOPLAYER::CProcessInfoWin10::Register();
#else
  VIDEOPLAYER::CProcessInfoWin::Register();
#endif
  CDVDFactoryCodec::ClearHWAccels();
  DXVA::CDecoder::Register();
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CWinRenderer::Register();
#if defined(TARGET_WINDOWS_DESKTOP)
  RETRO::CRPProcessInfoWin::Register();
  RETRO::CRPProcessInfoWin::RegisterRendererFactory(new RETRO::CWinRendererFactory);
#endif
  m_viewPort = m_deviceResources->GetScreenViewport();
  RestoreViewPort();

  auto outputSize = m_deviceResources->GetOutputSize();
  // set camera to center of screen
  CPoint camPoint = { outputSize.Width * 0.5f, outputSize.Height * 0.5f };
  SetCameraPosition(camPoint, outputSize.Width, outputSize.Height);

  const DXGI_ADAPTER_DESC AIdentifier = m_deviceResources->GetAdapterDesc();
  m_RenderRenderer = KODI::PLATFORM::WINDOWS::FromW(AIdentifier.Description);
  uint32_t version = 0;
  for (uint32_t decimal = m_deviceResources->GetDeviceFeatureLevel() >> 8, round = 0; decimal > 0; decimal >>= 4, ++round)
    version += (decimal % 16) * std::pow(10, round);
  m_RenderVersion = StringUtils::Format("{:.1f}", static_cast<float>(version) / 10.0f);

  CGUITextureD3D::Register();

  return true;
}

void CRenderSystemDX::OnResize()
{
  if (!m_bRenderCreated)
    return;

  auto outputSize = m_deviceResources->GetOutputSize();

  // set camera to center of screen
  CPoint camPoint = { outputSize.Width * 0.5f, outputSize.Height * 0.5f };
  SetCameraPosition(camPoint, outputSize.Width, outputSize.Height);

  CheckInterlacedStereoView();
}

bool CRenderSystemDX::IsFormatSupport(DXGI_FORMAT format, unsigned int usage) const
{
  ComPtr<ID3D11Device1> pD3DDev = m_deviceResources->GetD3DDevice();
  UINT supported;
  pD3DDev->CheckFormatSupport(format, &supported);
  return (supported & usage) != 0;
}

bool CRenderSystemDX::DestroyRenderSystem()
{
  std::unique_lock lock(m_resourceSection);

  if (m_pGUIShader)
  {
    m_pGUIShader->End();
    delete m_pGUIShader;
    m_pGUIShader = nullptr;
  }
  m_rightEyeTex.Release();
  m_guiTarget.Release();
  m_guiDSV = nullptr;
  m_BlendPremulState = nullptr;
  m_guiTargetBound = false;
  m_compositeActive = false;
  m_BlendEnableState = nullptr;
  m_BlendDisableState = nullptr;
  m_RSScissorDisable = nullptr;
  m_RSScissorEnable = nullptr;
  m_depthStencilState = nullptr;
  m_depthStencilStateRO = nullptr;
  m_depthStencilStateRW = nullptr;

  return true;
}

void CRenderSystemDX::CheckInterlacedStereoView()
{
  RenderStereoMode stereoMode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  if (m_rightEyeTex.Get() && RenderStereoMode::INTERLACED != stereoMode &&
      RenderStereoMode::CHECKERBOARD != stereoMode)
  {
    m_rightEyeTex.Release();
  }

  if (!m_rightEyeTex.Get() &&
      (RenderStereoMode::INTERLACED == stereoMode || RenderStereoMode::CHECKERBOARD == stereoMode))
  {
    const auto outputSize = m_deviceResources->GetOutputSize();
    DXGI_FORMAT texFormat = m_deviceResources->GetBackBuffer().GetFormat();
    if (!m_rightEyeTex.Create(outputSize.Width, outputSize.Height, 1, D3D11_USAGE_DEFAULT, texFormat))
    {
      CLog::LogF(LOGERROR, "Failed to create right eye buffer.");
      CServiceBroker::GetWinSystem()->GetGfxContext().SetStereoMode(
          RenderStereoMode::SPLIT_HORIZONTAL); // try fallback to split horizontal
    }
    else
      m_deviceResources->Unregister(&m_rightEyeTex); // we will handle its health
  }
}

bool CRenderSystemDX::CreateStates()
{
  auto m_pD3DDev = m_deviceResources->GetD3DDevice();
  auto m_pContext = m_deviceResources->GetD3DContext();

  if (!m_pD3DDev)
    return false;

  m_BlendEnableState = nullptr;
  m_BlendDisableState = nullptr;

  // Initialize the description of the stencil state.
  D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};

  // Set up the description of the stencil state.
  depthStencilDesc.DepthEnable = false;
  depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depthStencilDesc.DepthFunc = D3D11_COMPARISON_NEVER;
  depthStencilDesc.StencilEnable = false;
  depthStencilDesc.StencilReadMask = 0xFF;
  depthStencilDesc.StencilWriteMask = 0xFF;

  // Stencil operations if pixel is front-facing.
  depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
  depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

  // Stencil operations if pixel is back-facing.
  depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
  depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

  // Create the depth stencil state.
  HRESULT hr = m_pD3DDev->CreateDepthStencilState(&depthStencilDesc, &m_depthStencilState);
  if(FAILED(hr))
    return false;

  // Front to back pass - read & write depth
  depthStencilDesc.DepthEnable = true;
  depthStencilDesc.DepthFunc = D3D11_COMPARISON_GREATER;
  if (FAILED(m_pD3DDev->CreateDepthStencilState(&depthStencilDesc, &m_depthStencilStateRW)))
    return false;

  // Back to front pass - read depth, don't write it
  depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depthStencilDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;

  if (FAILED(m_pD3DDev->CreateDepthStencilState(&depthStencilDesc, &m_depthStencilStateRO)))
    return false;

  // Set the depth stencil state.
  m_pContext->OMSetDepthStencilState(m_depthStencilState.Get(), 0);

  D3D11_RASTERIZER_DESC rasterizerState;
  rasterizerState.CullMode = D3D11_CULL_NONE;
  rasterizerState.FillMode = D3D11_FILL_SOLID;// DEBUG - D3D11_FILL_WIREFRAME
  rasterizerState.FrontCounterClockwise = false;
  rasterizerState.DepthBias = 0;
  rasterizerState.DepthBiasClamp = 0.0f;
  rasterizerState.DepthClipEnable = true;
  rasterizerState.SlopeScaledDepthBias = 0.0f;
  rasterizerState.ScissorEnable = false;
  rasterizerState.MultisampleEnable = false;
  rasterizerState.AntialiasedLineEnable = false;

  if (FAILED(m_pD3DDev->CreateRasterizerState(&rasterizerState, &m_RSScissorDisable)))
    return false;

  rasterizerState.ScissorEnable = true;
  if (FAILED(m_pD3DDev->CreateRasterizerState(&rasterizerState, &m_RSScissorEnable)))
    return false;

  m_pContext->RSSetState(m_RSScissorDisable.Get()); // by default

  D3D11_BLEND_DESC blendState = {};
  blendState.RenderTarget[0].BlendEnable = true;
  blendState.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; // D3D11_BLEND_SRC_ALPHA;
  blendState.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; // D3D11_BLEND_INV_SRC_ALPHA;
  blendState.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blendState.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blendState.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blendState.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blendState.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

  m_pD3DDev->CreateBlendState(&blendState, &m_BlendEnableState);

  blendState.RenderTarget[0].BlendEnable = false;
  m_pD3DDev->CreateBlendState(&blendState, &m_BlendDisableState);

  // by default
  m_pContext->OMSetBlendState(m_BlendEnableState.Get(), nullptr, 0xFFFFFFFF);
  m_BlendEnabled = true;

  return true;
}

void CRenderSystemDX::PresentRender(bool rendered, bool videoLayer)
{
  if (!m_bRenderCreated)
    return;

  if (rendered && (m_stereoMode == RenderStereoMode::INTERLACED ||
                   m_stereoMode == RenderStereoMode::CHECKERBOARD))
  {
    auto m_pContext = m_deviceResources->GetD3DContext();

    // all views prepared, let's merge them before present
    m_pContext->OMSetRenderTargets(1, m_deviceResources->GetBackBuffer().GetAddressOfRTV(), m_deviceResources->GetDSV());

    auto outputSize = m_deviceResources->GetOutputSize();
    CRect destRect = { 0.0f, 0.0f, float(outputSize.Width), float(outputSize.Height) };

    SHADER_METHOD method = RenderStereoMode::INTERLACED == m_stereoMode
                               ? SHADER_METHOD_RENDER_STEREO_INTERLACED_RIGHT
                               : SHADER_METHOD_RENDER_STEREO_CHECKERBOARD_RIGHT;
    SetAlphaBlendEnable(true);
    CD3DTexture::DrawQuad(destRect, 0, &m_rightEyeTex, nullptr, method, 1.f);
    CD3DHelper::PSClearShaderResources(m_pContext);
  }

  // time for decoder that may require the context; not needed when the vsync wait happens on the
  // frame-latency object before rendering, because Present then doesn't block under the device lock
  if (!m_deviceResources->HasFrameLatencyWaitable())
  {
    std::unique_lock lock(m_decoderSection);
    XbmcThreads::EndTime<> timer;
    timer.Set(5ms);
    while (!m_decodingTimer.IsTimePast() && !timer.IsTimePast())
    {
      m_decodingEvent.wait(lock, 1ms);
    }
  }

  PresentRenderImpl(rendered);
}

void CRenderSystemDX::RequestDecodingTime()
{
  std::unique_lock lock(m_decoderSection);
  m_decodingTimer.Set(3ms);
}

void CRenderSystemDX::ReleaseDecodingTime()
{
  std::unique_lock lock(m_decoderSection);
  m_decodingTimer.SetExpired();
  m_decodingEvent.notify();
}

bool CRenderSystemDX::BeginRender()
{
  if (!m_bRenderCreated)
    return false;

  m_limitedColorRange = CServiceBroker::GetWinSystem()->UseLimitedColor();
  if (m_pGUIShader)
    m_pGUIShader->BeginFrame(m_limitedColorRange);
  m_inScene = m_deviceResources->Begin();
  if (m_inScene && m_splashTarget && BeginGuiTarget(true))
  {
    float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_deviceResources->ClearRenderTarget(m_guiTarget.GetRenderTarget(), black);
  }
  return m_inScene;
}

bool CRenderSystemDX::EndRender()
{
  m_inScene = false;

  if (!m_bRenderCreated)
    return false;

  if (m_splashTarget && m_guiTargetBound)
  {
    EndGuiTarget();
    CompositeGuiTarget();
  }

  return true;
}

void CRenderSystemDX::InvalidateColorBuffer()
{
  if (!m_bRenderCreated)
    return;

  // the GUI target persists across frames: render over it and the last GUI frame shows through
  if (m_guiTargetBound)
  {
    ClearBuffers(0);
    return;
  }

  /* clear is not affected by stipple pattern, so we can only clear on first frame */
  if (m_stereoMode == RenderStereoMode::INTERLACED && m_stereoView == RenderStereoView::RIGHT)
    return;

  // some platforms prefer a clear, instead of rendering over
  if (GetClearFunction() == ClearFunction::FIXED_FUNCTION)
  {
    ClearBuffers(0);
    return;
  }

  if (!GetEnabledFrontToBackRendering())
    return;

  m_deviceResources->ClearDepthStencil();
}

bool CRenderSystemDX::ClearBuffers(KODI::UTILS::COLOR::Color color)
{
  if (!m_bRenderCreated)
    return false;

  float fColor[4];
  CD3DHelper::XMStoreColor(fColor, color);
  ID3D11RenderTargetView* pRTView = m_guiTargetBound
                                        ? m_guiTarget.GetRenderTarget()
                                        : m_deviceResources->GetBackBuffer().GetRenderTarget();

  if (m_stereoMode != RenderStereoMode::OFF && m_stereoMode != RenderStereoMode::MONO)
  {
    // if stereo anaglyph/tab/sbs, data was cleared when left view was rendered
    if (m_stereoView == RenderStereoView::RIGHT)
    {
      // execute command's queue
      m_deviceResources->FinishCommandList();

      // do not clear RT for anaglyph modes
      if (m_stereoMode == RenderStereoMode::ANAGLYPH_GREEN_MAGENTA ||
          m_stereoMode == RenderStereoMode::ANAGLYPH_RED_CYAN ||
          m_stereoMode == RenderStereoMode::ANAGLYPH_YELLOW_BLUE)
      {
        pRTView = nullptr;
      }
      // for interlaced/checkerboard clear view for right texture
      else if (m_stereoMode == RenderStereoMode::INTERLACED ||
               m_stereoMode == RenderStereoMode::CHECKERBOARD)
      {
        pRTView = m_rightEyeTex.GetRenderTarget();
      }
    }
  }

  if (pRTView)
  {
    const CRect clRect = GetTargetRect();

    // Unlike Direct3D 9, D3D11 ClearRenderTargetView always clears full extent of the resource view.
    // Viewport and scissor settings are not applied. So clear RT by drawing full sized rect with clear color
    if (m_ScissorsEnabled && m_scissor != clRect)
    {
      bool alphaEnabled = m_BlendEnabled;
      if (alphaEnabled)
        SetAlphaBlendEnable(false);

      CGUITextureD3D::DrawQuad(clRect, color);

      if (alphaEnabled)
        SetAlphaBlendEnable(true);
    }
    else
    {
      m_deviceResources->ClearRenderTarget(pRTView, fColor);
      m_guiFullClear = m_guiFullClear || m_guiTargetBound;
    }

    // ClearRenderTargetView is no GUI draw: count an opaque clear, the composite must keep it
    if (m_guiTargetBound && (color >> 24) != 0)
      ++m_GUIElementCount;
  }

  if (GetEnabledFrontToBackRendering())
    ClearTargetDepthStencil();

  return true;
}

void CRenderSystemDX::CaptureStateBlock()
{
  if (!m_bRenderCreated)
    return;
}

void CRenderSystemDX::ApplyStateBlock()
{
  if (!m_bRenderCreated)
    return;

  auto m_pContext = m_deviceResources->GetD3DContext();

  m_pContext->RSSetState(m_ScissorsEnabled ? m_RSScissorEnable.Get() : m_RSScissorDisable.Get());
  m_pContext->OMSetDepthStencilState(GetDepthStencilState(), 0);
  float factors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  m_pContext->OMSetBlendState(m_BlendEnabled ? m_BlendEnableState.Get() : m_BlendDisableState.Get(),
                              factors, 0xFFFFFFFF);

  m_pGUIShader->ApplyStateBlock();
}

void CRenderSystemDX::SetCameraPosition(const CPoint &camera, int screenWidth, int screenHeight, float stereoFactor)
{
  if (!m_bRenderCreated)
    return;

  // grab the viewport dimensions and location
  float w = m_viewPort.Width * 0.5f;
  float h = m_viewPort.Height * 0.5f;

  XMFLOAT2 offset = XMFLOAT2(camera.x - screenWidth*0.5f, camera.y - screenHeight*0.5f);

  // world view.  Until this is moved onto the GPU (via a vertex shader for instance), we set it to the identity here.
  m_pGUIShader->SetWorld(XMMatrixIdentity());

  // Initialize the view matrix camera view.
  // Multiply the Y coord by -1 then translate so that everything is relative to the camera position.
  XMMATRIX flipY = XMMatrixScaling(1.0, -1.0f, 1.0f);
  XMMATRIX translate = XMMatrixTranslation(-(w + offset.x - stereoFactor), -(h + offset.y), 2 * h);
  m_pGUIShader->SetView(XMMatrixMultiply(translate, flipY));

  // projection onto screen space
  m_pGUIShader->SetProjection(XMMatrixPerspectiveOffCenterLH((-w - offset.x)*0.5f, (w - offset.x)*0.5f, (-h + offset.y)*0.5f, (h + offset.y)*0.5f, h, 100 * h));
}

void CRenderSystemDX::Project(float &x, float &y, float &z)
{
  if (!m_bRenderCreated)
    return;

  m_pGUIShader->Project(x, y, z);
}

CRect CRenderSystemDX::GetBackBufferRect()
{
  auto outputSize = m_deviceResources->GetOutputSize();
  return CRect(0.f, 0.f, static_cast<float>(outputSize.Width), static_cast<float>(outputSize.Height));
}

void CRenderSystemDX::GetViewPort(CRect& viewPort)
{
  if (!m_bRenderCreated)
    return;

  viewPort.x1 = m_viewPort.TopLeftX;
  viewPort.y1 = m_viewPort.TopLeftY;
  viewPort.x2 = m_viewPort.TopLeftX + m_viewPort.Width;
  viewPort.y2 = m_viewPort.TopLeftY + m_viewPort.Height;
}

void CRenderSystemDX::SetViewPort(const CRect& viewPort)
{
  if (!m_bRenderCreated)
    return;

  m_viewPort.MinDepth   = 0.0f;
  m_viewPort.MaxDepth   = 1.0f;
  m_viewPort.TopLeftX   = viewPort.x1;
  m_viewPort.TopLeftY   = viewPort.y1;
  m_viewPort.Width      = viewPort.x2 - viewPort.x1;
  m_viewPort.Height     = viewPort.y2 - viewPort.y1;

  m_deviceResources->SetViewPort(m_viewPort);
  m_pGUIShader->SetViewPort(m_viewPort);
}

void CRenderSystemDX::RestoreViewPort()
{
  if (!m_bRenderCreated)
    return;

  m_deviceResources->SetViewPort(m_viewPort);
  m_pGUIShader->SetViewPort(m_viewPort);
}

bool CRenderSystemDX::ScissorsCanEffectClipping()
{
  if (!m_bRenderCreated)
    return false;

  return m_pGUIShader != nullptr && m_pGUIShader->HardwareClipIsPossible();
}

CRect CRenderSystemDX::ClipRectToScissorRect(const CRect &rect)
{
  if (!m_bRenderCreated)
    return CRect();

  float xFactor = m_pGUIShader->GetClipXFactor();
  float xOffset = m_pGUIShader->GetClipXOffset();
  float yFactor = m_pGUIShader->GetClipYFactor();
  float yOffset = m_pGUIShader->GetClipYOffset();

  return CRect(rect.x1 * xFactor + xOffset,
               rect.y1 * yFactor + yOffset,
               rect.x2 * xFactor + xOffset,
               rect.y2 * yFactor + yOffset);
}

void CRenderSystemDX::SetScissors(const CRect& rect)
{
  if (!m_bRenderCreated)
    return;

  auto m_pContext = m_deviceResources->GetD3DContext();

  m_scissor = rect;
  CD3D11_RECT scissor(MathUtils::round_int(rect.x1)
                    , MathUtils::round_int(rect.y1)
                    , MathUtils::round_int(rect.x2)
                    , MathUtils::round_int(rect.y2));

  m_pContext->RSSetScissorRects(1, &scissor);
  m_pContext->RSSetState(m_RSScissorEnable.Get());
  m_ScissorsEnabled = true;
}

void CRenderSystemDX::ResetScissors()
{
  if (!m_bRenderCreated)
    return;

  auto m_pContext = m_deviceResources->GetD3DContext();

  m_scissor = GetTargetRect();

  m_pContext->RSSetState(m_RSScissorDisable.Get());
  m_ScissorsEnabled = false;
}

ID3D11DepthStencilState* CRenderSystemDX::GetDepthStencilState()
{
  switch (m_depthCulling)
  {
    case DepthCulling::BACK_TO_FRONT:
      return m_depthStencilStateRO.Get();
    case DepthCulling::FRONT_TO_BACK:
      return m_depthStencilStateRW.Get();
    default:
      return m_depthStencilState.Get();
  }
}

void CRenderSystemDX::SetDepthCulling(DepthCulling culling)
{
  if (!m_bRenderCreated)
    return;

  m_depthCulling = culling;
  m_deviceResources->GetD3DContext()->OMSetDepthStencilState(GetDepthStencilState(), 0);
}

void CRenderSystemDX::OnDXDeviceLost()
{
  CRenderSystemDX::DestroyRenderSystem();
}

void CRenderSystemDX::OnDXDeviceRestored()
{
  CRenderSystemDX::InitRenderSystem();
}

void CRenderSystemDX::SetStereoMode(RenderStereoMode mode, RenderStereoView view)
{
  CRenderSystemBase::SetStereoMode(mode, view);

  if (!m_bRenderCreated)
    return;

  auto m_pContext = m_deviceResources->GetD3DContext();

  UINT writeMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (m_stereoMode == RenderStereoMode::ANAGLYPH_RED_CYAN)
  {
    if (m_stereoView == RenderStereoView::LEFT)
      writeMask = D3D11_COLOR_WRITE_ENABLE_RED;
    else if (m_stereoView == RenderStereoView::RIGHT)
      writeMask = D3D11_COLOR_WRITE_ENABLE_BLUE | D3D11_COLOR_WRITE_ENABLE_GREEN;
  }
  if (m_stereoMode == RenderStereoMode::ANAGLYPH_GREEN_MAGENTA)
  {
    if (m_stereoView == RenderStereoView::LEFT)
      writeMask = D3D11_COLOR_WRITE_ENABLE_GREEN;
    else if (m_stereoView == RenderStereoView::RIGHT)
      writeMask = D3D11_COLOR_WRITE_ENABLE_BLUE | D3D11_COLOR_WRITE_ENABLE_RED;
  }
  if (m_stereoMode == RenderStereoMode::ANAGLYPH_YELLOW_BLUE)
  {
    if (m_stereoView == RenderStereoView::LEFT)
      writeMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    else if (m_stereoView == RenderStereoView::RIGHT)
      writeMask = D3D11_COLOR_WRITE_ENABLE_BLUE;
  }
  if (RenderStereoMode::INTERLACED == m_stereoMode ||
      RenderStereoMode::CHECKERBOARD == m_stereoMode)
  {
    if (m_stereoView == RenderStereoView::RIGHT)
    {
      m_pContext->OMSetRenderTargets(1, m_rightEyeTex.GetAddressOfRTV(), m_deviceResources->GetDSV());
    }
  }
  else if (RenderStereoMode::HARDWAREBASED == m_stereoMode)
  {
    m_deviceResources->SetStereoIdx(m_stereoView == RenderStereoView::RIGHT ? 1 : 0);

    m_pContext->OMSetRenderTargets(1, m_deviceResources->GetBackBuffer().GetAddressOfRTV(), m_deviceResources->GetDSV());
  }

  auto m_pD3DDev = m_deviceResources->GetD3DDevice();

  D3D11_BLEND_DESC desc;
  m_BlendEnableState->GetDesc(&desc);
  // update blend state
  if (desc.RenderTarget[0].RenderTargetWriteMask != writeMask)
  {
    m_BlendDisableState = nullptr;
    m_BlendEnableState = nullptr;

    desc.RenderTarget[0].RenderTargetWriteMask = writeMask;
    m_pD3DDev->CreateBlendState(&desc, &m_BlendEnableState);

    desc.RenderTarget[0].BlendEnable = false;
    m_pD3DDev->CreateBlendState(&desc, &m_BlendDisableState);

    float blendFactors[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_pContext->OMSetBlendState(m_BlendEnabled ? m_BlendEnableState.Get() : m_BlendDisableState.Get(), blendFactors, 0xFFFFFFFF);
  }
}

bool CRenderSystemDX::SupportsStereo(RenderStereoMode mode) const
{
  switch (mode)
  {
    case RenderStereoMode::ANAGLYPH_RED_CYAN:
    case RenderStereoMode::ANAGLYPH_GREEN_MAGENTA:
    case RenderStereoMode::ANAGLYPH_YELLOW_BLUE:
    case RenderStereoMode::INTERLACED:
    case RenderStereoMode::CHECKERBOARD:
      return true;
    case RenderStereoMode::HARDWAREBASED:
      return m_deviceResources->IsStereoAvailable();
    default:
      return CRenderSystemBase::SupportsStereo(mode);
  }
}

void CRenderSystemDX::FlushGPU() const
{
  if (!m_bRenderCreated)
    return;

  m_deviceResources->FinishCommandList();
  m_deviceResources->GetImmediateContext()->Flush();
}

bool CRenderSystemDX::InitGUIShader()
{
  delete m_pGUIShader;
  m_pGUIShader = nullptr;

  m_pGUIShader = new CGUIShaderDX();
  if (!m_pGUIShader->Initialize())
  {
    CLog::LogF(LOGERROR, "Failed to initialize GUI shader.");
    return false;
  }

  m_pGUIShader->ApplyStateBlock();
  return true;
}

void CRenderSystemDX::SetAlphaBlendEnable(bool enable)
{
  if (!m_bRenderCreated)
    return;

  m_deviceResources->GetD3DContext()->OMSetBlendState(enable ? m_BlendEnableState.Get() : m_BlendDisableState.Get(), nullptr, 0xFFFFFFFF);
  m_BlendEnabled = enable;
}

CD3DTexture& CRenderSystemDX::GetBackBuffer()
{
  if (m_stereoView == RenderStereoView::RIGHT && m_rightEyeTex.Get())
    return m_rightEyeTex;

  return m_deviceResources->GetBackBuffer();
}

void CRenderSystemDX::ShowSplash(const std::string& message)
{
  // the splash renders outside CApplication::Render: BeginRender/EndRender route it through the
  // GUI target as well, else it would cover only a part of the output
  m_splashTarget = CServiceBroker::GetWinSystem()->GetGfxContext().IsGuiReduced();
  CRenderSystemBase::ShowSplash(message);
  m_splashTarget = false;
}

CRect CRenderSystemDX::GetTargetRect()
{
  if (m_guiTargetBound)
    return CRect(0.0f, 0.0f, static_cast<float>(m_guiTarget.GetWidth()),
                 static_cast<float>(m_guiTarget.GetHeight()));

  return GetBackBufferRect();
}

void CRenderSystemDX::ClearTargetDepthStencil() const
{
  if (m_guiTargetBound && m_guiDSV)
    m_deviceResources->GetD3DContext()->ClearDepthStencilView(
        m_guiDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
  else
    m_deviceResources->ClearDepthStencil();
}

bool CRenderSystemDX::UpdateGuiTarget()
{
  const CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const UINT width = static_cast<UINT>(gfx.GetWidth());
  const UINT height = static_cast<UINT>(gfx.GetHeight());
  // the 2-bit alpha of a 10-bit (HDR) back buffer would ruin the GUI's translucency
  const DXGI_FORMAT format =
      m_deviceResources->GetBackBuffer().GetFormat() == DXGI_FORMAT_R10G10B10A2_UNORM
          ? DXGI_FORMAT_R16G16B16A16_FLOAT
          : DXGI_FORMAT_B8G8R8A8_UNORM;
  // the output-sized depth stencil view cannot be bound with a smaller render target
  const bool needDepth = GetEnabledFrontToBackRendering();

  if (m_guiTarget.Get() && m_guiTarget.GetWidth() == width && m_guiTarget.GetHeight() == height &&
      m_guiTarget.GetFormat() == format && (m_guiDSV != nullptr) == needDepth)
    return true;

  m_guiTarget.Release();
  m_guiDSV = nullptr;
  if (!m_guiTarget.Create(width, height, 1, D3D11_USAGE_DEFAULT, format))
  {
    CLog::LogF(LOGERROR, "failed to create the {}x{} GUI target", width, height);
    return false;
  }
  m_deviceResources->Unregister(&m_guiTarget); // we will handle its health

  auto pD3DDev = m_deviceResources->GetD3DDevice();
  if (needDepth)
  {
    const CD3D11_TEXTURE2D_DESC depthDesc(DXGI_FORMAT_D24_UNORM_S8_UINT, width, height, 1, 1,
                                          D3D11_BIND_DEPTH_STENCIL);
    const CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc(D3D11_DSV_DIMENSION_TEXTURE2D);
    ComPtr<ID3D11Texture2D> depthStencil;
    if (FAILED(pD3DDev->CreateTexture2D(&depthDesc, nullptr, &depthStencil)) ||
        FAILED(pD3DDev->CreateDepthStencilView(depthStencil.Get(), &dsvDesc, &m_guiDSV)))
    {
      CLog::LogF(LOGERROR, "failed to create the {}x{} GUI depth stencil", width, height);
      m_guiTarget.Release();
      m_guiDSV = nullptr;
      return false;
    }
  }

  if (!m_BlendPremulState && m_BlendEnableState)
  {
    // GUI blending into the transparent target leaves colour multiplied by alpha
    D3D11_BLEND_DESC desc;
    m_BlendEnableState->GetDesc(&desc);
    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    pD3DDev->CreateBlendState(&desc, &m_BlendPremulState);
  }

  m_guiTargetFresh = true;
  m_guiClean = true;
  const auto outputSize = m_deviceResources->GetOutputSize();
  CLog::LogF(LOGINFO, "GUI target {}x{} ({}) for a {}x{} output", width, height,
             format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "FP16" : "BGRA8",
             static_cast<int>(outputSize.Width), static_cast<int>(outputSize.Height));
  return true;
}

bool CRenderSystemDX::BeginGuiTarget(bool guiWillRender)
{
  if (!m_bRenderCreated)
    return false;

  CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  if (!gfx.IsGuiReduced())
  {
    // the output is no taller than the threshold (any more)
    if (m_guiTarget.Get())
    {
      m_guiTarget.Release();
      m_guiDSV = nullptr;
    }
    return false;
  }

  if (!UpdateGuiTarget())
    return false;

  // GL resets the count in BeginRender, DX did not: CompositeGuiTarget skips an empty GUI
  m_GUIElementCount = 0;
  m_guiWillRender = guiWillRender;
  m_guiFullClear = false;
  m_videoDrawn = false;

  // a skipped GUI walk keeps the target's last frame for the composite
  if (guiWillRender)
  {
    m_deviceResources->GetD3DContext()->OMSetRenderTargets(1, m_guiTarget.GetAddressOfRTV(),
                                                           m_guiDSV.Get());
    m_guiTargetBound = true;
    if (m_guiTargetFresh)
    {
      // in full: ClearBuffers would honour the scissors
      float transparent[4] = {};
      m_deviceResources->ClearRenderTarget(m_guiTarget.GetRenderTarget(), transparent);
      m_guiTargetFresh = false;
    }
    // the output-sized state of the last composite is still set
    gfx.RestoreGuiRenderState();
  }

  return true;
}

void CRenderSystemDX::SetOutputRenderState()
{
  const CRect output = GetBackBufferRect();
  SetViewPort(output);
  SetCameraPosition(CPoint(output.Width() * 0.5f, output.Height() * 0.5f),
                    static_cast<int>(output.Width()), static_cast<int>(output.Height()));
  // not the gfx scissors, they are in GUI pixels
  ResetScissors();
  SetDepthCulling(DepthCulling::OFF);
}

void CRenderSystemDX::EndGuiTarget()
{
  CD3DTexture& backBuffer = m_deviceResources->GetBackBuffer();
  m_deviceResources->GetD3DContext()->OMSetRenderTargets(1, backBuffer.GetAddressOfRTV(),
                                                         m_deviceResources->GetDSV());
  m_guiTargetBound = false;
  m_compositeActive = true;
  SetOutputRenderState();

  // the RenderEx pass renders video with clear=false, nothing draws its black bars
  const float black = m_limitedColorRange ? 16.0f / 255.0f : 0.0f;
  float color[4] = {black, black, black, 1.0f};
  m_deviceResources->ClearRenderTarget(backBuffer.GetRenderTarget(), color);
}

void CRenderSystemDX::CompositeGuiTarget()
{
  if (!m_compositeActive)
    return;

  m_compositeActive = false;
  // a skipped GUI walk leaves the target, and whether it is empty, as it was; dirty regions render
  // partially, so nothing drawn means empty only after a full clear or on an empty target
  if (m_guiWillRender)
    m_guiClean = GetGUIElementCount() == 0 && (m_guiFullClear || m_guiClean);
  m_guiWillRender = false;
  const bool videoDrawn = m_videoDrawn;
  m_videoDrawn = false;
  if (m_guiClean)
    return;

  // the video pass may have moved the viewport, camera or scissors
  SetOutputRenderState();

  auto pContext = m_deviceResources->GetD3DContext();
  // over the video the target's premultiplied colour blends, alone it is copied
  pContext->OMSetBlendState(videoDrawn ? m_BlendPremulState.Get() : m_BlendDisableState.Get(),
                            nullptr, 0xFFFFFFFF);
  // bilinear upscale; the noblend shader applies no range or tone mapping, the GUI draws did
  CD3DTexture::DrawQuad(GetBackBufferRect(), 0xFFFFFFFF, &m_guiTarget, nullptr,
                        SHADER_METHOD_RENDER_TEXTURE_NOBLEND, 1.0f);
  CD3DHelper::PSClearShaderResources(pContext);
  pContext->OMSetBlendState(m_BlendEnabled ? m_BlendEnableState.Get() : m_BlendDisableState.Get(),
                            nullptr, 0xFFFFFFFF);
}

CRect CRenderSystemDX::GetVideoScissors()
{
  if (m_compositeActive)
    return GetBackBufferRect();

  const CGraphicContext& context = CServiceBroker::GetWinSystem()->GetGfxContext();
  return context.StereoCorrection(context.GetScissors());
}

void CRenderSystemDX::RestoreVideoScissors()
{
  // the video pass of a reduced GUI is not clipped
  if (m_compositeActive)
    ResetScissors();
  else
    SetScissors(GetVideoScissors());
}

void CRenderSystemDX::CheckDeviceCaps()
{
  const auto feature_level = m_deviceResources->GetDeviceFeatureLevel();
  if (feature_level < D3D_FEATURE_LEVEL_9_3)
    m_maxTextureSize = D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  else if (feature_level < D3D_FEATURE_LEVEL_10_0)
    m_maxTextureSize = D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  else if (feature_level < D3D_FEATURE_LEVEL_11_0)
    m_maxTextureSize = D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  else
    // 11_x and greater feature level. Limit this size to avoid memory overheads
    m_maxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION >> 1;
}

bool CRenderSystemDX::SupportsNPOT(bool dxt) const
{
  // MSDN says:
  // At feature levels 9_1, 9_2 and 9_3, the display device supports the use
  // of 2D textures with dimensions that are not powers of two under two conditions:
  // 1) only one MIP-map level for each texture can be created - we are using both 1 and 0 mipmap levels
  // 2) no wrap sampler modes for textures are allowed - we are using clamp everywhere
  // At feature levels 10_0, 10_1 and 11_0, the display device unconditionally supports the use of 2D textures with dimensions that are not powers of two.
  // taking in account first condition we setup caps NPOT for FE > 9.x only
  return m_deviceResources->GetDeviceFeatureLevel() > D3D_FEATURE_LEVEL_9_3 ? true : false;
}

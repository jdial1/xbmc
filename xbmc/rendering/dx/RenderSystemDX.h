/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DeviceResources.h"
#include "rendering/RenderSystem.h"
#include "threads/Condition.h"
#include "threads/CriticalSection.h"
#include "threads/SystemClock.h"
#include "utils/ColorUtils.h"

#include <wrl/client.h>

class ID3DResource;
class CGUIShaderDX;
enum AVPixelFormat;
enum AVPixelFormat;

class CRenderSystemDX : public CRenderSystemBase, DX::IDeviceNotify
{
public:
  CRenderSystemDX();
  virtual ~CRenderSystemDX();

  // CRenderBase overrides
  bool InitRenderSystem() override;
  bool DestroyRenderSystem() override;
  bool BeginRender() override;
  bool EndRender() override;
  void PresentRender(bool rendered, bool videoLayer) override;
  void InvalidateColorBuffer() override;
  bool ClearBuffers(KODI::UTILS::COLOR::Color color) override;
  void SetViewPort(const CRect& viewPort) override;
  void GetViewPort(CRect& viewPort) override;
  void RestoreViewPort() override;
  CRect ClipRectToScissorRect(const CRect &rect) override;
  bool ScissorsCanEffectClipping() override;
  void SetScissors(const CRect &rect) override;
  void ResetScissors() override;
  void SetDepthCulling(DepthCulling culling) override;
  void CaptureStateBlock() override;
  void ApplyStateBlock() override;
  void SetCameraPosition(const CPoint &camera, int screenWidth, int screenHeight, float stereoFactor = 0.f) override;
  void SetStereoMode(RenderStereoMode mode, RenderStereoView view) override;
  bool SupportsStereo(RenderStereoMode mode) const override;
  void Project(float &x, float &y, float &z) override;
  bool SupportsNPOT(bool dxt) const override;

  // IDeviceNotify overrides
  void OnDXDeviceLost() override;
  void OnDXDeviceRestored() override;

  // CRenderSystemDX methods
  CGUIShaderDX* GetGUIShader() const { return m_pGUIShader; }
  bool IsFormatSupport(DXGI_FORMAT format, unsigned int usage) const;
  CRect GetBackBufferRect();
  CD3DTexture& GetBackBuffer();

  void FlushGPU() const;
  void RequestDecodingTime();
  void ReleaseDecodingTime();
  void SetAlphaBlendEnable(bool enable);
  void ShowSplash(const std::string& message) override;

  // reduced-resolution GUI layer (CGraphicContext::IsGuiReduced): the GUI renders into m_guiTarget,
  // the video into the back buffer at the output resolution, then the GUI is composited over it
  bool BeginGuiTarget(bool guiWillRender);
  void EndGuiTarget();
  void CompositeGuiTarget();
  //! where GUI draws land: the GUI target while it is bound, else the back buffer
  CD3DTexture& GetGuiRenderTarget() { return m_guiTargetBound ? m_guiTarget : GetBackBuffer(); }
  bool IsGuiTargetActive() const { return m_guiTargetBound; }
  //! the video was drawn under the GUI this frame: the composite blends
  void SetVideoDrawn() { m_videoDrawn = m_videoDrawn || m_compositeActive; }
  //! scissors of the video pass (the gfx scissors are in GUI pixels while the GUI is reduced)
  CRect GetVideoScissors();
  void RestoreVideoScissors();

  // empty overrides
  bool IsExtSupported(const char* extension) const override { return false; }
  bool ResetRenderSystem(int width, int height) override { return true; }

protected:
  virtual void PresentRenderImpl(bool rendered) = 0;

  bool CreateStates();
  bool InitGUIShader();
  void OnResize();
  void CheckInterlacedStereoView(void);
  void CheckDeviceCaps();
  ID3D11DepthStencilState* GetDepthStencilState();
  bool UpdateGuiTarget();
  CRect GetTargetRect();
  void SetOutputRenderState();
  void ClearTargetDepthStencil() const;

  CCriticalSection m_resourceSection;
  CCriticalSection m_decoderSection;

  // our adapter could change as we go
  bool m_inScene{ false }; ///< True if we're in a BeginScene()/EndScene() block
  bool m_BlendEnabled{ false };
  bool m_ScissorsEnabled{ false };
  D3D11_VIEWPORT m_viewPort;
  CRect m_scissor;
  CGUIShaderDX* m_pGUIShader{ nullptr };
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilStateRO;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilStateRW;

  Microsoft::WRL::ComPtr<ID3D11BlendState> m_BlendEnableState;
  Microsoft::WRL::ComPtr<ID3D11BlendState> m_BlendDisableState;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_RSScissorDisable;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_RSScissorEnable;
  // stereo interlaced/checkerboard intermediate target
  CD3DTexture m_rightEyeTex;
  // reduced-resolution GUI layer
  CD3DTexture m_guiTarget;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_guiDSV; ///< only for front-to-back rendering
  Microsoft::WRL::ComPtr<ID3D11BlendState> m_BlendPremulState;
  bool m_guiTargetBound{false}; ///< GUI draws go to m_guiTarget (BeginGuiTarget..EndGuiTarget)
  bool m_guiWillRender{false}; ///< the GUI walk renders into m_guiTarget this frame
  bool m_compositeActive{false}; ///< video pass of a reduced GUI (EndGuiTarget..CompositeGuiTarget)
  bool m_guiClean{true}; ///< m_guiTarget holds nothing to composite
  bool m_guiFullClear{false}; ///< m_guiTarget was cleared in full this frame
  bool m_guiTargetFresh{false}; ///< m_guiTarget was just created, its content is undefined
  bool m_videoDrawn{false}; ///< video was drawn under the GUI this frame
  bool m_splashTarget{false}; ///< ShowSplash renders through m_guiTarget

  XbmcThreads::EndTime<> m_decodingTimer;
  XbmcThreads::ConditionVariable m_decodingEvent;

  std::shared_ptr<DX::DeviceResources> m_deviceResources;
  DepthCulling m_depthCulling{DepthCulling::OFF};
};

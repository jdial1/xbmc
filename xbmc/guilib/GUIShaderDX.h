/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "D3DResource.h"
#include "Texture.h"
#include "utils/Geometry.h"
#include "utils/MemUtils.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <wrl/client.h>

struct Vertex {
  Vertex() {}

  Vertex(DirectX::XMFLOAT3 p, DirectX::XMFLOAT4 c)
    : pos(p), color(c) {}

  Vertex(DirectX::XMFLOAT3 p, DirectX::XMFLOAT4 c, DirectX::XMFLOAT2 t1, DirectX::XMFLOAT2 t2)
    : pos(p), color(c), texCoord(t1), texCoord2(t2) {}

  DirectX::XMFLOAT3 pos;
  DirectX::XMFLOAT4 color;
  DirectX::XMFLOAT2 texCoord;
  DirectX::XMFLOAT2 texCoord2;
};

class ID3DResource;

class CGUIShaderDX
{
public:
  CGUIShaderDX() {}
  CGUIShaderDX(const CGUIShaderDX&) = delete;
  CGUIShaderDX& operator=(const CGUIShaderDX&) = delete;
  ~CGUIShaderDX();

  bool Initialize();

  /*!
   * \brief Activate shaders and recalculate clipping information.
   * \param flags Shader method to activate
   */
  void Begin(unsigned int flags);
  void End(void);
  void ApplyStateBlock(void);
  void RestoreBuffers(void);

  void SetShaderViews(unsigned int numViews, ID3D11ShaderResourceView** views);
  void SetViewPort(D3D11_VIEWPORT viewPort);

  void XM_CALLCONV GetWVP(DirectX::XMMATRIX& w, DirectX::XMMATRIX& v, DirectX::XMMATRIX& p)
  {
    w = m_cbWorldViewProj.GetWorld();
    v = m_cbWorldViewProj.GetView();
    p = m_cbWorldViewProj.GetProjection();
  }
  DirectX::XMMATRIX XM_CALLCONV GetWorld() const { return m_cbWorldViewProj.GetWorld(); }
  DirectX::XMMATRIX XM_CALLCONV GetView() const { return m_cbWorldViewProj.GetView(); }
  DirectX::XMMATRIX XM_CALLCONV GetProjection() const { return m_cbWorldViewProj.GetProjection(); }
  void XM_CALLCONV SetWVP(const DirectX::XMMATRIX& w,
                          const DirectX::XMMATRIX& v,
                          const DirectX::XMMATRIX& p);
  void XM_CALLCONV SetWorld(const DirectX::XMMATRIX& value);
  void XM_CALLCONV SetView(const DirectX::XMMATRIX& value);
  void XM_CALLCONV SetProjection(const DirectX::XMMATRIX& value);
  void Project(float& x, float& y, float& z) const;

  /*!
   * \brief Sets the depth value of the primitives to be drawn (overrides z of the vertices)
   * \param[in] depth value -1=far to 1=near (GL convention).
   */
  void SetDepth(float depth);

  void SetShaderClip(float x1, float y1, float x2, float y2);
  void SetTexStep(float stepX, float stepY, float stepX2, float stepY2);

  void DrawQuad(Vertex& v1, Vertex& v2, Vertex& v3, Vertex& v4);

  //! Constants of guishader_quad_vert.hlsl (cbQuad, b2)
  struct QuadConstants
  {
    DirectX::XMFLOAT4 rect; // x, y, width, height of the texture rectangle
    DirectX::XMFLOAT4 clip; // x1, y1, x2, y2
    DirectX::XMFLOAT4 gui[3]; // rows of the GUI (final) transform
    DirectX::XMFLOAT4 color;
    DirectX::XMFLOAT2 texSwap;
    float snap;
    float pad;
  };
  //! Vertex of guishader_quad_vert.hlsl: corner, opposite corner, texture and diffuse coordinates
  struct QuadVertex
  {
    float corner[4];
    float opposite[4];
    float texture[4];
    float diffuse[4];
  };
  static constexpr unsigned int MAX_GUI_QUADS = 16;
  bool HasQuadPath() const { return m_bCreated && m_hasQuadPath; }
  /*!
   * \brief Draw quadCount quads (4 QuadVertex each) from an immutable vertex buffer with the current
   * shader method, views and depth; clipping, GUI transform and pixel snapping run on the GPU.
   * This is the per-texture path, used while batching (QueueGUIQuads) is off or unavailable.
   */
  void DrawGUIQuads(ID3D11Buffer* vertexBuffer, unsigned int quadCount, const QuadConstants& constants);

  //! One quad of a texture: its edges and texture coordinates, as rectangles (x1, y1, x2, y2)
  struct QuadGeometry
  {
    DirectX::XMFLOAT4 anchor; // fractions of the texture's rectangle
    DirectX::XMFLOAT4 offset; // added to the anchored edges
    DirectX::XMFLOAT4 texture;
    DirectX::XMFLOAT4 diffuse;
  };
  //! What the quads of one texture share: the QuadConstants of DrawGUIQuads plus the depth
  struct QuadParams
  {
    DirectX::XMFLOAT4 rect; // x, y, width, height of the texture rectangle
    DirectX::XMFLOAT4 clip; // x1, y1, x2, y2
    DirectX::XMFLOAT4 gui[3]; // rows of the GUI (final) transform
    DirectX::XMFLOAT4 color;
    DirectX::XMFLOAT2 texSwap;
    float snap; // 1 = round to whole pixels, 2 = keep subpixel positions (moving texture)
    float depth; // -1 = far to 1 = near (GL convention, as SetDepth)
  };
  //! Per-instance vertex of guishader_quad_batch_vert.hlsl (slot 1); params.depth is 0 far to 1 near
  struct QuadInstance
  {
    QuadGeometry geometry;
    QuadParams params;
  };
  static constexpr unsigned int BATCH_CAPACITY = 4096; //!< quads in the instance buffer

  //! Why queued quads were drawn (FlushBatch)
  enum class BatchFlush
  {
    TEXTURE, // the next quads use another shader method or view
    FULL, // the instance buffer is full
    BEGIN, // another GUI shader draw begins (fonts, DrawQuad, the per-texture path)
    VIEWS,
    DEPTH,
    MATRIX, // world, view or projection changed
    CLIP, // shader clip or texture step (fonts)
    VIEWPORT,
    DRAW, // a GUI shader draw or buffer restore without Begin() first
    STATE_BLOCK,
    BLEND,
    SCISSOR,
    DEPTH_CULLING,
    TARGET, // reduced GUI target bound, unbound or composited
    CLEAR,
    STEREO,
    PRESENT,
    EXTERNAL, // CRenderSystemBase::FlushGUIBatch (video, RetroPlayer)
    COMMAND_LIST, // DeviceResources::FinishCommandList
    FRAME,
    COUNT
  };
  //! Counters since the shader was created; the change is logged every 10 s at debug level
  struct BatchStats
  {
    uint64_t frames = 0;
    uint64_t textures = 0; // textures drawn by QueueGUIQuads or DrawGUIQuads
    uint64_t quads = 0;
    uint64_t draws = 0; // draw calls of those textures
    uint64_t otherDraws = 0; // DrawQuad, DrawIndexed and Draw (fonts, overlays, slideshow, ...)
    uint64_t flushes = 0; // batches drawn
    std::array<uint64_t, static_cast<size_t>(BatchFlush::COUNT)> reasons{};
  };
  //! Batching is on: feature level 9_3 or higher and <gui><batchquads> is not false
  bool HasBatchPath() const { return m_bCreated && m_hasBatchPath && m_batchEnabled; }
  //! The batched path exists (whatever <gui><batchquads> says)
  bool CanBatch() const { return m_bCreated && m_hasBatchPath; }
  /*!
   * \brief Queue count quads drawn with the shader method and the views (view1: diffuse texture of the
   * multi-texture methods). Consecutive quads with the same method and views are drawn by one instanced
   * draw; the batch holds the views until then. Everything else that draws or changes state flushes
   * the batch first. Returns false if nothing was queued.
   */
  bool QueueGUIQuads(unsigned int method,
                     ID3D11ShaderResourceView* view0,
                     ID3D11ShaderResourceView* view1,
                     const QuadGeometry* quads,
                     unsigned int count,
                     const QuadParams& params);
  /*! \brief Draw the queued quads; nothing to do if none are queued. */
  void FlushBatch(BatchFlush reason);
  /*! \brief Flush before the command list is finished; the next instance buffer map discards. */
  void EndCommandList();
  const BatchStats& GetBatchStats() const { return m_batchStats; }
  static const char* GetBatchFlushName(BatchFlush reason);
  /*!
   * \brief Read the per-frame output constants (limited range, PQ, SDR peak luminance) once.
   * \param batchQuads <gui><batchquads>: queue texture quads (A/B switch, see HasBatchPath)
   */
  void BeginFrame(bool limitedColorRange, bool batchQuads);
  void DrawIndexed(unsigned int indexCount, unsigned int startIndex, unsigned int startVertex);
  void Draw(unsigned int vertexCount, unsigned int startVertex);

  bool  HardwareClipIsPossible(void) const { return m_clipPossible; }
  float GetClipXFactor(void) const { return m_clipXFactor;  }
  float GetClipXOffset(void) const { return m_clipXOffset;  }
  float GetClipYFactor(void) const { return m_clipYFactor;  }
  float GetClipYOffset(void) const { return m_clipYOffset;  }

  // need to use aligned allocation because we use XMMATRIX in structures.
  void* operator new (size_t size)
  {
    void* ptr = KODI::MEMORY::AlignedMalloc(size, __alignof(CGUIShaderDX));
    if (!ptr)
      throw std::bad_alloc();
    return ptr;
  }
  // free aligned memory.
  void operator delete (void* ptr)
  {
    KODI::MEMORY::AlignedFree(ptr);
  }

private:
  class CWorldViewProj
  {
  public:
    void XM_CALLCONV GetWVP(DirectX::XMMATRIX& w, DirectX::XMMATRIX& v, DirectX::XMMATRIX& p) const
    {
      w = m_world;
      v = m_view;
      p = m_projection;
    }
    DirectX::XMMATRIX XM_CALLCONV GetWVP() const;
    DirectX::XMMATRIX XM_CALLCONV GetWorld() const { return m_world; }
    DirectX::XMMATRIX XM_CALLCONV GetView() const { return m_view; }
    DirectX::XMMATRIX XM_CALLCONV GetProjection() const { return m_projection; }

    void XM_CALLCONV SetWVP(const DirectX::XMMATRIX& w,
                            const DirectX::XMMATRIX& v,
                            const DirectX::XMMATRIX& p);
    void XM_CALLCONV SetWorld(const DirectX::XMMATRIX& value);
    void XM_CALLCONV SetView(const DirectX::XMMATRIX& value);
    void XM_CALLCONV SetProjection(const DirectX::XMMATRIX& value);

  private:
    DirectX::XMMATRIX m_world = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_view = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_projection = DirectX::XMMatrixIdentity();
    mutable DirectX::XMMATRIX m_cachedVP = DirectX::XMMatrixIdentity();
    mutable DirectX::XMMATRIX m_cachedWVP = DirectX::XMMatrixIdentity();
    mutable bool m_isVPDirty = false;
    mutable bool m_isWorldDirty = false;
  };

  struct cbViewPort
  {
    float TopLeftX;
    float TopLeftY;
    float Width;
    float Height;
  };
  struct cbWorld
  {
    DirectX::XMMATRIX wvp;
    DirectX::XMFLOAT4 m_shaderClip;
    DirectX::XMFLOAT2 m_texStep;
    DirectX::XMFLOAT2 m_texStep2;
    float blackLevel;
    float colorRange;
    float sdrPeakLum;
    int PQ;
    float depth;
  };

  void Release(void);
  bool CreateBuffers(void);
  bool CreateSamplers(void);
  void SetSamplers(void);
  void ApplyChanges(void);
  void ClipToScissorParams(void);
  void RestoreInputAssembler();
  void LogBatchStats();

  //! state the queued quads depend on, compared at the flush in debug builds
  struct BatchState
  {
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencil;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
    D3D11_RECT scissor{};
    D3D11_VIEWPORT viewport{};
  };
  void GetBatchState(BatchState& state) const;

  // GUI constants
  cbViewPort m_cbViewPort = {};
  CWorldViewProj m_cbWorldViewProj;
  float m_depth = 1.f;
  // Font vertex shader constants
  DirectX::XMFLOAT4 m_shaderClip{}; // clip rect (x1,y1,x2,y2) in font-local coords
  DirectX::XMFLOAT2 m_texStep{}; // step (1/resolution) for the input texture 1
  DirectX::XMFLOAT2 m_texStep2{}; // step (1/resolution) for the input texture 2

  bool m_bCreated = false;
  size_t m_currentShader = 0;

  CD3DVertexShader m_vertexShader;
  CD3DVertexShader m_vertexShaderClip;
  CD3DVertexShader m_vertexShaderQuad;
  bool m_hasQuadPath = false;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pQuadCB;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pQuadIndexBuffer;

  // batched quads (QueueGUIQuads)
  CD3DVertexShader m_vertexShaderBatch;
  bool m_hasBatchPath = false;
  bool m_batchEnabled = true;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pBatchCornerBuffer; // corner selectors of the 4 vertices
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pBatchIndexBuffer; // 6 indices
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pBatchInstanceBuffer; // BATCH_CAPACITY QuadInstance
  std::vector<QuadInstance> m_batch;
  unsigned int m_batchMethod = 0;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_batchViews[2]; // held until drawn
  BatchState m_batchState;
  unsigned int m_batchOffset = 0; // first free instance of m_pBatchInstanceBuffer
  bool m_batchDiscard = true; // the next map discards (first map of a command list)
  bool m_batchNoOverwrite = true; // false: every flush discards
  BatchStats m_batchStats;
  BatchStats m_batchStatsLogged;
  std::chrono::steady_clock::time_point m_batchLogTime;

  // per-frame output constants (BeginFrame)
  float m_blackLevel = 0.f;
  float m_colorRange = 1.f;
  float m_sdrPeakLum = 1.f;
  int m_PQ = 0;

  struct ShaderPair
  {
    CD3DVertexShader* m_vs = nullptr;
    CD3DPixelShader m_ps;
  };
  ShaderPair m_shaders[SHADER_METHOD_RENDER_COUNT];

  Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pSampLinear;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pSampNearestNeighbor;

  // GUI buffers
  bool m_bIsWVPDirty = true;
  bool m_bIsVPDirty = true;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pWVPBuffer;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pVPBuffer;
  Microsoft::WRL::ComPtr<ID3D11Buffer> m_pVertexBuffer;

  // clip to scissors params
  bool m_clipPossible = false;
  float m_clipXFactor = 0.0f;
  float m_clipXOffset = 0.0f;
  float m_clipYFactor = 0.0f;
  float m_clipYOffset = 0.0f;
};

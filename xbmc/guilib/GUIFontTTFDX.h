/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
\file GUIFont.h
\brief
*/

#include "D3DResource.h"
#include "GUIFontTTF.h"
#include "GUIShaderDX.h"
#include "threads/CriticalSection.h"

#include <atomic>
#include <list>
#include <memory>
#include <vector>

#include <wrl/client.h>

/*!
 \ingroup textures
 \brief
 */
class CGUIFontTTFDX : public CGUIFontTTF, public ID3DResource
{
public:
  explicit CGUIFontTTFDX(const std::string& fontIdent);
  virtual ~CGUIFontTTFDX(void);

  bool FirstBegin() override;
  void LastEnd() override;
  CVertexBuffer CreateVertexBuffer(const std::vector<SVertex>& vertices) const override;
  void DestroyVertexBuffer(CVertexBuffer& bufferHandle) const override;

  void OnDestroyDevice(bool fatal) override;
  void OnCreateDevice() override;

  static void CreateStaticIndexBuffer(void);
  static void DestroyStaticIndexBuffer(void);

protected:
  std::unique_ptr<CTexture> ReallocTexture(unsigned int& newHeight) override;
  bool CopyCharToTexture(const uint8_t* pixels,
                         unsigned int pitch,
                         unsigned int x1,
                         unsigned int y1,
                         unsigned int x2,
                         unsigned int y2) override;
  void DeleteHardwareTexture() override;

private:
  /*!
   * A text's vertices (CVertexBuffer::bufferHandle). Batched text (CGUIShaderDX::QueueGlyphs) uses
   * the glyph quads; the vertex buffer of the font's own draw is created when that draws the text.
   */
  struct CTextVertices
  {
    std::vector<CGUIShaderDX::GlyphQuad> glyphs; // empty if the text is not batched
    std::vector<SVertex> vertices; // until the buffer exists
    std::unique_ptr<CD3DBuffer> buffer;
  };

  static void AddReference(CGUIFontTTFDX* font, CTextVertices* text);
  static void ClearReference(CGUIFontTTFDX* font, CTextVertices* text);
  static const CD3DBuffer* GetBuffer(CTextVertices& text);
  bool QueueText(CGUIShaderDX& shader, CGraphicContext& context);

  std::unique_ptr<CD3DTexture> m_speedupTexture; // extra texture to speed up reallocations
  std::list<CTextVertices*> m_buffers;
  bool m_scissorClip{false};
  bool m_batchText{false}; // the texts of this Begin/End block join the GUI batch

  static CCriticalSection m_staticIndexBufferSection;
  static std::atomic<bool> m_staticIndexBufferCreated;
  static Microsoft::WRL::ComPtr<ID3D11Buffer> m_staticIndexBuffer;
};

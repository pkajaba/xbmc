/*
 *  Copyright (C) 2026 Pavel Kajaba
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDAudioCodec.h"
#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CProcessInfo;

// Opaque context of the hz-stream library (libhz_stream_ffi.so)
struct hz_stream_context;

/*!
 * \brief Real-time transcoding codec from DTS:X to Dolby TrueHD with Dolby Atmos.
 *
 * Transcodes DTS:X bitstream packets on-the-fly into TrueHD access units and
 * packs them like CDVDAudioCodecPassthrough does: into MAT frames for an IEC
 * device, or into groups of 24 raw access units for a RAW device.
 * Only 48 kHz streams are handled; Open() refuses anything else so the
 * normal passthrough or decoder path takes over.
 *
 * Cryptographic OAMD signing:
 * If an HMAC key is configured in advancedsettings.xml (<audio><dtsx2atmoskey>...</dtsx2atmoskey></audio>)
 * as hexadecimal text, the object metadata the library writes for DTS:X
 * streams that carry extension audio is signed; nothing else is signed. A key
 * that is not clean hex is refused (and logged, without its value); the
 * metadata is then written unsigned, as it is when no key is configured.
 */
class CDVDAudioCodecDtsToAtmos : public CDVDAudioCodec
{
public:
  explicit CDVDAudioCodecDtsToAtmos(CProcessInfo &processInfo);
  ~CDVDAudioCodecDtsToAtmos() override;

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  void Dispose() override;
  bool AddData(const DemuxPacket &packet) override;
  void GetData(DVDAudioFrame &frame) override;
  void Drain() override;
  void Reset() override;

  AEAudioFormat GetFormat() override { return m_format; }
  bool NeedPassthrough() override { return true; }
  std::string GetName() override { return "pt-dtsx2atmos"; }
  int GetBufferSize() override;

private:
  bool InitializeLibrary();
  void ApplyKey(); //!< set the signing key from the advanced settings, if one is given
  bool Recreate(); //!< build a new context and clear the packing state; false if that fails

  struct Api
  {
    hz_stream_context* (*create)(uint32_t, uint32_t){nullptr};
    void (*destroy)(hz_stream_context*){nullptr};
    void (*reset)(hz_stream_context*){nullptr};
    int (*setKeyHex)(hz_stream_context*, const uint8_t*, size_t){nullptr};
    int (*push)(hz_stream_context*, const uint8_t*, size_t, double){nullptr};
    int (*pull)(hz_stream_context*, uint8_t*, size_t, double*){nullptr};
    int (*flush)(hz_stream_context*){nullptr};
  };

  void* m_libHandle{nullptr};
  Api m_api;
  hz_stream_context* m_context{nullptr};

  AEAudioFormat m_format;
  bool m_deviceIsRAW{false};
  std::unique_ptr<CPackerMAT> m_packerMAT;
  std::vector<uint8_t> m_outBuffer; //!< the frame handed out by GetData
  std::vector<uint8_t> m_unitBuffer; //!< one TrueHD access unit pulled from the library
  std::vector<uint8_t> m_rawBuffer; //!< RAW device: up to 24 access units
  unsigned int m_rawUnits{0};

  double m_framePts{DVD_NOPTS_VALUE}; //!< pts of the first unit of the frame being built
  bool m_framePtsSet{false};
  bool m_loggedRateRefusal{false};
  bool m_loggedPushFailure{false};
  bool m_loggedRecreateFailure{false};
  bool m_keyConfigured{false};
};

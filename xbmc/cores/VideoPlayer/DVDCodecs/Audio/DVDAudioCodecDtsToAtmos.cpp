/*
 *  Copyright (C) 2026 Pavel Kajaba
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDAudioCodecDtsToAtmos.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Utils/AEChannelData.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <cmath>
#include <dlfcn.h>

namespace
{
// A MAT frame (and the RAW passthrough group): 24 TrueHD access units.
constexpr unsigned int UNITS_PER_FRAME = 24;
constexpr int TRUEHD_MAT_FRAME_SIZE = 61440;
// A TrueHD access unit is at most 4095 16-bit words.
constexpr size_t MAX_UNIT_SIZE = 8192;
constexpr unsigned int SUPPORTED_SAMPLE_RATE = 48000;
// The interface version of libhz_stream_ffi this code was written against:
// HZ_STREAM_ABI_VERSION in its header hz_stream.h. A library that answers
// anything else is not used.
constexpr uint32_t HZ_STREAM_ABI_VERSION = 1;
// Where the library is looked for: the dynamic loader's own search path.
constexpr const char* LIBRARY_NAME = "libhz_stream_ffi.so";
// The bed of the TrueHD presentation as the sink sees it: 7.1.
constexpr unsigned int PASSTHROUGH_CHANNELS = 8;

template<typename Fn>
Fn Symbol(void* lib, const char* name)
{
  return reinterpret_cast<Fn>(dlsym(lib, name));
}
} // namespace

CDVDAudioCodecDtsToAtmos::CDVDAudioCodecDtsToAtmos(CProcessInfo &processInfo)
  : CDVDAudioCodec(processInfo)
{
  // Same format fields CDVDAudioCodecPassthrough sets for TrueHD.
  m_deviceIsRAW = processInfo.WantsRawPassthrough();
  m_format.m_dataFormat = AE_FMT_RAW;
  m_format.m_sampleRate = SUPPORTED_SAMPLE_RATE;
  m_format.m_frameSize = 1;
  m_format.m_streamInfo.m_type = CAEStreamInfo::STREAM_TYPE_TRUEHD;
  m_format.m_streamInfo.m_sampleRate = SUPPORTED_SAMPLE_RATE;
  m_format.m_streamInfo.m_channels = PASSTHROUGH_CHANNELS;
  m_format.m_streamInfo.m_bitDepth = 24;
  m_format.m_streamInfo.m_hasAtmos = true;
  CAEChannelInfo layout;
  for (unsigned int i = 0; i < PASSTHROUGH_CHANNELS; i++)
    layout += AE_CH_RAW;
  m_format.m_channelLayout = layout;

  m_unitBuffer.resize(MAX_UNIT_SIZE);
  m_outBuffer.reserve(TRUEHD_MAT_FRAME_SIZE);
  m_rawBuffer.reserve(UNITS_PER_FRAME * MAX_UNIT_SIZE);
}

CDVDAudioCodecDtsToAtmos::~CDVDAudioCodecDtsToAtmos()
{
  Dispose();
}

bool CDVDAudioCodecDtsToAtmos::InitializeLibrary()
{
  if (m_libHandle)
    return true;

  // NODELETE: the library's worker threads outlive any one codec instance
  // and keep running its code, so dlclose must never unmap it.
  m_libHandle = dlopen(LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
  if (!m_libHandle)
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecDtsToAtmos: {} not found, skipping transcode", LIBRARY_NAME);
    return false;
  }
  CLog::Log(LOGINFO, "CDVDAudioCodecDtsToAtmos: loaded {}", LIBRARY_NAME);

  // Refuse a library whose interface is not the one this code calls.
  using FnAbiVersion = uint32_t (*)();
  const auto abiVersion =
      Symbol<FnAbiVersion>(m_libHandle, "hz_stream_abi_version");
  if (!abiVersion || abiVersion() != HZ_STREAM_ABI_VERSION)
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecDtsToAtmos: {} has interface version {} (need {})",
              LIBRARY_NAME, abiVersion ? abiVersion() : 0, HZ_STREAM_ABI_VERSION);
    dlclose(m_libHandle);
    m_libHandle = nullptr;
    return false;
  }

  m_api.create = Symbol<decltype(Api::create)>(m_libHandle, "hz_stream_create");
  m_api.destroy = Symbol<decltype(Api::destroy)>(m_libHandle, "hz_stream_destroy");
  m_api.reset = Symbol<decltype(Api::reset)>(m_libHandle, "hz_stream_reset");
  m_api.setKeyHex = Symbol<decltype(Api::setKeyHex)>(m_libHandle, "hz_stream_set_key_hex");
  m_api.push = Symbol<decltype(Api::push)>(m_libHandle, "hz_stream_push_dts");
  m_api.pull = Symbol<decltype(Api::pull)>(m_libHandle, "hz_stream_pull_truehd");
  m_api.flush = Symbol<decltype(Api::flush)>(m_libHandle, "hz_stream_flush");

  if (!m_api.create || !m_api.destroy || !m_api.reset || !m_api.push || !m_api.pull || !m_api.flush)
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecDtsToAtmos: missing required symbols in libhz_stream_ffi.so");
    dlclose(m_libHandle);
    m_libHandle = nullptr;
    m_api = {};
    return false;
  }

  return true;
}

void CDVDAudioCodecDtsToAtmos::ApplyKey()
{
  // Optional HMAC signing key. The library parses the hex text; a malformed
  // key is refused there and never used as raw bytes. Its value is not logged.
  m_keyConfigured = false;
  if (const auto settingsComp = CServiceBroker::GetSettingsComponent())
  {
    if (const auto advSettings = settingsComp->GetAdvancedSettings())
    {
      const std::string& keyStr = advSettings->GetDtsx2AtmosKey();
      if (!keyStr.empty())
      {
        if (m_api.setKeyHex &&
            m_api.setKeyHex(m_context, reinterpret_cast<const uint8_t*>(keyStr.data()),
                            keyStr.size()) == 0)
        {
          m_keyConfigured = true;
        }
        else
        {
          CLog::Log(LOGWARNING,
                    "CDVDAudioCodecDtsToAtmos: <dtsx2atmoskey> is not usable (it must be an "
                    "even number of hex digits, 1 to 128 bytes, and the library must support "
                    "keys); the object metadata will be unsigned");
        }
      }
    }
  }
}

bool CDVDAudioCodecDtsToAtmos::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  // The encoder and the MAT packer are built for 48 kHz. Refuse anything
  // else here so the normal passthrough or decode path handles the stream.
  if (hints.samplerate != 0 && hints.samplerate != static_cast<int>(SUPPORTED_SAMPLE_RATE))
  {
    CLog::Log(LOGWARNING,
              "CDVDAudioCodecDtsToAtmos: stream sample rate {} Hz is not supported (48000 Hz only), "
              "not transcoding",
              hints.samplerate);
    return false;
  }

  if (!InitializeLibrary())
    return false;

  m_context = m_api.create(SUPPORTED_SAMPLE_RATE, 0); // real-time effort
  if (!m_context)
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecDtsToAtmos: failed to create a transcode context");
    return false;
  }

  ApplyKey();

  CLog::Log(LOGINFO,
            "CDVDAudioCodecDtsToAtmos: opened real-time DTS:X to TrueHD Atmos transcode "
            "(output: {}, key for the object metadata: {})",
            m_deviceIsRAW ? "RAW" : "MAT", m_keyConfigured ? "set" : "none, unsigned");

  m_packerMAT = std::make_unique<CPackerMAT>();
  m_rawBuffer.clear();
  m_rawUnits = 0;
  m_framePts = DVD_NOPTS_VALUE;
  m_framePtsSet = false;
  m_loggedRateRefusal = false;
  m_loggedPushFailure = false;
  m_loggedRecreateFailure = false;

  return true;
}

bool CDVDAudioCodecDtsToAtmos::Recreate()
{
  // The new pipeline starts from a clean stream, so the packer and the
  // half-built output frame must not carry data from the old one.
  if (m_packerMAT)
    m_packerMAT->Reset();
  m_rawBuffer.clear();
  m_rawUnits = 0;
  m_framePts = DVD_NOPTS_VALUE;
  m_framePtsSet = false;

  m_context = m_api.create ? m_api.create(SUPPORTED_SAMPLE_RATE, 0) : nullptr;
  if (!m_context)
  {
    if (!m_loggedRecreateFailure)
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecDtsToAtmos: failed to rebuild the transcode context, "
                          "dropping packets until it can be");
      m_loggedRecreateFailure = true;
    }
    return false;
  }
  m_loggedRecreateFailure = false;
  ApplyKey();
  return true;
}

void CDVDAudioCodecDtsToAtmos::Dispose()
{
  if (m_context && m_api.destroy)
    m_api.destroy(m_context);
  m_context = nullptr;
  m_packerMAT.reset();

  // The function pointers point into the library: drop them with it.
  m_api = {};
  if (m_libHandle)
  {
    dlclose(m_libHandle);
    m_libHandle = nullptr;
  }
}

bool CDVDAudioCodecDtsToAtmos::AddData(const DemuxPacket &packet)
{
  // After a stopped pipeline the context may be gone: try to build one for
  // this packet. If that fails the packet is dropped (the player would resend
  // the same one for ever on false) and the next packet tries again.
  if (!m_context && !Recreate())
    return true;

  // GetData() is called with empty packets to drain; nothing to push then.
  if (!packet.pData || packet.iSize <= 0)
    return true;

  // The library takes DVD_TIME_BASE microseconds and treats DVD_NOPTS_VALUE
  // (and anything negative or not finite) as "no timestamp".
  const int ret = m_api.push(m_context, packet.pData, static_cast<size_t>(packet.iSize), packet.pts);
  if (ret == -3)
  {
    // The library carries silence for the frames it refused, so timing holds.
    if (!m_loggedRateRefusal)
    {
      CLog::Log(LOGWARNING, "CDVDAudioCodecDtsToAtmos: DTS frame with an unsupported sample rate, "
                            "substituting silence");
      m_loggedRateRefusal = true;
    }
    return true;
  }
  if (ret >= 0)
    m_loggedPushFailure = false;
  if (ret < 0)
  {
    // The library has taken or dropped the bytes either way; returning false
    // would make the player send the same packet again and duplicate it.
    // A stopped pipeline stays stopped (reset does not revive it), so build a
    // new context. The log is limited to the first failure of a run of them.
    if (!m_loggedPushFailure)
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecDtsToAtmos: push failed ({}), packet dropped", ret);
      m_loggedPushFailure = true;
    }
    if (ret == -4)
    {
      m_api.destroy(m_context);
      m_context = nullptr;
      Recreate();
    }
    return true;
  }
  return true;
}

void CDVDAudioCodecDtsToAtmos::GetData(DVDAudioFrame &frame)
{
  frame.nb_frames = 0;
  frame.framesOut = 0;
  frame.hasDiscontinuity = false;
  frame.discontinuityCorrection = 0.0;
  frame.hasTimestamp = false;

  if (!m_context || !m_packerMAT)
    return;

  while (true)
  {
    // One TrueHD access unit per call, which is what CPackerMAT::PackTrueHD takes.
    double unitPts = std::nan("");
    const int bytes = m_api.pull(m_context, m_unitBuffer.data(), m_unitBuffer.size(), &unitPts);
    if (bytes < 0)
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecDtsToAtmos: pull failed ({})", bytes);
      break;
    }
    if (bytes == 0)
      break;

    // The frame is stamped with the time of its first access unit.
    if (!m_framePtsSet)
    {
      m_framePts = std::isfinite(unitPts) ? unitPts : DVD_NOPTS_VALUE;
      m_framePtsSet = true;
    }

    bool complete = false;
    if (m_deviceIsRAW)
    {
      m_rawBuffer.insert(m_rawBuffer.end(), m_unitBuffer.begin(), m_unitBuffer.begin() + bytes);
      complete = (++m_rawUnits == UNITS_PER_FRAME);
      if (complete)
        m_outBuffer.swap(m_rawBuffer);
    }
    else if (m_packerMAT->PackTrueHD(m_unitBuffer.data(), bytes))
    {
      m_outBuffer = m_packerMAT->GetOutputFrame();
      complete = !m_outBuffer.empty();
    }

    if (!complete)
      continue;

    // nb_frames counts bytes for raw passthrough (frame size 1), exactly as
    // CDVDAudioCodecPassthrough reports it.
    frame.data[0] = m_outBuffer.data();
    frame.nb_frames = static_cast<unsigned int>(m_outBuffer.size());
    frame.framesize = 1;
    frame.planes = 1;
    frame.bits_per_sample = 8;
    frame.passthrough = true;
    frame.format = m_format;
    // 24 units of 40 samples at 48 kHz: 20 ms.
    frame.duration = DVD_MSEC_TO_TIME(frame.format.m_streamInfo.GetDuration());
    frame.pts = m_framePts;
    frame.hasTimestamp = (m_framePts != DVD_NOPTS_VALUE);

    m_framePtsSet = false;
    m_framePts = DVD_NOPTS_VALUE;
    // RAW: m_outBuffer now owns the bytes; m_rawBuffer was swapped out empty.
    m_rawBuffer.clear();
    m_rawUnits = 0;
    return;
  }
}

void CDVDAudioCodecDtsToAtmos::Drain()
{
  if (m_context && m_api.flush)
    m_api.flush(m_context);
}

void CDVDAudioCodecDtsToAtmos::Reset()
{
  if (m_context && m_api.reset)
    m_api.reset(m_context);

  if (m_packerMAT)
    m_packerMAT->Reset();

  m_rawBuffer.clear();
  m_rawUnits = 0;
  m_framePts = DVD_NOPTS_VALUE;
  m_framePtsSet = false;
}

int CDVDAudioCodecDtsToAtmos::GetBufferSize()
{
  return TRUEHD_MAT_FRAME_SIZE;
}

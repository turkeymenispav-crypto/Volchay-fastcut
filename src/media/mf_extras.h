// GUIDs and enum values from the Windows SDK that are absent from the
// MinGW-w64 headers we ship in CI. We define them here so the build is
// self-contained and mirrors the official values bit-for-bit.
//
// MSVC + the official Windows SDK already declare every one of these
// (typically via EXTERN_GUID in mfapi.h / mfidl.h), so for that toolchain
// we simply rely on the system headers. The defines below are gated on
// __MINGW32__ and only kick in on the MinGW cross-build.
#pragma once

#include "platform/windows.h"

#ifdef __MINGW32__
#include <initguid.h>

DEFINE_GUID(MFTranscodeContainerType_MPEG4,
    0xdc6cd05d, 0xb9d0, 0x40ef, 0xbd, 0x35, 0xfa, 0x62, 0x2c, 0x1a, 0xb2, 0x8a);

DEFINE_GUID(MF_TRANSCODE_CONTAINERTYPE,
    0x150ff23f, 0x4abc, 0x478b, 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca);

DEFINE_GUID(MF_TRANSCODE_TOPOLOGYMODE,
    0x3e3df610, 0x394a, 0x40b2, 0x9d, 0xea, 0x3b, 0xab, 0x65, 0x0b, 0xeb, 0xf2);

DEFINE_GUID(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,
    0x7632f0e6, 0x9538, 0x4d61, 0xac, 0xda, 0xea, 0x29, 0xc8, 0xc1, 0x44, 0x56);

// Required for IMFSourceReader / IMFSinkWriter to use hardware MFTs
// (NVENC, Quick Sync, AMF). Without this attribute, MF falls back to
// software encode/decode even on a system that has hardware available.
DEFINE_GUID(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
    0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0);

// Lets the sink writer write samples asynchronously so the source
// reader / encoder pipeline never stalls behind the muxer flushing.
DEFINE_GUID(MF_SINK_WRITER_DISABLE_THROTTLING,
    0x08b845d8, 0x2b74, 0x4afe, 0x9d, 0x53, 0xbe, 0x16, 0xd2, 0xd5, 0xae, 0x4f);

#endif  // __MINGW32__

// MF_TOPOLOGY_HARDWARE_MODE values (enum). The token is interchangeable
// across SDK versions, so we just provide the integer if no header has it.
#ifndef MF_TOPOLOGY_HARDWARE_MODE
#define MF_TOPOLOGY_HARDWARE_MODE      1
#endif
#ifndef MFTOPOLOGY_HWMODE_USE_HARDWARE
#define MFTOPOLOGY_HWMODE_USE_HARDWARE 1
#endif

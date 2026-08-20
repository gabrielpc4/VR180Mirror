#pragma once

// Cross-process contract between the optional VaM Oculus submission hook and
// VR180Mirror.  It deliberately contains no pointers: legacy D3D11 shared
// handles are opened by the consumer on the same adapter, while every other
// field is plain POD suitable for a named file mapping.

#include <cstdint>

namespace vr180::oculus_capture {

constexpr std::uint32_t kMagic = 0x4d41564fU;  // "OVAM"
constexpr std::uint32_t kVersion = 2U;
constexpr wchar_t kMappingName[] = L"Local\\VR180Mirror.VaMOculusCapture.v1";

struct Eye {
  std::uint64_t sharedHandle = 0U;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::uint32_t format = 0U;
  std::uint32_t sampleCount = 0U;
  // Standard Oculus tangent FOV convention: positive up/down/right/left.
  float upTan = 0.0F;
  float downTan = 0.0F;
  float leftTan = 0.0F;
  float rightTan = 0.0F;
};

// Pose of the source headset at the time its submitted eye textures were
// rendered.  The producer uses the public LibOVR RenderPose value, not a
// later polled pose, so stabilization can be correlated to frameSerial.
struct SourcePose {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float w = 1.0F;
  double sensorSampleTime = 0.0;
  std::uint32_t valid = 0U;
  std::uint32_t reserved = 0U;
};

struct SharedState {
  std::uint32_t magic = kMagic;
  std::uint32_t version = kVersion;
  std::uint32_t bytes = sizeof(SharedState);
  std::uint32_t producerPid = 0U;
  std::uint64_t epoch = 0U;
  std::uint64_t frameSerial = 0U;
  // 1=IAT hook installed, 2=submit entry point resolved, 3=submit invoked,
  // 4=both eye copies published. Diagnostic only; the reader never relies on
  // this for frame readiness.
  std::uint32_t hookState = 0U;
  std::uint32_t reserved = 0U;
  Eye left{};
  Eye right{};
  SourcePose pose{};
};

static_assert(sizeof(Eye) == 40U);
static_assert(sizeof(SourcePose) == 32U);
static_assert(sizeof(SharedState) == 152U);

}  // namespace vr180::oculus_capture

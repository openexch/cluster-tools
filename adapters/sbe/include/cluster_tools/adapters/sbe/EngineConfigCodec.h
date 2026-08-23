#ifndef CLUSTER_TOOLS_ADAPTERS_SBE_ENGINE_CONFIG_CODEC_H
#define CLUSTER_TOOLS_ADAPTERS_SBE_ENGINE_CONFIG_CODEC_H

#include <cstdint>
#include <vector>

#include "cluster_tools/domain/EngineConfig.h"
#include "cluster_tools/ports/Result.h"

// EngineConfigCodec — encodes the domain EngineConfig as match order-schema bytes
// (MessageHeader + EngineConfig, template 8, schema 1, wire v10): exactly the
// app-payload form ClusterSession::offer expects, which the aeron adapter then frames
// with the cluster SessionMessageHeader — the same nesting the PoC proved for
// CreateOrder.
//
// This is the only translation unit outside the generated tree that includes the
// match-schema codec headers (the aeron adapter includes only the cluster-protocol
// ones); the header itself pulls in neither, so app code stays codec-free.
//
// The codec encodes what it is given — field values and market ORDER included.
// Callers pass a validated, normalized() config: the engine sorts on decode anyway,
// but sending canonical bytes keeps what-was-validated == what-rides-the-wire.
// No exception crosses the boundary; failures come back as Result.

namespace cluster_tools { namespace sbe_adapter {

// Wire identity of the encoding, for operator-facing output ("what did I just send").
struct EngineConfigWireInfo
{
    std::uint16_t schemaId;
    std::uint16_t schemaVersion;
    std::uint16_t templateId;
};

EngineConfigWireInfo engineConfigWireInfo() noexcept;

ports::Result<std::vector<std::uint8_t>> encodeEngineConfig(const domain::EngineConfig &config);

}}

#endif

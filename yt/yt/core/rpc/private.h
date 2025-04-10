#pragma once

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/public.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, RpcServerLogger, "RpcServer");
YT_DEFINE_GLOBAL(const NLogging::TLogger, RpcClientLogger, "RpcClient");

YT_DEFINE_GLOBAL(const NProfiling::TProfiler, RpcServerProfiler, "/rpc/server");
YT_DEFINE_GLOBAL(const NProfiling::TProfiler, RpcClientProfiler, "/rpc/client");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc

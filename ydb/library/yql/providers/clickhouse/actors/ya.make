LIBRARY()

SRCS(
    yql_ch_read_actor.cpp
    yql_ch_source_factory.cpp
)

PEERDIR(
    yql/essentials/minikql/computation
    ydb/library/yql/providers/common/token_accessor/client
    yql/essentials/public/types
    ydb/library/yql/dq/actors/compute
    ydb/library/yql/providers/clickhouse/proto
    ydb/library/yql/providers/common/http_gateway
)

YQL_LAST_ABI_VERSION()

END()

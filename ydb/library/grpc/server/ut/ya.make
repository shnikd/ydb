UNITTEST_FOR(ydb/library/grpc/server)

SIZE(MEDIUM)

PEERDIR(
    ydb/library/grpc/server
)

SRCS(
    grpc_response_ut.cpp
    stream_adaptor_ut.cpp
)

END()


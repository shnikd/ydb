Y_BENCHMARK()

ALLOCATOR(B)

TIMEOUT(1800)
SIZE(LARGE)
TAG(ya:fat)

PEERDIR(
    yql/essentials/minikql
    library/cpp/packedtypes
)

SRCS(
    pack.cpp
    pack_num_bench.cpp
)


END()

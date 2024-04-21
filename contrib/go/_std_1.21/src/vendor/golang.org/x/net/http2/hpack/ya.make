GO_LIBRARY()
IF (OS_DARWIN AND ARCH_ARM64)
    SRCS(
		encode.go
		hpack.go
		huffman.go
		static_table.go
		tables.go
    )
ELSEIF (OS_DARWIN AND ARCH_X86_64)
    SRCS(
		encode.go
		hpack.go
		huffman.go
		static_table.go
		tables.go
    )
ELSEIF (OS_LINUX AND ARCH_AARCH64)
    SRCS(
		encode.go
		hpack.go
		huffman.go
		static_table.go
		tables.go
    )
ELSEIF (OS_LINUX AND ARCH_X86_64)
    SRCS(
		encode.go
		hpack.go
		huffman.go
		static_table.go
		tables.go
    )
ELSEIF (OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		encode.go
		hpack.go
		huffman.go
		static_table.go
		tables.go
    )
ENDIF()
END()
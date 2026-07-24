file(GLOB astcenc_src CONFIGURE_DEPENDS
	"Source/*.cpp"
)

add_library(kyty_astcenc STATIC ${astcenc_src})

target_include_directories(kyty_astcenc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")

target_compile_definitions(kyty_astcenc PRIVATE
	ASTCENC_DECOMPRESS_ONLY
	ASTCENC_ISA_NONE
	# Scalar astcenc still processes four texels per SIMD lane group.
	# 52 is the smallest supported multiple that contains a 10x5 block.
	ASTCENC_BLOCK_MAX_TEXELS=52
)

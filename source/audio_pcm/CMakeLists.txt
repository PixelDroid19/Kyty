add_library(kyty_audio_pcm STATIC
	"${CMAKE_SOURCE_DIR}/emulator/src/AudioPcm.cpp"
)

target_include_directories(kyty_audio_pcm
	PUBLIC
		"${CMAKE_SOURCE_DIR}/emulator/include"
)

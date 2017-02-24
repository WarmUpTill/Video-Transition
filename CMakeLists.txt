project(stinger-transition)

if(DISABLE_STINGER)
	message(STATUS "Stinger transition support disabled")
	return()
endif()

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avfilter avdevice avutil swscale avformat swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

include_directories(
	SYSTEM "${CMAKE_SOURCE_DIR}/libobs"
)

set(stinger-transition_config_HEADERS
	obs-ffmpeg-compat.h
	obs-ffmpeg-formats.h
)

set(stinger-transition_SOURCES
	transition_stinger.c
	stringer-transition-module.c

)

add_library(stinger-transition MODULE
	${stinger-transition_config_HEADERS}
	${stinger-transition_SOURCES}
)
target_link_libraries(stinger-transition
	libobs
	libff
	${FFMPEG_LIBRARIES}
)

install_obs_plugin_with_data(stinger-transition data)

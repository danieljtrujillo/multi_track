include(${CMAKE_CURRENT_SOURCE_DIR}/../../max-sdk-base/script/max-pretarget.cmake)

#############################################################
# MAX EXTERNAL
#############################################################

include_directories(
	"${MAX_SDK_INCLUDES}"
	"${MAX_SDK_MSP_INCLUDES}"
	"${MAX_SDK_JIT_INCLUDES}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../../oscpack_1_1_0"
)

file(GLOB PROJECT_SRC
     "*.h"
	 "*.c"
     "*.cpp"
)
add_library(
	${PROJECT_NAME}
	MODULE
	${PROJECT_SRC}
)

# Link oscpack — platform-specific
if(WIN32)
	target_link_libraries(${PROJECT_NAME} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/../../../oscpack_1_1_0/build/$<CONFIG>/oscpack.lib"
		Ws2_32.lib
		winmm.lib
	)
elseif(APPLE)
	target_link_libraries(${PROJECT_NAME} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/../../../oscpack_1_1_0/build/oscpack_1_1_0/build/liboscpack.a"
	)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../max-sdk-base/script/max-posttarget.cmake)




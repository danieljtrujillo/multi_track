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

# Link oscpack and Windows libraries
target_link_libraries(${PROJECT_NAME}
	"${CMAKE_CURRENT_SOURCE_DIR}/../../../oscpack_1_1_0/build/Debug/oscpack.lib"
	Ws2_32.lib
	winmm.lib
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../max-sdk-base/script/max-posttarget.cmake)




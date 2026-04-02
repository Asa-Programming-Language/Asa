cmake_minimum_required(VERSION 3.15)

project(asa_compiler CXX C)
set (CMAKE_CXX_STANDARD 17)

# Make sure build type si set
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()


message(STATUS "Build type is: ${CMAKE_BUILD_TYPE}")
set(CMAKE_INSTALL_PREFIX ".")


# Handle including LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

#set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -Og")

set(CMAKE_CXX_FLAGS_RELEASE "-Os -s")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -s -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "-s -Wl,--gc-sections")
#set(CMAKE_CXX_FLAGS_RELEASE "-Os -s -fno-indent -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections")

add_executable(asa main.cpp main.h messagehandler.cpp messagehandler.h interpreter.cpp interpreter.h tests.cpp tests.h settings.h filemanager.cpp filemanager.h tokenizer.cpp tokenizer.h parser.cpp parser.h dependencies.cpp dependencies.h codegen.cpp codegen.h lexer.cpp lexer.h console.cpp console.h strops.cpp strops.h pch.cpp llvm.cpp monolithic.h)
target_compile_definitions(asa PUBLIC $<$<CONFIG:Debug>:DEBUG>)

# Enable function and data sections for dead code elimination
target_compile_options(asa PRIVATE 
	$<$<CONFIG:MinSizeRel>:-ffunction-sections>
	$<$<CONFIG:MinSizeRel>:-fdata-sections>
)

# Debug speed optimizations
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(CMAKE_C_COMPILER_LAUNCHER ccache)
	set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
	target_precompile_headers(asa PRIVATE "pch.h" "llvm.h")
	message(STATUS "Using precompiled headers: pch.h llvm.h")
endif()

# Find the libraries that correspond to the LLVM components
# that we wish to use
#llvm_map_components_to_libnames(llvm_libs support core target irreader nativecodegen)
llvm_map_components_to_libnames(llvm_libs ${LLVM_TARGETS_TO_BUILD} support core target irreader passes analysis)

# Link against LLVM libraries
target_link_libraries(asa ${llvm_libs})

install(TARGETS asa RUNTIME)
file(COPY "../modules" DESTINATION "./")
file(COPY "../modules" DESTINATION "./bin")
#file(COPY "fonts" DESTINATION "./")

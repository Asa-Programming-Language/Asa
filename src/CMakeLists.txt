cmake_minimum_required(VERSION 3.15)

project(asa_compiler CXX)
set (CMAKE_CXX_STANDARD 17)

# Handle including LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Set your project compile flags.
# E.g. if using the C++ header files
# you will need to enable C++11 support
# for your compiler.

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})



if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

#set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g")

set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_executable(asa main.cpp main.h tests.cpp tests.h settings.h filemanager.cpp filemanager.h tokenizer.cpp tokenizer.h parser.cpp parser.h dependencies.cpp dependencies.h codegen.cpp codegen.h lexer.cpp lexer.h console.cpp console.h)
target_compile_definitions(asa PUBLIC $<$<CONFIG:Debug>:DEBUG>)

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs support core irreader)

# Link against LLVM libraries
target_link_libraries(asa ${llvm_libs})

file(COPY "../modules" DESTINATION "./")
#file(COPY "fonts" DESTINATION "./")

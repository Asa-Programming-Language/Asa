cmake_minimum_required(VERSION 3.15)

project(asa_compiler CXX)
set (CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

#set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g")

set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_executable(asa main.cpp main.h tests.cpp tests.h settings.h filemanager.cpp filemanager.h tokenizer.cpp tokenizer.h parser.cpp parser.h dependencies.cpp dependencies.h lexer.cpp lexer.h console.cpp console.h)
target_compile_definitions(asa PUBLIC $<$<CONFIG:Debug>:DEBUG>)

file(COPY "../modules" DESTINATION "./")
#file(COPY "fonts" DESTINATION "./")

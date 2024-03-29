cmake_minimum_required(VERSION 3.27.9)
project(mineunit LANGUAGES CUDA CXX VERSION 1.0)
set(CMAKE_BUILD_TYPE Release)
file(GLOB PROJECT_LINK_LIBS "/content/Complier/src/build/*.so")
link_directories(/content/Complier/src/build)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

set(CMAKE_CUDA_ARCHITECTURES 75)
include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
include_directories(${CUDA_INCLUDE_DIRS})

#However, the file(GLOB...) allows for wildcard additions:
#file(GLOB SOURCES "src/Student.cpp")
#file(GLOB SOURCES "./*.cpp")
find_package(unofficial-argon2 CONFIG REQUIRED)
if(NOT unofficial-argon2_FOUND)
    message(FATAL_ERROR "Libargon2 not found. Please install Libargon2.")
endif()

find_package(CURL CONFIG REQUIRED)
if(NOT CURL_FOUND)
    message(FATAL_ERROR "libcurl not found. Please install libcurl.")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
if(NOT nlohmann_json_FOUND)
    message(FATAL_ERROR "nlohmann_json library not found. Please install nlohmann_json.")
endif()

find_package(cryptopp CONFIG REQUIRED)
if (NOT cryptopp_FOUND)
    message(FATAL_ERROR "Crypto++ library not found. Please install Crypto++.")
endif()

find_package(OpenSSL REQUIRED)
if(NOT OPENSSL_FOUND)
    message(FATAL_ERROR "OpenSSL not found. Please install OpenSSL.")
endif()

list(APPEND PROJECT_LINK_LIBS unofficial::argon2::libargon2 cryptopp::cryptopp CURL::libcurl nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)

#Bring the headers, such as Student.h into the project
include_directories(../include)
include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})

file(GLOB SOURCES "./*.cpp")
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
#Generate the shared library from the sources
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} ${PROJECT_LINK_LIBS} )
install(TARGETS ${PROJECT_NAME} DESTINATION /usr/lib)
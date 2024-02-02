cmake_minimum_required(VERSION 3.27.9)
project(kernelrunner LANGUAGES CUDA CXX VERSION 1.0)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CUDA_ARCHITECTURES 75)
include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}) 
#Bring the headers, such as Student.h into the project
file(GLOB LINK_LIBRARIES "./*.so")
link_directories(/content/VMLearner/build)

set(CMAKE_BUILD_TYPE Release)
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

list(APPEND LINK_LIBRARIES unofficial::argon2::libargon2 cryptopp::cryptopp CURL::libcurl nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)

#Bring the headers, such as Student.h into the project
include_directories(../include)

#However, the file(GLOB...) allows for wildcard additions:
#file(GLOB SOURCES "src/Student.cpp")
file(GLOB SOURCES "./*.cu")
set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CUDA)
#Generate the shared library from the sources
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE ${LINK_LIBRARIES})

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-O2>
    $<$<COMPILE_LANGUAGE:CUDA>:-O2>
)

set_target_properties(${PROJECT_NAME} PROPERTIES 
    COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>"
)
#Set the location for library installation -- i.e., /usr/lib in this case
# not really necessary in this example. Use "sudo make install" to apply
install(TARGETS ${PROJECT_NAME} DESTINATION /usr/lib)
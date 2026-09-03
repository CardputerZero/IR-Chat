cmake_minimum_required(VERSION 3.16)

project(M5CardputerZeroIRChat VERSION 0.1.2 LANGUAGES C CXX ASM)

option(IR_CHAT_USE_SDL "Use LVGL's SDL backend and mock IR transport" ON)
option(IR_CHAT_BUILD_TESTS "Build IR Chat unit tests" OFF)
set(IR_CHAT_BIN_NAME "M5CardputerZero-IR-Chat" CACHE STRING "Output executable name")
set(IR_CHAT_DEPS_DIR "${CMAKE_CURRENT_LIST_DIR}/dependencies" CACHE PATH "Third-party source directory")
set(IR_CHAT_OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/dist" CACHE PATH "Runtime output directory")

if(APPLE AND NOT IR_CHAT_USE_SDL)
    message(FATAL_ERROR "macOS builds only support IR_CHAT_USE_SDL=ON.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(IR_CHAT_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(IR_CHAT_SRC_DIR "${IR_CHAT_ROOT}/src")
set(IR_CHAT_LVGL_DIR "${IR_CHAT_DEPS_DIR}/lvgl")
set(IR_CHAT_SPDLOG_DIR "${IR_CHAT_DEPS_DIR}/spdlog")
set(IR_CHAT_SMOOTH_UI_DIR "${IR_CHAT_DEPS_DIR}/smooth_ui_toolkit")
set(IR_CHAT_SYSTEM_LIBS ${CMAKE_DL_LIBS})

if(UNIX AND NOT APPLE)
    list(APPEND IR_CHAT_SYSTEM_LIBS m)
endif()

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")
endif()

foreach(required_dir IN LISTS IR_CHAT_LVGL_DIR IR_CHAT_SPDLOG_DIR IR_CHAT_SMOOTH_UI_DIR)
    if(NOT EXISTS "${required_dir}/CMakeLists.txt")
        message(FATAL_ERROR "Missing dependency: ${required_dir}. Run ./bootstrap.sh first or set IR_CHAT_DEPS_DIR.")
    endif()
endforeach()

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

set(LV_BUILD_CONF_PATH "${IR_CHAT_ROOT}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)

set(SMOOTH_UI_TOOLKIT_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SMOOTH_UI_TOOLKIT_BUILD_TEST OFF CACHE BOOL "" FORCE)

add_subdirectory("${IR_CHAT_LVGL_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dependencies/lvgl" EXCLUDE_FROM_ALL)
add_subdirectory("${IR_CHAT_SPDLOG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dependencies/spdlog" EXCLUDE_FROM_ALL)
add_subdirectory("${IR_CHAT_SMOOTH_UI_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dependencies/smooth_ui_toolkit"
                 EXCLUDE_FROM_ALL)

function(ir_chat_add_sdl2_parent_include target imported_target)
    if(NOT TARGET "${imported_target}")
        return()
    endif()

    get_target_property(sdl2_include_dirs "${imported_target}" INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT sdl2_include_dirs)
        return()
    endif()

    foreach(sdl2_include_dir IN LISTS sdl2_include_dirs)
        if(sdl2_include_dir MATCHES "/SDL2$")
            get_filename_component(sdl2_parent_include_dir "${sdl2_include_dir}" DIRECTORY)
            target_include_directories("${target}" SYSTEM PRIVATE "${sdl2_parent_include_dir}")
        endif()
    endforeach()
endfunction()

set(IR_CHAT_LVGL_POSIX_PATH "./")
if(NOT IR_CHAT_USE_SDL)
    set(IR_CHAT_LVGL_POSIX_PATH "/usr/share/IR-Chat/")
endif()

target_compile_definitions(lvgl PUBLIC
    IR_CHAT_LVGL_USE_SDL=$<IF:$<BOOL:${IR_CHAT_USE_SDL}>,1,0>
    IR_CHAT_LVGL_FS_POSIX_PATH="${IR_CHAT_LVGL_POSIX_PATH}"
)

if(IR_CHAT_USE_SDL)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
    endif()

    if(SDL2_FOUND)
        target_link_libraries(lvgl PRIVATE PkgConfig::SDL2)
        ir_chat_add_sdl2_parent_include(lvgl PkgConfig::SDL2)
        set(IR_CHAT_SDL_LIBS PkgConfig::SDL2)
    else()
        find_package(SDL2 CONFIG QUIET)
        if(TARGET SDL2::SDL2)
            target_link_libraries(lvgl PRIVATE SDL2::SDL2)
            ir_chat_add_sdl2_parent_include(lvgl SDL2::SDL2)
            set(IR_CHAT_SDL_LIBS SDL2::SDL2)
        else()
            find_package(SDL2 REQUIRED)
            target_include_directories(lvgl SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
            foreach(sdl2_include_dir IN LISTS SDL2_INCLUDE_DIRS)
                if(sdl2_include_dir MATCHES "/SDL2$")
                    get_filename_component(sdl2_parent_include_dir "${sdl2_include_dir}" DIRECTORY)
                    target_include_directories(lvgl SYSTEM PRIVATE "${sdl2_parent_include_dir}")
                endif()
            endforeach()
            set(IR_CHAT_SDL_LIBS ${SDL2_LIBRARIES})
        endif()
    endif()
endif()

file(GLOB IR_CHAT_APP_CPP CONFIGURE_DEPENDS
    "${IR_CHAT_SRC_DIR}/main.cpp"
    "${IR_CHAT_SRC_DIR}/core/*.cpp"
    "${IR_CHAT_SRC_DIR}/input/*.cpp"
    "${IR_CHAT_SRC_DIR}/models/*.cpp"
    "${IR_CHAT_SRC_DIR}/view_models/*.cpp"
    "${IR_CHAT_SRC_DIR}/views/*.cpp"
)

set(IR_CHAT_TRANSPORT_CPP
    "${IR_CHAT_SRC_DIR}/radio/ir_chat_codec.cpp"
    "${IR_CHAT_SRC_DIR}/radio/radio_backend.cpp"
    "${IR_CHAT_SRC_DIR}/radio/radio_worker.cpp"
)

if(IR_CHAT_USE_SDL)
    list(APPEND IR_CHAT_TRANSPORT_CPP "${IR_CHAT_SRC_DIR}/radio/mock_radio_backend.cpp")
else()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The real IR backend requires Linux LIRC support.")
    endif()
    list(APPEND IR_CHAT_TRANSPORT_CPP
        "${IR_CHAT_SRC_DIR}/radio/linux_ir_backend.cpp"
        "${IR_CHAT_SRC_DIR}/radio/lirc_device_discovery.cpp"
    )
endif()

add_executable(${IR_CHAT_BIN_NAME}
    ${IR_CHAT_APP_CPP}
    "${IR_CHAT_SRC_DIR}/hal/ir_chat_lvgl_hal.cpp"
    ${IR_CHAT_TRANSPORT_CPP}
)

target_include_directories(${IR_CHAT_BIN_NAME} PRIVATE "${IR_CHAT_SRC_DIR}")
target_compile_definitions(${IR_CHAT_BIN_NAME} PRIVATE
    IR_CHAT_USE_MOCK_RADIO=$<IF:$<BOOL:${IR_CHAT_USE_SDL}>,1,0>
    IR_CHAT_ENABLE_LINUX_RADIO=$<IF:$<BOOL:${IR_CHAT_USE_SDL}>,0,1>
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${IR_CHAT_BIN_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(${IR_CHAT_BIN_NAME} PRIVATE
    lvgl::lvgl
    smooth_ui_toolkit
    spdlog::spdlog
    Threads::Threads
    ${IR_CHAT_SDL_LIBS}
    ${IR_CHAT_SYSTEM_LIBS}
)

set_target_properties(${IR_CHAT_BIN_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${IR_CHAT_OUTPUT_DIR}"
)

if(IR_CHAT_BUILD_TESTS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "IR_CHAT_BUILD_TESTS requires Linux LIRC headers.")
    endif()

    enable_testing()
    add_executable(ir_chat_codec_test
        "${IR_CHAT_SRC_DIR}/radio/ir_chat_codec.cpp"
        "${IR_CHAT_ROOT}/tests/ir_chat_codec_test.cpp"
    )
    target_include_directories(ir_chat_codec_test PRIVATE "${IR_CHAT_SRC_DIR}")
    add_test(NAME ir_chat_codec COMMAND ir_chat_codec_test)

    add_executable(ir_chat_lirc_device_discovery_test
        "${IR_CHAT_SRC_DIR}/radio/lirc_device_discovery.cpp"
        "${IR_CHAT_ROOT}/tests/lirc_device_discovery_test.cpp"
    )
    target_include_directories(ir_chat_lirc_device_discovery_test PRIVATE "${IR_CHAT_SRC_DIR}")
    target_link_libraries(ir_chat_lirc_device_discovery_test PRIVATE spdlog::spdlog)
    add_test(NAME ir_chat_lirc_device_discovery COMMAND ir_chat_lirc_device_discovery_test)
endif()

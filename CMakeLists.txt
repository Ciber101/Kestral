cmake_minimum_required(VERSION 3.25)
project(Kestral VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCASCADE CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)    # font rendering (Dear ImGui) and sketch text outlines on Linux
find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)   # printers on the local network (Bambu Lab MQTT / FTPS)
find_package(ZLIB REQUIRED)      # reading sliced 3MF files

file(GLOB_RECURSE KESTRAL_SOURCES CONFIGURE_DEPENDS src/*.cpp)

if(WIN32)
    enable_language(RC)
    list(APPEND KESTRAL_SOURCES kestral.rc)
    # App icon (Explorer, taskbar, title bar: GLFW loads the resource named GLFW_ICON). Written with
    # the full path so the resource compiler finds it from any build folder.
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/kestral_icon.rc "GLFW_ICON ICON \"${CMAKE_CURRENT_SOURCE_DIR}/assets/kestral.ico\"\n")
    list(APPEND KESTRAL_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/kestral_icon.rc)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/kestral_icon.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/kestral.ico)
endif()

# Dear ImGui, built here from third_party/imgui (unchanged 1.92.9) with its item hooks on: Kestral
# knows every button, menu and field by name (src/ui/UiItems.cpp), so MCP apps can click by label.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(kestral_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
target_include_directories(kestral_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/cpp ${IMGUI_DIR}/misc/freetype)
target_compile_definitions(kestral_imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE IMGUI_ENABLE_FREETYPE)
target_link_libraries(kestral_imgui PUBLIC glfw Vulkan::Vulkan Freetype::Freetype)
if(MSVC)
    target_compile_options(kestral_imgui PRIVATE /utf-8 /MP)
endif()

add_executable(Kestral WIN32 ${KESTRAL_SOURCES})
target_include_directories(Kestral PRIVATE src ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(Kestral PRIVATE
    kestral_imgui glfw glm::glm Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
    ${OpenCASCADE_LIBRARIES})

if(MSVC)
    # /EHa lets hardware faults inside the geometry kernel become catchable errors (never crash).
    target_compile_options(Kestral PRIVATE /EHa /W3 /permissive- /utf-8 /MP /Zc:__cplusplus)
    target_compile_definitions(Kestral PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_options(Kestral PRIVATE /ENTRY:mainCRTStartup)
    target_link_libraries(Kestral PRIVATE dbghelp comdlg32 shell32 ole32 advapi32 winhttp crypt32 ws2_32 bcrypt)
    set_property(TARGET Kestral PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Kestral>")
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(Kestral PRIVATE Threads::Threads Freetype::Freetype ${CMAKE_DL_LIBS})   # FreeType: sketch text outlines (Windows uses GDI)
    target_compile_options(Kestral PRIVATE -Wall -Wno-unused-parameter)
    # "make install" / "cmake --install": the program, its icon and a menu entry.
    include(GNUInstallDirs)
    install(TARGETS Kestral RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES assets/kestral_icon_256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME kestral.png)
    install(FILES assets/kestral_icon.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME kestral.svg)
    install(FILES assets/kestral.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

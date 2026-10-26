# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.16...3.26)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${PROJECT_NAME} MODULE)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::frontend-api)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)

find_package(Qt6 COMPONENTS Widgets Core Network)
if(BUILD_OUT_OF_TREE)
  if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
    find_package(Qt6 REQUIRED Gui)
  endif()
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets Qt::Network)

if((OS_LINUX OR OS_FREEBSD OR OS_OPENBSD) AND Qt6_VERSION VERSION_LESS "6.9.0")
  find_package(Qt6 COMPONENTS GuiPrivate)
  target_link_libraries(${PROJECT_NAME} PRIVATE Qt::GuiPrivate)
endif()

target_compile_options(
${PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                -Wno-comma>)
set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_target_properties(
${PROJECT_NAME}
PROPERTIES AUTOMOC ON
            AUTOUIC ON
            AUTORCC ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h)

if(OS_WINDOWS)
  configure_file(cmake/windows/resources/installer-Windows.iss.in "${CMAKE_CURRENT_BINARY_DIR}/installer-Windows.generated.iss")
endif()

target_sources(${PROJECT_NAME} PRIVATE
  src/aitum-stream-suite.cpp
  src/component-source.c
  resources.qrc
  src/version.h)

target_sources(${PROJECT_NAME} PRIVATE
  src/docks/canvas-dock.cpp
  src/docks/canvas-dock.hpp
  src/docks/canvas-clone-dock.cpp
  src/docks/canvas-clone-dock.hpp
  src/docks/native-dock-tabs.cpp
  src/docks/native-dock-tabs.hpp
  src/docks/main-canvas-context-fix.cpp
  src/docks/main-canvas-context-fix.hpp
  src/docks/controls-other-options.cpp
  src/docks/controls-other-options.hpp
  src/docks/output-dock.cpp
  src/docks/output-dock.hpp
  src/docks/stream-info-dock.cpp
  src/docks/stream-info-dock.hpp
  src/docks/sources-dock.cpp
  src/docks/sources-dock.hpp)

target_sources(${PROJECT_NAME} PRIVATE
  src/dialogs/config-dialog.cpp
  src/dialogs/config-dialog.hpp
  src/dialogs/ffmpeg-output-dialog.cpp
  src/dialogs/ffmpeg-output-dialog.hpp
  src/dialogs/name-dialog.cpp
  src/dialogs/name-dialog.hpp
  src/dialogs/source-select-dialog.cpp
  src/dialogs/source-select-dialog.hpp
  src/dialogs/record-output-dialog.cpp
  src/dialogs/record-output-dialog.hpp
  src/dialogs/recording-names-dialog.cpp
  src/dialogs/recording-names-dialog.hpp
  src/dialogs/stream-output-dialog.cpp
  src/dialogs/stream-output-dialog.hpp)

target_sources(${PROJECT_NAME} PRIVATE
  src/utils/color.cpp
  src/utils/event-filter.cpp
  src/utils/icon.cpp
  src/utils/obs-websocket.cpp
  src/utils/widgets/focus-scroll-spinbox.cpp
  src/utils/widgets/hotkey-edit.cpp
  src/utils/widgets/locked-checkbox.cpp
  src/utils/widgets/output-widget.cpp
  src/utils/widgets/slider-ignore-scroll.cpp
  src/utils/widgets/absolute-slider.cpp
  src/utils/widgets/media-controls.cpp
  src/utils/widgets/source-toolbar.cpp
  src/utils/widgets/scene-tree.cpp
  src/utils/widgets/scene-tree.hpp
  src/utils/widgets/source-tree.cpp
  src/utils/widgets/stream-key-input.cpp
  src/utils/widgets/switching-splitter.cpp
  src/utils/widgets/projector.cpp
  src/utils/widgets/qt-display.cpp
  src/utils/widgets/visibility-checkbox.cpp
  src/utils/color.hpp
  src/utils/event-filter.hpp
  src/utils/icon.hpp
  src/utils/widgets/focus-scroll-spinbox.hpp
  src/utils/widgets/hotkey-edit.hpp
  src/utils/widgets/locked-checkbox.hpp
  src/utils/widgets/output-widget.hpp
  src/utils/widgets/slider-ignore-scroll.hpp
  src/utils/widgets/absolute-slider.hpp
  src/utils/widgets/media-controls.hpp
  src/utils/widgets/source-toolbar.hpp
  src/utils/widgets/source-tree.hpp
  src/utils/widgets/stream-key-input.hpp
  src/utils/widgets/switching-splitter.hpp
  src/utils/widgets/projector.hpp
  src/utils/widgets/qt-display.hpp
  src/utils/widgets/visibility-checkbox.hpp)

if(BUILD_OUT_OF_TREE)
	set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
else()
	set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/aitum" PREFIX "")
endif()

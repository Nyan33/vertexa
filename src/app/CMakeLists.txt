add_library(vx_app STATIC
    Theme.h Theme.cpp
    Icons.h Icons.cpp
    Widgets.h Widgets.cpp
    Editor.h Editor.cpp EditorCommands.cpp
    CrashHandler.h CrashHandler.cpp
    StageView.h StageView.cpp
    StageCanvas.h StageCanvas.cpp
    Dialogs.h Dialogs.cpp
    DemoDocument.h DemoDocument.cpp
    MainWindow.h MainWindow.cpp
    tools/Tool.h
    tools/DrawTools.h tools/DrawTools.cpp
    tools/BasicTools.h tools/BasicTools.cpp
    tools/SelectTools.h tools/SelectTools.cpp
    panels/ToolsPanel.h panels/ToolsPanel.cpp
    panels/TimelinePanel.h panels/TimelinePanel.cpp
    panels/PropertiesPanel.h panels/PropertiesPanel.cpp
    panels/LibraryPanel.h panels/LibraryPanel.cpp
    panels/ColorPanel.h panels/ColorPanel.cpp
    panels/BrushPanel.h panels/BrushPanel.cpp
)
target_include_directories(vx_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vx_app PUBLIC vx_render Qt6::Widgets)
target_compile_definitions(vx_app PUBLIC VERTEXA_VERSION="${PROJECT_VERSION}")
# Crash reports name the build: the commit on CI, "local" otherwise.
set(VERTEXA_BUILD_ID "local")
if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 10 VERTEXA_BUILD_ID)
endif()
set_source_files_properties(CrashHandler.cpp PROPERTIES COMPILE_DEFINITIONS VERTEXA_BUILD_ID="${VERTEXA_BUILD_ID}")
if(WIN32)
    target_link_libraries(vx_app PRIVATE dbghelp)
endif()

qt_add_executable(vertexa WIN32 MACOSX_BUNDLE main.cpp)

# Application icon: Windows resource, macOS bundle icon, window icon (qrc).
if(WIN32)
    target_sources(vertexa PRIVATE "${PROJECT_SOURCE_DIR}/resources/vertexa.rc")
elseif(APPLE)
    set(VERTEXA_ICNS "${PROJECT_SOURCE_DIR}/resources/icons/vertexa.icns")
    set_source_files_properties("${VERTEXA_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(vertexa PRIVATE "${VERTEXA_ICNS}")
endif()
qt_add_resources(vertexa "icons"
    PREFIX "/icons"
    BASE "${PROJECT_SOURCE_DIR}/resources/icons"
    FILES "${PROJECT_SOURCE_DIR}/resources/icons/vertexa-256.png"
)

qt_add_resources(vertexa "fonts"
    PREFIX "/fonts"
    BASE "${PROJECT_SOURCE_DIR}/resources/fonts"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/fonts/Inter-Regular.otf"
        "${PROJECT_SOURCE_DIR}/resources/fonts/Inter-SemiBold.otf"
        "${PROJECT_SOURCE_DIR}/resources/fonts/InterDisplay-Black.otf"
)

target_link_libraries(vertexa PRIVATE vx_app)
# Function names in the stacks of crash reports (Linux).
set_target_properties(vertexa PROPERTIES ENABLE_EXPORTS ON)
set_target_properties(vertexa PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Vertexa"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.vertexa.Vertexa"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE vertexa.icns
)

install(TARGETS vertexa
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
if(UNIX AND NOT APPLE)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/vertexa.desktop" DESTINATION share/applications)
    install(FILES "${PROJECT_SOURCE_DIR}/resources/icons/vertexa.svg" DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/resources/fonts/OFL.txt" DESTINATION share/doc/vertexa)
endif()

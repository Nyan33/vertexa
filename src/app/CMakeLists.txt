add_library(vx_app STATIC
    Theme.h Theme.cpp
    Icons.h Icons.cpp
    Widgets.h Widgets.cpp
    Editor.h Editor.cpp EditorCommands.cpp
    StageView.h StageView.cpp
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

qt_add_executable(vertexa WIN32 MACOSX_BUNDLE main.cpp)

qt_add_resources(vertexa "fonts"
    PREFIX "/fonts"
    BASE "${PROJECT_SOURCE_DIR}/resources/fonts"
    FILES
        "${PROJECT_SOURCE_DIR}/resources/fonts/Inter-Regular.otf"
        "${PROJECT_SOURCE_DIR}/resources/fonts/Inter-SemiBold.otf"
        "${PROJECT_SOURCE_DIR}/resources/fonts/InterDisplay-Black.otf"
)

target_link_libraries(vertexa PRIVATE vx_app)
set_target_properties(vertexa PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Vertexa"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.vertexa.Vertexa"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

install(TARGETS vertexa
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

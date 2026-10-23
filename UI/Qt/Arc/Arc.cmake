# Arc-style chrome. Included from UI/Qt/CMakeLists.txt (the one upstream hook in that file).
target_sources(ladybird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ArcChrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcImporter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcSidebar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/CommandBar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/FloatingPanel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarTree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarWidgets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceStore.cpp
)

# Monochrome space icons come from SF Symbols on macOS; other platforms fall back to letters.
if (APPLE)
    target_sources(ladybird PRIVATE ${CMAKE_CURRENT_LIST_DIR}/SymbolIcon.mm ${CMAKE_CURRENT_LIST_DIR}/WindowControls.mm)
else()
    target_sources(ladybird PRIVATE ${CMAKE_CURRENT_LIST_DIR}/SymbolIconOther.cpp ${CMAKE_CURRENT_LIST_DIR}/WindowControlsOther.cpp)
endif()

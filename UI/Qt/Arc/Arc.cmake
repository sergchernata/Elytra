# Arc-style chrome. Included from UI/Qt/CMakeLists.txt (the one upstream hook in that file).
target_sources(ladybird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ArcChrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcIcons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcImportParser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcImporter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcSidebar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/CommandBar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/CustomGlyphs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/DownloadsFormat.cpp
    ${CMAKE_CURRENT_LIST_DIR}/FloatingPanel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ProfileSessions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarTree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarWidgets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceStoreProfile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SymbolButton.cpp
)

# The importer reads Arc's (Chromium) icon databases.
if (CMAKE_VERSION VERSION_GREATER_EQUAL 4.3.1)
    target_link_libraries(ladybird PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(ladybird PRIVATE SQLite::SQLite3)
endif()

# Monochrome space icons come from SF Symbols on macOS; other platforms fall back to letters.
if (APPLE)
    target_sources(ladybird PRIVATE ${CMAKE_CURRENT_LIST_DIR}/SymbolIcon.mm ${CMAKE_CURRENT_LIST_DIR}/WindowControls.mm)
else()
    target_sources(ladybird PRIVATE ${CMAKE_CURRENT_LIST_DIR}/SymbolIconOther.cpp ${CMAKE_CURRENT_LIST_DIR}/WindowControlsOther.cpp)
endif()

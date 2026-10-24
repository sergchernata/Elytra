# Arc-style chrome. Included from UI/Qt/CMakeLists.txt (the one upstream hook in that file).
target_sources(ladybird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/AddressBar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcChrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcIcons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcImportParser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcImporter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcSelfTest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcShortcuts.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcSidebar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/CommandBar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/CustomGlyphs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/DownloadsFormat.cpp
    ${CMAKE_CURRENT_LIST_DIR}/FloatingPanel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/LittleArc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/MiniPlayer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/OnePassword.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ProfileSessions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ReleaseInfo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ReleaseSignature.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarTree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarWidgets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceSlide.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceStoreProfile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SymbolButton.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Toast.cpp
    ${CMAKE_CURRENT_LIST_DIR}/UpdateChecker.cpp
)

# The update notice asks GitHub for the latest release through Ladybird's own network stack, and checks its signature.
target_link_libraries(ladybird PRIVATE LibCrypto LibHTTP LibRequests)

# The importer reads Arc's (Chromium) icon databases.
if (CMAKE_VERSION VERSION_GREATER_EQUAL 4.3.1)
    target_link_libraries(ladybird PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(ladybird PRIVATE SQLite::SQLite3)
endif()

# Monochrome space icons come from SF Symbols on macOS; other platforms fall back to letters.
if (APPLE)
    target_sources(ladybird PRIVATE ${CMAKE_CURRENT_LIST_DIR}/SymbolIcon.mm ${CMAKE_CURRENT_LIST_DIR}/WindowControls.mm ${CMAKE_CURRENT_LIST_DIR}/LittleArcHotKey.mm)
    target_link_libraries(ladybird PRIVATE "-framework Carbon")
else()
    target_sources(ladybird PRIVATE ${CMAKE_CURRENT_LIST_DIR}/SymbolIconOther.cpp ${CMAKE_CURRENT_LIST_DIR}/WindowControlsOther.cpp)
endif()

# Release builds (see .github/workflows/arc-release-macos.yml) know their tag and repository, to notice newer releases.
set(ARC_RELEASE_TAG "" CACHE STRING "Arc fork: this build's release tag (release builds only)")
set(ARC_RELEASE_REPOSITORY "" CACHE STRING "Arc fork: the GitHub repository releases come from (owner/name)")
if (ARC_RELEASE_TAG AND ARC_RELEASE_REPOSITORY)
    # Only the file that uses them, so a new tag doesn't change (and recompile) every other file.
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/UpdateChecker.cpp PROPERTIES
        COMPILE_DEFINITIONS "ARC_RELEASE_TAG=\"${ARC_RELEASE_TAG}\";ARC_RELEASE_REPOSITORY=\"${ARC_RELEASE_REPOSITORY}\"")
endif()

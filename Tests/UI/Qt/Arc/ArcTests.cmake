# Tests for the Arc fork's Qt chrome. Included at the end of Tests/UI/Qt/CMakeLists.txt (fork-only hook),
# after QtTestMain and deploy_qt_offscreen_platform_plugin() are defined.

set(ARC_SOURCE_DIR "${LADYBIRD_SOURCE_DIR}/UI/Qt/Arc")

ladybird_test("Arc/TestArcSpaceStore.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestArcSpaceStore PRIVATE "${ARC_SOURCE_DIR}/SpaceStore.cpp")
set_target_properties(TestArcSpaceStore PROPERTIES AUTOMOC ON)
deploy_qt_offscreen_platform_plugin(TestArcSpaceStore)

ladybird_test("Arc/TestArcWidgets.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestArcWidgets PRIVATE
    "${ARC_SOURCE_DIR}/CustomGlyphs.cpp"
    "${ARC_SOURCE_DIR}/DownloadsFormat.cpp"
    "${ARC_SOURCE_DIR}/FloatingPanel.cpp"
    "${ARC_SOURCE_DIR}/SidebarTree.cpp"
    "${ARC_SOURCE_DIR}/SymbolButton.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
if (APPLE)
    target_sources(TestArcWidgets PRIVATE "${ARC_SOURCE_DIR}/SymbolIcon.mm")
    target_link_libraries(TestArcWidgets PRIVATE "-framework AppKit")
else()
    target_sources(TestArcWidgets PRIVATE "${ARC_SOURCE_DIR}/SymbolIconOther.cpp")
endif()
set_target_properties(TestArcWidgets PROPERTIES AUTOMOC ON)
deploy_qt_offscreen_platform_plugin(TestArcWidgets)

ladybird_test("Arc/TestArcImportParser.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestArcImportParser PRIVATE "${ARC_SOURCE_DIR}/ArcImportParser.cpp" "${ARC_SOURCE_DIR}/CustomGlyphs.cpp" "${ARC_SOURCE_DIR}/SpaceStore.cpp")
if (APPLE)
    target_sources(TestArcImportParser PRIVATE "${ARC_SOURCE_DIR}/SymbolIcon.mm")
    target_link_libraries(TestArcImportParser PRIVATE "-framework AppKit")
else()
    target_sources(TestArcImportParser PRIVATE "${ARC_SOURCE_DIR}/SymbolIconOther.cpp")
endif()
set_target_properties(TestArcImportParser PROPERTIES AUTOMOC ON)
deploy_qt_offscreen_platform_plugin(TestArcImportParser)

# Profiles: real WebContent and RequestServer processes, run headlessly (modeled on upstream's
# Tests/LibWebView/TestRequestServerClientSessions).
add_executable(TestArcProfileSessions "${CMAKE_CURRENT_LIST_DIR}/TestArcProfileSessions.cpp"
    "${ARC_SOURCE_DIR}/ArcIcons.cpp" "${ARC_SOURCE_DIR}/ProfileSessions.cpp" "${ARC_SOURCE_DIR}/SpaceStore.cpp" "${ARC_SOURCE_DIR}/SpaceStoreProfile.cpp")
set_target_properties(TestArcProfileSessions PROPERTIES AUTOMOC ON)
add_dependencies(TestArcProfileSessions ladybird_build_resource_files ${ladybird_helper_processes})
target_link_libraries(TestArcProfileSessions PRIVATE AK LibCore LibFileSystem LibGfx LibHTTP LibIPC LibMain LibURL LibWeb LibWebView Qt::Core Qt::Gui)
if (APPLE)
    target_compile_definitions(TestArcProfileSessions PRIVATE LADYBIRD_BINARY_PATH="$<TARGET_FILE_DIR:ladybird>")
endif()
add_test(NAME TestArcProfileSessions COMMAND $<TARGET_FILE:TestArcProfileSessions>)
set_tests_properties(TestArcProfileSessions PROPERTIES TIMEOUT 120 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})

ladybird_test("Arc/TestArcReleaseInfo.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestArcReleaseInfo PRIVATE "${ARC_SOURCE_DIR}/ReleaseInfo.cpp")
deploy_qt_offscreen_platform_plugin(TestArcReleaseInfo)

# The sidebar's integration tests, in a real browser window (see UI/Qt/Arc/ArcSelfTest.cpp). Needs a desktop session.
if (APPLE)
    add_test(NAME TestArcSelfTest COMMAND "${CMAKE_CURRENT_LIST_DIR}/run-arc-self-test.sh" "$<TARGET_FILE:ladybird>")
    set_tests_properties(TestArcSelfTest PROPERTIES TIMEOUT 300 ENVIRONMENT LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR})
endif()

ladybird_test("Arc/TestArcEdgeCases.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestArcEdgeCases PRIVATE
    "${ARC_SOURCE_DIR}/ArcIcons.cpp"
    "${ARC_SOURCE_DIR}/CustomGlyphs.cpp"
    "${ARC_SOURCE_DIR}/ReleaseInfo.cpp"
    "${ARC_SOURCE_DIR}/SidebarTree.cpp"
    "${ARC_SOURCE_DIR}/SymbolButton.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
if (APPLE)
    target_sources(TestArcEdgeCases PRIVATE "${ARC_SOURCE_DIR}/SymbolIcon.mm")
    target_link_libraries(TestArcEdgeCases PRIVATE "-framework AppKit")
else()
    target_sources(TestArcEdgeCases PRIVATE "${ARC_SOURCE_DIR}/SymbolIconOther.cpp")
endif()
set_target_properties(TestArcEdgeCases PROPERTIES AUTOMOC ON)
deploy_qt_offscreen_platform_plugin(TestArcEdgeCases)

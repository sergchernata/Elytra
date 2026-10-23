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

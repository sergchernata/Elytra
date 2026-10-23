# Arc-style chrome. Included from UI/Qt/CMakeLists.txt (the one upstream hook in that file).
target_sources(ladybird PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ArcChrome.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcImporter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ArcSidebar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/CommandBar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SidebarTree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/SpaceStore.cpp
)

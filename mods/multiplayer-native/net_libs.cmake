# ---- ENet6 (networking) and miniupnpc (UPnP port mapping for internet hosting), static, used only by this mod and its tests ----
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/enet6 ${CMAKE_BINARY_DIR}/enet6 EXCLUDE_FROM_ALL)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/miniupnp/miniupnpc ${CMAKE_BINARY_DIR}/miniupnpc EXCLUDE_FROM_ALL)
# Linked into a shared library, which needs position-independent code on Linux.
set_target_properties(enet6 libminiupnpc-static PROPERTIES POSITION_INDEPENDENT_CODE ON)

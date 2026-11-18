# ---- ENet6 (networking) and miniupnpc (UPnP port mapping for internet hosting), static, used only by this mod and its tests ----
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/enet6 ${CMAKE_BINARY_DIR}/enet6 EXCLUDE_FROM_ALL)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/miniupnp/miniupnpc ${CMAKE_BINARY_DIR}/miniupnpc EXCLUDE_FROM_ALL)
# Monocypher (password games' key exchange and sealing), its core plus the optional SHA-512.
set(MONOCYPHER_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/monocypher/src)
add_library(monocypher STATIC EXCLUDE_FROM_ALL ${MONOCYPHER_DIR}/monocypher.c ${MONOCYPHER_DIR}/optional/monocypher-ed25519.c)
target_include_directories(monocypher PUBLIC ${MONOCYPHER_DIR} ${MONOCYPHER_DIR}/optional)
# The browser build's ENet sockets are WebTransport sessions (web/web_socket.c); unix.c keeps the rest, its socket functions renamed out of the way.
if (EMSCRIPTEN)
    set(enet_web_renames enet_socketset_select=enet_unix_socketset_select)

    foreach(f bind get_address listen create set_option get_option connect accept shutdown destroy send receive wait)
        list(APPEND enet_web_renames enet_socket_${f}=enet_unix_socket_${f})
    endforeach()

    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/lib/enet6/src/unix.c TARGET_DIRECTORY enet6 PROPERTIES COMPILE_DEFINITIONS "${enet_web_renames}")
    target_sources(enet6 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/web/web_socket.c)
    target_include_directories(enet6 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/web>)
endif()

# Linked into a shared library, which needs position-independent code on Linux.
set_target_properties(enet6 libminiupnpc-static monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)

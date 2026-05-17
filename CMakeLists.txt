cmake_minimum_required(VERSION 3.29)
project(lua_bencode C)

set(CMAKE_C_STANDARD 11)

include_directories(E:\\pyproject\\Lua_5.4_WinBuild\\lua-5.5.0\\src)
link_directories(E:\\pyproject\\Lua_5.4_WinBuild\\cmake-build-release)


add_library(bencode SHARED src/lbencode.c src/util.c src/sds.c)
target_link_libraries(bencode lua)
set_target_properties(bencode PROPERTIES PREFIX "")
#add_compile_definitions(DEBUG=1)

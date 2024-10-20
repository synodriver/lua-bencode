cmake_minimum_required(VERSION 3.29)
project(lua_bencode C)

set(CMAKE_C_STANDARD 11)

include_directories(D:\\conda\\envs\\py310\\Library\\include)
link_directories("D:\\conda\\envs\\py310\\Library\\lib" D:\\conda\\envs\\py310\\Library\\bin)


add_library(bencode SHARED src/lbencode.c src/util.c src/sds.c)
target_link_libraries(bencode lua)
set_target_properties(bencode PROPERTIES PREFIX "")
#add_compile_definitions(DEBUG=1)

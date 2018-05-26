cmake_minimum_required (VERSION 2.6)
project (NS)

set (NS_SRCS
  src/nvenc_srv.c
)

set (NS_LIBS
  nanomsg
  avformat
  avcodec
  avutil
  pthread
  dl
  m
)

add_executable (nvenc_srv ${NS_SRCS})
target_link_libraries (nvenc_srv ${NS_LIBS})

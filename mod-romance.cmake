# mod-romance build hook. Included by AzerothCore's modules/CMakeLists.txt after
# the `modules` target exists. All sources under src/ (core + ac) are collected by
# AzerothCore automatically; this file only adds platform libraries.
#
# The engine-independent core uses plain sockets for the local Ollama HTTP API,
# so no extra third-party dependency (curl, httplib, json) is required.

if(TARGET modules)
  if(WIN32)
    target_link_libraries(modules PRIVATE ws2_32)
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(modules PRIVATE Threads::Threads)
endif()

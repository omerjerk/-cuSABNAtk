CUDA_ADD_EXECUTABLE(query query.cpp)
TARGET_LINK_LIBRARIES(query gpu_util)

INSTALL(TARGETS query RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/examples)

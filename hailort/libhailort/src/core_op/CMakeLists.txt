cmake_minimum_required(VERSION 3.0.0)

set(SRC_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/core_op.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/resource_manager/resource_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_manager/resource_manager_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_manager/config_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_manager/intermediate_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_manager/channel_allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_manager/context_switch_buffer_builder.cpp
)

set(HAILORT_CPP_SOURCES ${HAILORT_CPP_SOURCES} ${SRC_FILES} PARENT_SCOPE)

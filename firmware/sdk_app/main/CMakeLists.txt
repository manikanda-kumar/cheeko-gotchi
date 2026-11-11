# Compiles the selected app's src/*.cc plus the app_main shim.
set(CHEEKO_APP "../../examples/hello_display" CACHE STRING "SDK app directory (contains src/)")
get_filename_component(_app_dir "${CHEEKO_APP}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB _app_srcs "${_app_dir}/src/*.cc" "${_app_dir}/src/*.cpp")
if(NOT _app_srcs)
  message(FATAL_ERROR "No app sources in ${_app_dir}/src")
endif()
message(STATUS "Cheeko app: ${_app_dir}")
idf_component_register(
    SRCS "main.cc" ${_app_srcs}
    REQUIRES cheeko_runtime
)

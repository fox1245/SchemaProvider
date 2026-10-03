find_path(yyjson_INCLUDE_DIR NAMES yyjson.h
  HINTS "${YYJSON_ROOT}" ENV YYJSON_ROOT
  PATH_SUFFIXES include src)
find_library(yyjson_LIBRARY NAMES yyjson
  HINTS "${YYJSON_ROOT}" ENV YYJSON_ROOT
  PATH_SUFFIXES lib lib64)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(yyjson
  REQUIRED_VARS yyjson_LIBRARY yyjson_INCLUDE_DIR)

if(yyjson_FOUND AND NOT TARGET yyjson::yyjson)
  add_library(yyjson::yyjson UNKNOWN IMPORTED)
  set_target_properties(yyjson::yyjson PROPERTIES
    IMPORTED_LOCATION "${yyjson_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${yyjson_INCLUDE_DIR}")
endif()
mark_as_advanced(yyjson_INCLUDE_DIR yyjson_LIBRARY)

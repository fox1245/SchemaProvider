set(SP_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/SchemaProvider")

install(TARGETS sp_core sp_json sp_descriptor sp_codecs sp_wire sp_transport sp_runtime
  EXPORT SchemaProviderTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Preserve the public include spelling while keeping headers out of the generic
# include root. Private scheduling seams, canaries and qualification grants are
# deliberately not part of this package.
install(DIRECTORY
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/json"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/descriptor"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/codecs"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/transport"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/configuration"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/SchemaProvider"
  FILES_MATCHING PATTERN "*.h"
  PATTERN "native_archive_fs.h" EXCLUDE
  PATTERN "io_thread.h" EXCLUDE)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/client.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/policy.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/SchemaProvider/runtime")
install(FILES "${SP_GENERATED_INCLUDE_DIR}/sp/config_defaults.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/SchemaProvider/sp")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SchemaProviderConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/SchemaProviderConfig.cmake"
  INSTALL_DESTINATION "${SP_INSTALL_CMAKE_DIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/SchemaProviderConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY ExactVersion)
install(EXPORT SchemaProviderTargets
  FILE SchemaProviderTargets.cmake
  NAMESPACE SchemaProvider::
  DESTINATION "${SP_INSTALL_CMAKE_DIR}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/SchemaProviderConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/SchemaProviderConfigVersion.cmake"
  DESTINATION "${SP_INSTALL_CMAKE_DIR}")
if(SP_EXTERNAL_YYJSON)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findyyjson.cmake"
    DESTINATION "${SP_INSTALL_CMAKE_DIR}")
endif()

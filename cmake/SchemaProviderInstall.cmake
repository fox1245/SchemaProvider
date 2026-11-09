set(SP_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/SchemaProvider")

set(SP_INSTALL_TARGETS sp_core sp_json sp_descriptor sp_codecs sp_wire sp_runtime)
if(SP_TRANSPORT)
  list(APPEND SP_INSTALL_TARGETS sp_transport)
endif()
if(SP_BUILD_JS_HOST)
  list(APPEND SP_INSTALL_TARGETS sp_js_host)
endif()
install(TARGETS ${SP_INSTALL_TARGETS}
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
  PATTERN "native_archive_custody_format.h" EXCLUDE
  PATTERN "native_archive_custody_internal.h" EXCLUDE
  PATTERN "io_thread.h" EXCLUDE
  PATTERN "reason.h" EXCLUDE
  PATTERN "served_model.h" EXCLUDE)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/client.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/host.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/cost.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/policy.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/SchemaProvider/runtime")
if(SP_BUILD_JS_HOST)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/js_host.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/SchemaProvider/runtime")
endif()
install(FILES "${SP_GENERATED_INCLUDE_DIR}/sp/config_defaults.h" "${SP_GENERATED_INCLUDE_DIR}/sp/build_config.h"
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
if(SP_SODIUM)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/FindSodium.cmake"
    DESTINATION "${SP_INSTALL_CMAKE_DIR}")
  install(CODE "
    if(NOT EXISTS \"${SP_SODIUM_STAGED_LICENSE}\")
      execute_process(
        COMMAND \"${Python3_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_libsodium.py\"
          --dependencies \"${CMAKE_CURRENT_SOURCE_DIR}/config/dependencies.json\"
          --unpack-dir \"${SP_SODIUM_UNPACK_DIR}\"
        RESULT_VARIABLE _sp_sodium_lic_res)
      if(NOT _sp_sodium_lic_res EQUAL 0)
        message(FATAL_ERROR \"Failed to unpack pinned libsodium source for licence installation\")
      endif()
      file(MAKE_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}/third_party/libsodium\")
      file(COPY_FILE
        \"${SP_SODIUM_UNPACK_DIR}/libsodium-${SP_SODIUM_PINNED_VERSION}/LICENSE\"
        \"${SP_SODIUM_STAGED_LICENSE}\"
        ONLY_IF_DIFFERENT)
    endif()
  ")
  install(FILES "${SP_SODIUM_STAGED_LICENSE}"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/third_party/libsodium")
endif()

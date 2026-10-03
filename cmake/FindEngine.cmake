# Locates the UNIGINE engine import library of the SDK the project is built
# against and exposes it as the imported target Unigine::Engine.
#
# Inputs:
#   UNIGINE_LIB_DIR / UNIGINE_BIN_DIR - SDK library search paths
#   UNIGINE_DOUBLE                    - use the double-precision libraries
#
# Part of the artist_tool_unigine plugin (MIT); written from scratch,
# not affiliated with or derived from UNIGINE SDK build files.

if (TARGET Unigine::Engine)
	set(Engine_FOUND TRUE)
	return()
endif()

set(_engine_base "Unigine")
if (UNIGINE_DOUBLE)
	string(APPEND _engine_base "_double")
endif()
string(APPEND _engine_base "_x64")

find_library(Engine_LIBRARY_RELEASE
	NAMES ${_engine_base}
	PATHS ${UNIGINE_LIB_DIR} ${UNIGINE_BIN_DIR}
	NO_DEFAULT_PATH
	)
find_library(Engine_LIBRARY_DEBUG
	NAMES ${_engine_base}d
	PATHS ${UNIGINE_LIB_DIR} ${UNIGINE_BIN_DIR}
	NO_DEFAULT_PATH
	)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Engine DEFAULT_MSG Engine_LIBRARY_RELEASE)

if (Engine_FOUND)
	add_library(Unigine::Engine UNKNOWN IMPORTED)
	set_target_properties(Unigine::Engine PROPERTIES
		IMPORTED_LOCATION "${Engine_LIBRARY_RELEASE}"
		IMPORTED_LOCATION_RELEASE "${Engine_LIBRARY_RELEASE}"
		)
	# the store's "debug" plugin binaries are optimized RelWithDebInfo builds
	# linked against the d-suffixed engine libraries
	if (Engine_LIBRARY_DEBUG)
		set_target_properties(Unigine::Engine PROPERTIES
			IMPORTED_LOCATION_DEBUG "${Engine_LIBRARY_DEBUG}"
			IMPORTED_LOCATION_RELWITHDEBINFO "${Engine_LIBRARY_DEBUG}"
			)
	endif()
	set(Engine_LIBRARY ${Engine_LIBRARY_RELEASE})
endif()

unset(_engine_base)

# Locates the UNIGINE editor core import library of the SDK the project is
# built against and exposes it as the imported target Unigine::Editor.
#
# Inputs:
#   UNIGINE_LIB_DIR / UNIGINE_BIN_DIR - SDK library search paths
#   UNIGINE_DOUBLE                    - use the double-precision libraries
#
# Part of the artist_tool_unigine plugin (MIT); written from scratch,
# not affiliated with or derived from UNIGINE SDK build files.

if (TARGET Unigine::Editor)
	set(Editor_FOUND TRUE)
	return()
endif()

set(_editor_base "EditorCore")
if (UNIGINE_DOUBLE)
	string(APPEND _editor_base "_double")
endif()
string(APPEND _editor_base "_x64")

find_library(Editor_LIBRARY_RELEASE
	NAMES ${_editor_base}
	PATHS ${UNIGINE_LIB_DIR} ${UNIGINE_BIN_DIR}
	NO_DEFAULT_PATH
	)
find_library(Editor_LIBRARY_DEBUG
	NAMES ${_editor_base}d
	PATHS ${UNIGINE_LIB_DIR} ${UNIGINE_BIN_DIR}
	NO_DEFAULT_PATH
	)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Editor DEFAULT_MSG Editor_LIBRARY_RELEASE)

if (Editor_FOUND)
	add_library(Unigine::Editor UNKNOWN IMPORTED)
	set_target_properties(Unigine::Editor PROPERTIES
		IMPORTED_LOCATION "${Editor_LIBRARY_RELEASE}"
		IMPORTED_LOCATION_RELEASE "${Editor_LIBRARY_RELEASE}"
		)
	# the store's "debug" plugin binaries are optimized RelWithDebInfo builds
	# linked against the d-suffixed editor libraries
	if (Editor_LIBRARY_DEBUG)
		set_target_properties(Unigine::Editor PROPERTIES
			IMPORTED_LOCATION_DEBUG "${Editor_LIBRARY_DEBUG}"
			IMPORTED_LOCATION_RELWITHDEBINFO "${Editor_LIBRARY_DEBUG}"
			)
	endif()
	set(Editor_LIBRARY ${Editor_LIBRARY_RELEASE})
endif()

unset(_editor_base)

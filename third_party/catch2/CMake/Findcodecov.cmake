# Copyright (c)
# See the LICENSE file in the package base directory for details


option(ENABLE_COVERAGE "Enable coverage build." OFF)

set(COVERAGE_FLAG_CANDIDATES
	"-O0 -g -fprofile-arcs -ftest-coverage"

	"-O0 -g --coverage"
)


function (add_coverage TNAME)
	if (ENABLE_COVERAGE)
		foreach (TNAME ${ARGV})
			add_coverage_target(${TNAME})
		endforeach ()
	endif ()
endfunction (add_coverage)


function (coverage_evaluate)
	if (LCOV_FOUND)
		lcov_capture_initial()
		lcov_capture()
	endif (LCOV_FOUND)
endfunction ()


if (NOT ENABLE_COVERAGE)
	return()
endif ()




set(CMAKE_REQUIRED_QUIET_SAVE ${CMAKE_REQUIRED_QUIET})
set(CMAKE_REQUIRED_QUIET ${codecov_FIND_QUIETLY})

get_property(ENABLED_LANGUAGES GLOBAL PROPERTY ENABLED_LANGUAGES)
foreach (LANG ${ENABLED_LANGUAGES})
	set(COMPILER ${CMAKE_${LANG}_COMPILER_ID})
	if (NOT COVERAGE_${COMPILER}_FLAGS)
		foreach (FLAG ${COVERAGE_FLAG_CANDIDATES})
			if(NOT CMAKE_REQUIRED_QUIET)
				message(STATUS "Try ${COMPILER} code coverage flag = [${FLAG}]")
			endif()

			set(CMAKE_REQUIRED_FLAGS "${FLAG}")
			unset(COVERAGE_FLAG_DETECTED CACHE)

			if (${LANG} STREQUAL "C")
				include(CheckCCompilerFlag)
				check_c_compiler_flag("${FLAG}" COVERAGE_FLAG_DETECTED)

			elseif (${LANG} STREQUAL "CXX")
				include(CheckCXXCompilerFlag)
				check_cxx_compiler_flag("${FLAG}" COVERAGE_FLAG_DETECTED)

			elseif (${LANG} STREQUAL "Fortran")
				include(CheckFortranCompilerFlag OPTIONAL
					RESULT_VARIABLE INCLUDED)
				if (INCLUDED)
					check_fortran_compiler_flag("${FLAG}"
						COVERAGE_FLAG_DETECTED)
				elseif (NOT CMAKE_REQUIRED_QUIET)
					message("-- Performing Test COVERAGE_FLAG_DETECTED")
					message("-- Performing Test COVERAGE_FLAG_DETECTED - Failed"
						" (Check not supported)")
				endif ()
			endif()

			if (COVERAGE_FLAG_DETECTED)
				set(COVERAGE_${COMPILER}_FLAGS "${FLAG}"
					CACHE STRING "${COMPILER} flags for code coverage.")
				mark_as_advanced(COVERAGE_${COMPILER}_FLAGS)
				break()
			else ()
				message(WARNING "Code coverage is not available for ${COMPILER}"
				        " compiler. Targets using this compiler will be "
				        "compiled without it.")
			endif ()
		endforeach ()
	endif ()
endforeach ()

set(CMAKE_REQUIRED_QUIET ${CMAKE_REQUIRED_QUIET_SAVE})




function (codecov_lang_of_source FILE RETURN_VAR)
	get_filename_component(FILE_EXT "${FILE}" EXT)
	string(TOLOWER "${FILE_EXT}" FILE_EXT)
	string(SUBSTRING "${FILE_EXT}" 1 -1 FILE_EXT)

	get_property(ENABLED_LANGUAGES GLOBAL PROPERTY ENABLED_LANGUAGES)
	foreach (LANG ${ENABLED_LANGUAGES})
		list(FIND CMAKE_${LANG}_SOURCE_FILE_EXTENSIONS "${FILE_EXT}" TEMP)
		if (NOT ${TEMP} EQUAL -1)
			set(${RETURN_VAR} "${LANG}" PARENT_SCOPE)
			return()
		endif ()
	endforeach()

	set(${RETURN_VAR} "" PARENT_SCOPE)
endfunction ()


function (codecov_path_of_source FILE RETURN_VAR)
	string(REGEX MATCH "TARGET_OBJECTS:([^ >]+)" _source ${FILE})

	if (NOT "${_source}" STREQUAL "")
		set(${RETURN_VAR} "" PARENT_SCOPE)
		return()
	endif ()


	string(REPLACE "${CMAKE_CURRENT_BINARY_DIR}/" "" FILE "${FILE}")
	if(IS_ABSOLUTE ${FILE})
		file(RELATIVE_PATH FILE ${CMAKE_CURRENT_SOURCE_DIR} ${FILE})
	endif()

	string(REPLACE ".." "__" PATH "${FILE}")

	set(${RETURN_VAR} "${PATH}" PARENT_SCOPE)
endfunction()


function(add_coverage_target TNAME)
	get_target_property(TSOURCES ${TNAME} SOURCES)
	set(TARGET_COMPILER "")
	set(ADDITIONAL_FILES "")
	foreach (FILE ${TSOURCES})
		string(REGEX MATCH "TARGET_OBJECTS:([^ >]+)" _file ${FILE})
		if ("${_file}" STREQUAL "")
			codecov_lang_of_source(${FILE} LANG)
			if (LANG)
				list(APPEND TARGET_COMPILER ${CMAKE_${LANG}_COMPILER_ID})

				list(APPEND ADDITIONAL_FILES "${FILE}.gcno")
				list(APPEND ADDITIONAL_FILES "${FILE}.gcda")
			endif ()
		endif ()
	endforeach ()

	list(REMOVE_DUPLICATES TARGET_COMPILER)
	list(LENGTH TARGET_COMPILER NUM_COMPILERS)

	if (NUM_COMPILERS GREATER 1)
		message(WARNING "Can't use code coverage for target ${TNAME}, because "
		        "it will be compiled by incompatible compilers. Target will be "
		        "compiled without code coverage.")
		return()

	elseif (NUM_COMPILERS EQUAL 0)
		message(WARNING "Can't use code coverage for target ${TNAME}, because "
		        "it uses an unknown compiler. Target will be compiled without "
		        "code coverage.")
		return()

	elseif (NOT DEFINED "COVERAGE_${TARGET_COMPILER}_FLAGS")
		return()
	endif()


	set_property(TARGET ${TNAME} APPEND_STRING
		PROPERTY COMPILE_FLAGS " ${COVERAGE_${TARGET_COMPILER}_FLAGS}")
	set_property(TARGET ${TNAME} APPEND_STRING
		PROPERTY LINK_FLAGS " ${COVERAGE_${TARGET_COMPILER}_FLAGS}")


	set(CLEAN_FILES "")
	foreach (FILE ${ADDITIONAL_FILES})
		codecov_path_of_source(${FILE} FILE)
		list(APPEND CLEAN_FILES "CMakeFiles/${TNAME}.dir/${FILE}")
	endforeach()

	set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES
		"${CLEAN_FILES}")


	add_gcov_target(${TNAME})
	add_lcov_target(${TNAME})
endfunction(add_coverage_target)




find_package(Gcov)
find_package(Lcov)

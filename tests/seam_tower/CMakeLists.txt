get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
	${_TEST_NAME}_tests.cpp
	test_seam_tower_geometry.cpp
	test_seam_tower_cluster.cpp
	test_seam_tower_planner.cpp
	test_seam_tower_collision.cpp
	test_seam_tower_gcode.cpp
	test_seam_tower_config.cpp
	nanosvg_impl.cpp
	${PROJECT_SOURCE_DIR}/src/slic3r/Utils/BBLUtil.cpp
	${PROJECT_SOURCE_DIR}/src/slic3r/Utils/Http.cpp
)

# CMAKE_MACOSX_BUNDLE is ON for the app; keep this a plain binary so it is easy to run.
set_target_properties(${_TEST_NAME}_tests PROPERTIES MACOSX_BUNDLE FALSE)

target_include_directories(${_TEST_NAME}_tests PRIVATE
	${PROJECT_SOURCE_DIR}/src
	${PROJECT_SOURCE_DIR}/src/slic3r
	${PROJECT_SOURCE_DIR}/src/slic3r/Utils
)

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r
	libcurl
	OpenSSL::SSL OpenSSL::Crypto
)

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})

# SPDX-License-Identifier: GPL-2.0-or-later

option(PLUGIN_STANDARD_ALIS "Install the ALiS standard plugin" OFF)
option(ALIS_BUILD_TESTS "Build ALiS non-GUI logic tests" OFF)

if(PLUGIN_STANDARD_ALIS)
	project(ALIS_PLUGIN)

	# CloudCompare 2.13 uses Qt 5, while the 2.14 development line uses Qt 6.
	# Keep one plugin source tree and bind to the Qt major selected by the host.
	if(TARGET Qt6::Widgets)
		set(ALIS_QT_CORE Qt6::Core)
		set(ALIS_QT_WIDGETS Qt6::Widgets)
	elseif(TARGET Qt5::Widgets)
		set(ALIS_QT_CORE Qt5::Core)
		set(ALIS_QT_WIDGETS Qt5::Widgets)
	else()
		message(FATAL_ERROR "ALiS requires the Qt Core and Widgets targets provided by CloudCompare")
	endif()

	AddPlugin(NAME ${PROJECT_NAME})

	# Reuse the exact CSF implementation shipped in the pinned CloudCompare
	# source tree. qCSF exposes no reusable library/API target in v2.13.2, so
	# these algorithm files are compiled directly without copying or modifying
	# the third-party source. The qCSF GUI/plugin entry points stay excluded.
	set(ALIS_QCSF_DIR "${CMAKE_SOURCE_DIR}/plugins/core/Standard/qCSF")
	if(NOT EXISTS "${ALIS_QCSF_DIR}/include/CSF.h")
		message(FATAL_ERROR "ALiS requires the bundled CloudCompare qCSF source tree")
	endif()
	add_library(ALiSCSFCore STATIC
		"${ALIS_QCSF_DIR}/include/CSF.h"
		"${ALIS_QCSF_DIR}/include/Cloth.h"
		"${ALIS_QCSF_DIR}/include/Cloud2CloudDist.h"
		"${ALIS_QCSF_DIR}/include/Particle.h"
		"${ALIS_QCSF_DIR}/include/Rasterization.h"
		"${ALIS_QCSF_DIR}/include/Vec3.h"
		"${ALIS_QCSF_DIR}/include/wlPointCloud.h"
		"${ALIS_QCSF_DIR}/src/CSF.cpp"
		"${ALIS_QCSF_DIR}/src/Cloth.cpp"
		"${ALIS_QCSF_DIR}/src/Cloud2CloudDist.cpp"
		"${ALIS_QCSF_DIR}/src/Particle.cpp"
		"${ALIS_QCSF_DIR}/src/Rasterization.cpp"
	)
	target_include_directories(ALiSCSFCore PUBLIC "${ALIS_QCSF_DIR}/include")
	target_link_libraries(ALiSCSFCore PUBLIC CCCoreLib CCPluginAPI QCC_DB_LIB ${ALIS_QT_WIDGETS})
	target_compile_features(ALiSCSFCore PUBLIC cxx_std_14)
	set_target_properties(ALiSCSFCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

	add_library(ALiSAuditLogic STATIC
		include/AuditLogic.h
		include/WorkspaceLogic.h
		src/AuditLogic.cpp
		src/WorkspaceLogic.cpp
	)
	target_include_directories(ALiSAuditLogic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
	target_compile_features(ALiSAuditLogic PUBLIC cxx_std_14)
	set_target_properties(ALiSAuditLogic PROPERTIES POSITION_INDEPENDENT_CODE ON)

	target_sources(${PROJECT_NAME} PRIVATE
		include/ProcessingPreset.h
		include/CloudProfile.h
		src/ProcessingPreset.cpp
		src/CloudProfile.cpp
		src/WorkspaceProcessing.cpp
		src/WorkspaceVegetationModel.cpp
		src/WorkspaceFeatureRanges.cpp
		include/AnnotationGeometry.h
		include/AnnotationCanvas.h
		include/AnnotationStudio.h
		src/AnnotationCanvas.cpp
		src/AnnotationStudio.cpp
		include/GroundFilter.h
		include/FeatureEngine.h
		include/SessionModel.h
		include/TerrainEngine.h
		include/WorkspaceController.h
		include/WorkspaceDock.h
		include/ModelCatalog.h
		include/ModelResultsDialog.h
		include/SettingsDialog.h
		include/ALiSPlugin.h
		src/GroundFilter.cpp
		src/FeatureEngine.cpp
		src/SessionModel.cpp
		src/TerrainEngine.cpp
		src/WorkspaceController.cpp
		src/WorkspaceDock.cpp
		src/ModelCatalog.cpp
		src/ModelResultsDialog.cpp
		src/SettingsDialog.cpp
		src/ALiSPlugin.cpp
	)
	target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
	# AddPlugin() uses CMake's plain target_link_libraries signature for this
	# target, therefore every later call for the plugin must use it as well.
	target_link_libraries(${PROJECT_NAME}
		ALiSAuditLogic
		ALiSCSFCore
		QCC_DB_LIB
		${ALIS_QT_WIDGETS}
	)
	target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_14)
	target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/worker/"
		DESTINATION "${CLOUDCOMPARE_DEST_FOLDER}/worker/ALiS"
		PATTERN "__pycache__" EXCLUDE
		PATTERN "*.pyc" EXCLUDE
		PATTERN "tests" EXCLUDE)

	if(ALIS_BUILD_TESTS)
		add_executable(ALiS_processing_tests
			tests/test_processing.cpp tests/benchmark_ground_review.cpp
			include/WorkspaceController.h include/WorkspaceDock.h include/SessionModel.h
			include/AnnotationStudio.h include/AnnotationCanvas.h
			src/WorkspaceController.cpp src/WorkspaceDock.cpp src/WorkspaceProcessing.cpp src/WorkspaceVegetationModel.cpp src/WorkspaceFeatureRanges.cpp src/ModelCatalog.cpp
			include/ModelResultsDialog.h src/ModelResultsDialog.cpp
			src/ProcessingPreset.cpp src/CloudProfile.cpp src/SessionModel.cpp
			src/FeatureEngine.cpp src/GroundFilter.cpp src/TerrainEngine.cpp
			src/AnnotationStudio.cpp src/AnnotationCanvas.cpp)
		target_include_directories(ALiS_processing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_processing_tests PRIVATE ALiSAuditLogic ALiSCSFCore CCPluginAPI QCC_IO_LIB QCC_DB_LIB ${ALIS_QT_WIDGETS})
		target_compile_features(ALiS_processing_tests PRIVATE cxx_std_14)
		target_compile_options(ALiS_processing_tests PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
		if(BUILD_TESTING)
			add_test(NAME ALiS.processing COMMAND ALiS_processing_tests)
			set_tests_properties(ALiS.processing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
		endif()
		add_executable(ALiS_export_features tests/export_classifier_features.cpp src/FeatureEngine.cpp src/GroundFilter.cpp src/TerrainEngine.cpp)
		add_executable(ALiS_classifier_roundtrip tests/classifier_roundtrip.cpp src/SessionModel.cpp include/SessionModel.h)
		target_include_directories(ALiS_classifier_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_classifier_roundtrip PRIVATE ALiSAuditLogic CCPluginAPI QCC_IO_LIB QCC_DB_LIB ${ALIS_QT_WIDGETS})
		target_compile_features(ALiS_classifier_roundtrip PRIVATE cxx_std_14)
		target_include_directories(ALiS_export_features PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_export_features PRIVATE ALiSAuditLogic ALiSCSFCore QCC_DB_LIB ${ALIS_QT_WIDGETS})
		target_compile_features(ALiS_export_features PRIVATE cxx_std_14)
		add_executable(ALiS_persistence_tests tests/test_persistence.cpp src/SessionModel.cpp include/SessionModel.h)
		target_include_directories(ALiS_persistence_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_persistence_tests PRIVATE ALiSAuditLogic CCPluginAPI QCC_IO_LIB QCC_DB_LIB ${ALIS_QT_WIDGETS})
		target_compile_features(ALiS_persistence_tests PRIVATE cxx_std_14)
		if(BUILD_TESTING)
			add_test(NAME ALiS.persistence COMMAND ALiS_persistence_tests)
			set_tests_properties(ALiS.persistence PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
		endif()
		add_executable(ALiS_annotation_tests tests/test_annotation_studio.cpp src/AnnotationStudio.cpp include/AnnotationStudio.h src/AnnotationCanvas.cpp include/AnnotationCanvas.h)
		target_include_directories(ALiS_annotation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_annotation_tests PRIVATE ALiSAuditLogic QCC_DB_LIB ${ALIS_QT_WIDGETS})
		target_compile_features(ALiS_annotation_tests PRIVATE cxx_std_14)
		target_compile_options(ALiS_annotation_tests PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
		if(BUILD_TESTING)
			add_test(NAME ALiS.annotation COMMAND ALiS_annotation_tests)
			set_tests_properties(ALiS.annotation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
		endif()
		add_executable(ALiS_logic_tests tests/test_audit_logic.cpp)
		target_link_libraries(ALiS_logic_tests PRIVATE ALiSAuditLogic)
		target_compile_features(ALiS_logic_tests PRIVATE cxx_std_14)
		if(BUILD_TESTING)
			add_test(NAME ALiS.logic COMMAND ALiS_logic_tests)
		endif()
		add_executable(ALiS_workspace_logic_tests tests/test_workspace_logic.cpp)
		target_link_libraries(ALiS_workspace_logic_tests PRIVATE ALiSAuditLogic)
		target_compile_features(ALiS_workspace_logic_tests PRIVATE cxx_std_14)
		add_executable(ALiS_workspace_benchmark tests/benchmark_workspace_logic.cpp)
		target_link_libraries(ALiS_workspace_benchmark PRIVATE ALiSAuditLogic)
		target_compile_features(ALiS_workspace_benchmark PRIVATE cxx_std_14)
		if(BUILD_TESTING)
			add_test(NAME ALiS.workspace_logic COMMAND ALiS_workspace_logic_tests)
		endif()
		add_executable(ALiS_session_tests tests/test_session_model.cpp src/SessionModel.cpp include/SessionModel.h)
		target_include_directories(ALiS_session_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_session_tests PRIVATE ALiSAuditLogic QCC_DB_LIB ${ALIS_QT_CORE})
		target_compile_features(ALiS_session_tests PRIVATE cxx_std_14)
		if(BUILD_TESTING)
			add_test(NAME ALiS.session COMMAND ALiS_session_tests)
		endif()
		add_executable(ALiS_ground_filter_tests
			tests/test_ground_filter.cpp
			include/GroundFilter.h
			src/GroundFilter.cpp
		)
		target_include_directories(ALiS_ground_filter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_ground_filter_tests PRIVATE
			ALiSCSFCore
			QCC_DB_LIB
			${ALIS_QT_CORE}
			${ALIS_QT_WIDGETS}
		)
		target_compile_features(ALiS_ground_filter_tests PRIVATE cxx_std_14)
		if(BUILD_TESTING)
			add_test(NAME ALiS.ground_filters COMMAND ALiS_ground_filter_tests)
		endif()

		# Explicit, long-running real-tile runner. It consumes the deterministic
		# .qalf fixture produced by tools/prepare_e2e_fixture.py and is not added to
		# the default CTest set unless ALIS_E2E_FIXTURE is supplied.
		add_executable(ALiS_real_tile_e2e
			tests/real_tile_e2e.cpp
			include/FeatureEngine.h
			include/GroundFilter.h
			include/TerrainEngine.h
			src/FeatureEngine.cpp
			src/GroundFilter.cpp
			src/TerrainEngine.cpp
		)
		target_include_directories(ALiS_real_tile_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
		target_link_libraries(ALiS_real_tile_e2e PRIVATE
			ALiSAuditLogic
			ALiSCSFCore
			QCC_DB_LIB
			${ALIS_QT_CORE}
			${ALIS_QT_WIDGETS}
		)
		target_compile_features(ALiS_real_tile_e2e PRIVATE cxx_std_14)
		set(ALIS_E2E_FIXTURE "" CACHE FILEPATH
			"Optional prepared .qalf fixture registered as the long-running real-tile CTest")
		set(ALIS_E2E_OUTPUT_DIR "${CMAKE_BINARY_DIR}/ALiS-real-tile-results" CACHE PATH
			"Output directory used by the optional real-tile CTest")
		if(BUILD_TESTING AND ALIS_E2E_FIXTURE)
			if(NOT EXISTS "${ALIS_E2E_FIXTURE}")
				message(FATAL_ERROR "ALIS_E2E_FIXTURE does not exist: ${ALIS_E2E_FIXTURE}")
			endif()
			add_test(NAME ALiS.real_tile_e2e
				COMMAND ALiS_real_tile_e2e
					--fixture "${ALIS_E2E_FIXTURE}"
					--output-dir "${ALIS_E2E_OUTPUT_DIR}")
			set_tests_properties(ALiS.real_tile_e2e PROPERTIES
				LABELS "long;real-data;e2e"
				TIMEOUT 3600
				ENVIRONMENT "QT_QPA_PLATFORM=minimal")
		endif()
	endif()
endif()

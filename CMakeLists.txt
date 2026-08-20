cmake_minimum_required(VERSION 3.24)

# Public release/version string. This is the single authoritative version used
# by About, CMake's numeric project metadata, Windows file metadata and the
# automated GitHub release tooling.
set(VFXPHOTOLAB_RELEASE_VERSION "0.14.0m.2")
string(REGEX MATCH "^([0-9]+)[.]([0-9]+)[.]([0-9]+)"
       VFXPHOTOLAB_NUMERIC_VERSION_MATCH "${VFXPHOTOLAB_RELEASE_VERSION}")
if(NOT VFXPHOTOLAB_NUMERIC_VERSION_MATCH)
    message(FATAL_ERROR
        "VFXPHOTOLAB_RELEASE_VERSION must begin with a numeric major.minor.patch version")
endif()
set(VFXPHOTOLAB_CMAKE_VERSION
    "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

project(VFXPhotoLab
    VERSION "${VFXPHOTOLAB_CMAKE_VERSION}"
    DESCRIPTION "A fast, non-destructive photo editor for Linux and Windows"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VFXPHOTOLAB_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer for diagnostic builds"
       OFF)
if(VFXPHOTOLAB_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined
                            -fno-omit-frame-pointer
                            -fno-sanitize-recover=all)
        add_link_options(-fsanitize=address,undefined
                         -fno-omit-frame-pointer
                         -fno-sanitize-recover=all)
        add_compile_definitions(_GLIBCXX_ASSERTIONS)
        message(STATUS "VFX Photo Lab: ASan/UBSan diagnostic instrumentation enabled")
    else()
        message(FATAL_ERROR
            "VFXPHOTOLAB_ENABLE_SANITIZERS requires a GCC- or Clang-compatible compiler")
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Keep the checked-in WGSL file as the single authoritative adjustment shader.
# CMake embeds that exact source into the core library at configure time so the
# runtime and the standalone WGSL validator cannot drift apart.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/adjustment_tile.wgsl"
     VFXPHOTOLAB_ADJUSTMENT_SHADER_SOURCE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(WIN32)
    string(REGEX MATCHALL "[0-9]+" VFXPHOTOLAB_WINDOWS_VERSION_PARTS
           "${VFXPHOTOLAB_RELEASE_VERSION}")
    list(LENGTH VFXPHOTOLAB_WINDOWS_VERSION_PARTS _vfx_version_part_count)
    while(_vfx_version_part_count LESS 4)
        list(APPEND VFXPHOTOLAB_WINDOWS_VERSION_PARTS 0)
        list(LENGTH VFXPHOTOLAB_WINDOWS_VERSION_PARTS _vfx_version_part_count)
    endwhile()
    list(GET VFXPHOTOLAB_WINDOWS_VERSION_PARTS 0 _vfx_version_0)
    list(GET VFXPHOTOLAB_WINDOWS_VERSION_PARTS 1 _vfx_version_1)
    list(GET VFXPHOTOLAB_WINDOWS_VERSION_PARTS 2 _vfx_version_2)
    list(GET VFXPHOTOLAB_WINDOWS_VERSION_PARTS 3 _vfx_version_3)
    set(VFXPHOTOLAB_WINDOWS_FILE_VERSION
        "${_vfx_version_0},${_vfx_version_1},${_vfx_version_2},${_vfx_version_3}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/VFXPhotoLab.ico"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/VFXPhotoLab.ico"
        COPYONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/VFXPhotoLab.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/VFXPhotoLab.rc"
        @ONLY)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/AdjustmentShaderSource.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/AdjustmentShaderSource.h"
    @ONLY)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/display_colour_transform.wgsl"
     VFXPHOTOLAB_DISPLAY_COLOUR_SHADER_SOURCE)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/DisplayColourShaderSource.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/DisplayColourShaderSource.h"
    @ONLY)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vector_feather.wgsl"
     VFXPHOTOLAB_VECTOR_FEATHER_SHADER_SOURCE)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/VectorFeatherShaderSource.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/VectorFeatherShaderSource.h"
    @ONLY)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()

option(VFXPHOTOLAB_ENABLE_OCIO
       "Enable the OpenColorIO 2.5 colour-management foundation when available"
       ON)
set(VFXPHOTOLAB_OCIO_DEFAULT_ROOT "${CMAKE_SOURCE_DIR}/third_party/opencolorio")
if(DEFINED ENV{OCIO_ROOT} AND NOT "$ENV{OCIO_ROOT}" STREQUAL "")
    set(VFXPHOTOLAB_OCIO_DEFAULT_ROOT "$ENV{OCIO_ROOT}")
endif()
set(VFXPHOTOLAB_OCIO_ROOT "${VFXPHOTOLAB_OCIO_DEFAULT_ROOT}"
    CACHE PATH "Root of an OpenColorIO 2.5.2 installation")
set(VFXPHOTOLAB_OCIO_DEPENDENCY_ROOT
    "${CMAKE_SOURCE_DIR}/build/deps/opencolorio-2.5.2/ext/dist"
    CACHE PATH "Prefix containing dependencies of the locally built static OpenColorIO package")

if(VFXPHOTOLAB_ENABLE_OCIO)
    # A static OpenColorIO package resolves expat, Imath, pystring, yaml-cpp,
    # ZLIB and minizip-ng while its Config file is being loaded.  The local
    # bootstrap installs those packages into OCIO's private ext/dist prefix,
    # so make both prefixes visible to find_dependency rather than only using
    # the OpenColorIO Config directory as a one-shot hint.
    foreach(_vfx_ocio_prefix
            "${VFXPHOTOLAB_OCIO_ROOT}"
            "${VFXPHOTOLAB_OCIO_DEPENDENCY_ROOT}"
            "$ENV{OCIO_ROOT}")
        if(NOT "${_vfx_ocio_prefix}" STREQUAL "" AND EXISTS "${_vfx_ocio_prefix}")
            list(PREPEND CMAKE_PREFIX_PATH "${_vfx_ocio_prefix}")
        endif()
    endforeach()

    find_package(OpenColorIO 2.5.2 CONFIG QUIET
        HINTS
            "${VFXPHOTOLAB_OCIO_ROOT}"
            "${VFXPHOTOLAB_OCIO_ROOT}/lib/cmake/OpenColorIO"
            "${VFXPHOTOLAB_OCIO_ROOT}/lib64/cmake/OpenColorIO"
            "${VFXPHOTOLAB_OCIO_DEPENDENCY_ROOT}"
            "$ENV{OCIO_ROOT}"
            "$ENV{OCIO_ROOT}/lib/cmake/OpenColorIO"
            "$ENV{OCIO_ROOT}/lib64/cmake/OpenColorIO")
endif()

option(VFXPHOTOLAB_ENABLE_WEBGPU
       "Link the pinned wgpu-native SDK and enable the native WebGPU foundation"
       ON)
set(VFXPHOTOLAB_WGPU_DEFAULT_ROOT "${CMAKE_SOURCE_DIR}/third_party/wgpu-native")
if(DEFINED ENV{WGPU_ROOT} AND NOT "$ENV{WGPU_ROOT}" STREQUAL "")
    set(VFXPHOTOLAB_WGPU_DEFAULT_ROOT "$ENV{WGPU_ROOT}")
endif()
set(VFXPHOTOLAB_WGPU_ROOT "${VFXPHOTOLAB_WGPU_DEFAULT_ROOT}"
    CACHE PATH "Root of a wgpu-native release containing include/ and lib/")

if(VFXPHOTOLAB_ENABLE_WEBGPU)
    find_path(VFXPHOTOLAB_WGPU_INCLUDE_DIR
        NAMES webgpu/webgpu.h webgpu-headers/webgpu.h
        HINTS
            "${VFXPHOTOLAB_WGPU_ROOT}/include"
            "$ENV{WGPU_ROOT}/include"
    )
    find_library(VFXPHOTOLAB_WGPU_LIBRARY
        NAMES wgpu_native wgpu
        HINTS
            "${VFXPHOTOLAB_WGPU_ROOT}/lib"
            "${VFXPHOTOLAB_WGPU_ROOT}/lib64"
            "$ENV{WGPU_ROOT}/lib"
            "$ENV{WGPU_ROOT}/lib64"
    )
endif()

add_library(vfxphotolab_core STATIC
    src/ColourManagement.cpp
    src/ColourManagement.h
    src/ColourResourceAudit.cpp
    src/ColourResourceAudit.h
    src/ColourConversion.cpp
    src/ColourConversion.h
    src/OcioIntegration.cpp
    src/OcioIntegration.h
    src/DisplayColourManagement.cpp
    src/DisplayColourManagement.h
    src/ImageProfileImport.cpp
    src/ImageProfileImport.h
    src/ImageExport.cpp
    src/ImageExport.h
    src/ExportNamingTemplate.cpp
    src/ExportNamingTemplate.h
    src/ExportProfileStore.cpp
    src/ExportProfileStore.h
    src/ProductionExport.cpp
    src/ProductionExport.h
    src/ExportQueueCore.cpp
    src/ExportQueueCore.h
    src/ExportQueuePersistence.cpp
    src/ExportQueuePersistence.h
    src/ExportQueueController.cpp
    src/ExportQueueController.h
    src/BlueNoise64.h
    src/Adjustment.cpp
    src/Adjustment.h
    src/SmartLayerFoundation.cpp
    src/SmartLayerFoundation.h
    src/SmartLayerTileCache.cpp
    src/SmartLayerTileCache.h
    src/SpatialFilter.cpp
    src/SpatialFilter.h
    src/PresetCore.cpp
    src/PresetCore.h
    src/AdjustmentPresetStore.cpp
    src/AdjustmentPresetStore.h
    src/VectorAppearancePresetStore.cpp
    src/VectorAppearancePresetStore.h
    src/TextLayer.cpp
    src/TextLayer.h
    src/TextRasterizer.cpp
    src/TextRasterizer.h
    src/VectorLayer.cpp
    src/VectorLayer.h
    src/VectorRasterizer.cpp
    src/VectorRasterizer.h
    src/SvgWorkflow.cpp
    src/SvgWorkflow.h
    src/CubeLut.cpp
    src/CubeLut.h
    src/CanvasSizeOperations.cpp
    src/CanvasSizeOperations.h
    src/ImageSizeOperations.cpp
    src/ImageSizeOperations.h
    src/TrimOperations.cpp
    src/TrimOperations.h
    src/TransformInterpolation.h
    src/TransformSampling.cpp
    src/TransformSampling.h
    src/TransformSafety.cpp
    src/TransformSafety.h
    src/ClipboardOperations.cpp
    src/CropOperations.cpp
    src/ClipboardOperations.h
    src/CropOperations.h
    src/CropTypes.h
    src/CloneStamp.cpp
    src/CloneStamp.h
    src/HealingBrush.cpp
    src/HealingBrush.h
    src/FillOperations.cpp
    src/FillOperations.h
    src/GradientOperations.cpp
    src/GradientOperations.h
    src/LayerMergeOperations.cpp
    src/LayerMergeOperations.h
    src/HistogramService.cpp
    src/HistogramService.h
    src/TonalMapping.cpp
    src/TonalMapping.h
    src/SpotHealing.cpp
    src/SpotHealing.h
    src/PatchTool.cpp
    src/PatchTool.h
    src/ToneBrush.cpp
    src/ToneBrush.h
    src/SmudgeBrush.cpp
    src/SmudgeBrush.h
    src/DocumentSession.cpp
    src/DocumentSession.h
    src/DocumentTransformOperations.cpp
    src/DocumentTransformOperations.h
    src/ImageProcessor.cpp
    src/ImageProcessor.h
    src/ManagedAdjustmentGpuLut.cpp
    src/ManagedAdjustmentGpuLut.h
    src/gpu/ProgressivePreview.cpp
    src/gpu/ProgressivePreview.h
    src/gpu/RenderBackend.cpp
    src/gpu/RenderBackend.h
    src/gpu/TileCache.cpp
    src/gpu/TileCache.h
    src/gpu/TiledCanvasEngine.cpp
    src/gpu/TiledCanvasEngine.h
    src/gpu/WebGpuContext.cpp
    src/gpu/WebGpuContext.h
    src/PhotoDocument.cpp
    src/PhotoDocument.h
    src/RasterHistory.cpp
    src/RasterHistory.h
    src/SessionCache.cpp
    src/SessionCache.h
    src/SelectionHistory.cpp
    src/SelectionHistory.h
    src/SelectionLocalEditing.cpp
    src/SelectionLocalEditing.h
    src/SelectionTransformOperations.cpp
    src/SelectionTransformOperations.h
    src/SelectionMask.cpp
    src/SelectionMask.h
    src/SelectionOperations.cpp
    src/SelectionOperations.h
    src/ShortcutUtils.cpp
    src/ShortcutUtils.h
    src/TgaCodec.cpp
    src/TgaCodec.h
)

target_link_libraries(vfxphotolab_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Concurrent
)
if(WIN32)
    target_link_libraries(vfxphotolab_core PUBLIC Mscms)
endif()

target_include_directories(vfxphotolab_core
    PUBLIC src
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(vfxphotolab_core PUBLIC VFXPHOTOLAB_VERSION="${VFXPHOTOLAB_RELEASE_VERSION}")

if(VFXPHOTOLAB_ENABLE_OCIO)
    if(TARGET OpenColorIO::OpenColorIO)
        target_link_libraries(vfxphotolab_core PUBLIC OpenColorIO::OpenColorIO)
        target_compile_definitions(vfxphotolab_core PUBLIC VFXPHOTOLAB_HAS_OCIO=1)
        message(STATUS "VFX Photo Lab: OpenColorIO ${OpenColorIO_VERSION} found; ACES/config inspection and CPU colour conversion enabled")
    elseif(TARGET OpenColorIO)
        target_link_libraries(vfxphotolab_core PUBLIC OpenColorIO)
        target_compile_definitions(vfxphotolab_core PUBLIC VFXPHOTOLAB_HAS_OCIO=1)
        message(STATUS "VFX Photo Lab: OpenColorIO found; ACES/config inspection and CPU colour conversion enabled")
    else()
        message(STATUS "VFX Photo Lab: OpenColorIO 2.5.2 not found; ICC-only fallback active. Run scripts/fetch-opencolorio.sh or .ps1")
    endif()
else()
    message(STATUS "VFX Photo Lab: OpenColorIO explicitly disabled; ICC-only fallback active")
endif()

if(VFXPHOTOLAB_ENABLE_WEBGPU AND VFXPHOTOLAB_WGPU_INCLUDE_DIR AND VFXPHOTOLAB_WGPU_LIBRARY)
    target_include_directories(vfxphotolab_core PRIVATE "${VFXPHOTOLAB_WGPU_INCLUDE_DIR}")
    target_link_libraries(vfxphotolab_core PRIVATE "${VFXPHOTOLAB_WGPU_LIBRARY}")
    if(UNIX AND NOT APPLE)
        target_link_libraries(vfxphotolab_core PRIVATE ${CMAKE_DL_LIBS} m)
    endif()
    target_compile_definitions(vfxphotolab_core PRIVATE VFXPHOTOLAB_HAS_WEBGPU=1)
    message(STATUS "VFX Photo Lab: wgpu-native SDK found; adapter/device/queue, tiled GPU brush/Clone Stamp/Fill/Gradient/vector Feather/compositor/Image Size resampling, WGSL adjustments, parity-gated display and managed adjustment-domain colour transforms, and progressive canvas path enabled")

    get_filename_component(VFXPHOTOLAB_WGPU_LIBRARY_DIR "${VFXPHOTOLAB_WGPU_LIBRARY}" DIRECTORY)
    if(UNIX AND NOT APPLE)
        set_target_properties(vfxphotolab_core PROPERTIES BUILD_RPATH "${VFXPHOTOLAB_WGPU_LIBRARY_DIR}")
    endif()
elseif(VFXPHOTOLAB_ENABLE_WEBGPU)
    message(STATUS "VFX Photo Lab: wgpu-native SDK not found; CPU renderer active. Run scripts/fetch-wgpu-native.sh or .ps1")
else()
    message(STATUS "VFX Photo Lab: native WebGPU explicitly disabled; CPU renderer active")
endif()

qt_add_executable(VFXPhotoLab
    WIN32
    src/main.cpp
    src/AppStyle.cpp
    src/AppStyle.h
    src/CanvasSizeDialog.cpp
    src/CanvasSizeDialog.h
    src/ImageSizeDialog.cpp
    src/ImageSizeDialog.h
    src/ImageExportDialog.cpp
    src/ImageExportDialog.h
    src/ProductionExportDialog.cpp
    src/ProductionExportDialog.h
    src/ExportQueueDock.cpp
    src/ExportQueueDock.h
    src/TrimDialogs.cpp
    src/TrimDialogs.h
    src/ColourWheelWidget.cpp
    src/ColourWheelWidget.h
    src/DocumentStripWidget.cpp
    src/DocumentStripWidget.h
    src/ImageCanvas.cpp
    src/ImageCanvas.h
    src/LayerTreeWidget.cpp
    src/LayerTreeWidget.h
    src/LevelsEditorWidget.cpp
    src/LevelsEditorWidget.h
    src/CurvesEditorWidget.cpp
    src/CurvesEditorWidget.h
    src/GradientMapEditorWidget.cpp
    src/GradientMapEditorWidget.h
    src/SliderSpinBox.cpp
    src/SliderSpinBox.h
    src/PresetManagerDialog.cpp
    src/PresetManagerDialog.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/ToolBarActionLifecycle.h
    src/NewDocumentDialog.cpp
    src/NewDocumentDialog.h
    resources/resources.qrc
)
if(WIN32)
    target_sources(VFXPhotoLab PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/VFXPhotoLab.rc")
endif()

target_link_libraries(VFXPhotoLab
    PRIVATE
        vfxphotolab_core
        Qt6::Widgets
        Qt6::Concurrent
)

if(VFXPHOTOLAB_ENABLE_WEBGPU AND VFXPHOTOLAB_WGPU_INCLUDE_DIR AND VFXPHOTOLAB_WGPU_LIBRARY)
    if(WIN32)
        find_file(VFXPHOTOLAB_WGPU_RUNTIME
            NAMES wgpu_native.dll wgpu.dll
            HINTS "${VFXPHOTOLAB_WGPU_ROOT}/bin" "${VFXPHOTOLAB_WGPU_ROOT}/lib"
                  "$ENV{WGPU_ROOT}/bin" "$ENV{WGPU_ROOT}/lib")
        if(VFXPHOTOLAB_WGPU_RUNTIME)
            target_compile_definitions(VFXPhotoLab PRIVATE VFXPHOTOLAB_PACKAGE_HAS_WEBGPU=1)
            add_custom_command(TARGET VFXPhotoLab POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${VFXPHOTOLAB_WGPU_RUNTIME}" "$<TARGET_FILE_DIR:VFXPhotoLab>")
            install(FILES "${VFXPHOTOLAB_WGPU_RUNTIME}" DESTINATION .)
        endif()
    elseif(UNIX AND NOT APPLE)
        set_target_properties(VFXPhotoLab PROPERTIES BUILD_RPATH "${VFXPHOTOLAB_WGPU_LIBRARY_DIR}")
        if(VFXPHOTOLAB_WGPU_LIBRARY MATCHES "\\.so")
            add_custom_command(TARGET VFXPhotoLab POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${VFXPHOTOLAB_WGPU_LIBRARY}" "$<TARGET_FILE_DIR:VFXPhotoLab>")
            set_target_properties(VFXPhotoLab PROPERTIES BUILD_RPATH "\$ORIGIN;${VFXPHOTOLAB_WGPU_LIBRARY_DIR}")
            install(FILES "${VFXPHOTOLAB_WGPU_LIBRARY}" DESTINATION bin)
        endif()
    endif()
endif()

if(MSVC)
    target_compile_options(vfxphotolab_core PRIVATE /W4 /permissive-)
    target_compile_options(VFXPhotoLab PRIVATE /W4 /permissive-)
else()
    target_compile_options(vfxphotolab_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(VFXPhotoLab PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # Keep the Windows portable/install tree flat around the executable so the
    # same staged directory can be zipped directly or consumed by Inno Setup.
    install(TARGETS VFXPhotoLab
        RUNTIME DESTINATION .
    )
    install(DIRECTORY shaders/ DESTINATION shaders
            FILES_MATCHING PATTERN "*.wgsl")
    install(FILES LICENSE README.md CHANGELOG.md DESTINATION .)
else()
    install(TARGETS VFXPhotoLab
        BUNDLE DESTINATION .
        RUNTIME DESTINATION bin
    )
    install(DIRECTORY shaders/ DESTINATION share/vfxphotolab/shaders
            FILES_MATCHING PATTERN "*.wgsl")
endif()

if(COMMAND qt_generate_deploy_app_script)
    qt_generate_deploy_app_script(
        TARGET VFXPhotoLab
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(VFXPhotoLabTests
        tests/test_core.cpp
    )
    target_link_libraries(VFXPhotoLabTests PRIVATE vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabTests COMMAND VFXPhotoLabTests)

    qt_add_executable(VFXPhotoLabColourManagementTests
        tests/test_colour_management.cpp
    )
    target_link_libraries(VFXPhotoLabColourManagementTests
        PRIVATE vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabColourManagementTests
             COMMAND VFXPhotoLabColourManagementTests)

    qt_add_executable(VFXPhotoLabLutConformanceTests
        tests/test_lut_conformance.cpp
    )
    target_link_libraries(VFXPhotoLabLutConformanceTests PRIVATE
        vfxphotolab_core Qt6::Test)
    target_compile_definitions(VFXPhotoLabLutConformanceTests PRIVATE
        VFXPHOTOLAB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME VFXPhotoLabLutConformanceTests
             COMMAND VFXPhotoLabLutConformanceTests)

    qt_add_executable(VFXPhotoLabSpatialFilterTests
        tests/test_spatial_filters.cpp
    )
    target_link_libraries(VFXPhotoLabSpatialFilterTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabSpatialFilterTests
             COMMAND VFXPhotoLabSpatialFilterTests)

    qt_add_executable(VFXPhotoLabColourAdjustmentEssentialsTests
        tests/test_colour_adjustment_essentials.cpp
    )
    target_link_libraries(VFXPhotoLabColourAdjustmentEssentialsTests PRIVATE
        vfxphotolab_core Qt6::Test)
    target_compile_definitions(VFXPhotoLabColourAdjustmentEssentialsTests PRIVATE
        VFXPHOTOLAB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME VFXPhotoLabColourAdjustmentEssentialsTests
             COMMAND VFXPhotoLabColourAdjustmentEssentialsTests)

    qt_add_executable(VFXPhotoLabAdvancedColourControlsTests
        tests/test_advanced_colour_controls.cpp
    )
    target_link_libraries(VFXPhotoLabAdvancedColourControlsTests PRIVATE
        vfxphotolab_core Qt6::Test)
    target_compile_definitions(VFXPhotoLabAdvancedColourControlsTests PRIVATE
        VFXPHOTOLAB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME VFXPhotoLabAdvancedColourControlsTests
             COMMAND VFXPhotoLabAdvancedColourControlsTests)

    qt_add_executable(VFXPhotoLabOpticalSpatialColourEffectsTests
        tests/test_optical_spatial_colour_effects.cpp
    )
    target_link_libraries(VFXPhotoLabOpticalSpatialColourEffectsTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabOpticalSpatialColourEffectsTests
             COMMAND VFXPhotoLabOpticalSpatialColourEffectsTests)

    qt_add_executable(VFXPhotoLabAdditionalSpatialFiltersTests
        tests/test_additional_spatial_filters.cpp
    )
    target_link_libraries(VFXPhotoLabAdditionalSpatialFiltersTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabAdditionalSpatialFiltersTests
             COMMAND VFXPhotoLabAdditionalSpatialFiltersTests)

    qt_add_executable(VFXPhotoLabPresetTests
        tests/test_presets.cpp
    )
    target_link_libraries(VFXPhotoLabPresetTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabPresetTests COMMAND VFXPhotoLabPresetTests)


    qt_add_executable(VFXPhotoLabExportProfileTests
        tests/test_export_profiles.cpp
    )
    target_link_libraries(VFXPhotoLabExportProfileTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabExportProfileTests
             COMMAND VFXPhotoLabExportProfileTests)


    qt_add_executable(VFXPhotoLabProductionExportTests
        tests/test_production_export.cpp
    )
    target_link_libraries(VFXPhotoLabProductionExportTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabProductionExportTests
             COMMAND VFXPhotoLabProductionExportTests)

    qt_add_executable(VFXPhotoLabExportQueueTests
        tests/test_export_queue.cpp
    )
    target_link_libraries(VFXPhotoLabExportQueueTests PRIVATE
        vfxphotolab_core Qt6::Test)
    add_test(NAME VFXPhotoLabExportQueueTests
             COMMAND VFXPhotoLabExportQueueTests)

    qt_add_executable(VFXPhotoLabSvgTests
        tests/test_svg.cpp
    )
    target_link_libraries(VFXPhotoLabSvgTests PRIVATE
        vfxphotolab_core Qt6::Gui Qt6::Widgets Qt6::Test)
    add_test(NAME VFXPhotoLabSvgTests COMMAND VFXPhotoLabSvgTests)
    set_tests_properties(VFXPhotoLabSvgTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(VFXPhotoLabTextTests
        tests/test_text.cpp
    )
    target_link_libraries(VFXPhotoLabTextTests PRIVATE
        vfxphotolab_core Qt6::Gui Qt6::Widgets Qt6::Test)
    add_test(NAME VFXPhotoLabTextTests COMMAND VFXPhotoLabTextTests)
    set_tests_properties(VFXPhotoLabTextTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(VFXPhotoLabCanvasTests
        tests/test_canvas.cpp
        src/DocumentStripWidget.cpp
        src/DocumentStripWidget.h
        src/ImageCanvas.cpp
        src/ImageCanvas.h
        src/LayerTreeWidget.cpp
        src/LayerTreeWidget.h
        src/GradientMapEditorWidget.cpp
        src/GradientMapEditorWidget.h
        src/SliderSpinBox.cpp
        src/SliderSpinBox.h
        src/Adjustment.cpp
        src/Adjustment.h
        src/TransformSafety.cpp
        src/TransformSafety.h
        src/ShortcutUtils.cpp
        src/ShortcutUtils.h
        src/ToolBarActionLifecycle.h
    )
    target_link_libraries(VFXPhotoLabCanvasTests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    target_include_directories(VFXPhotoLabCanvasTests PRIVATE src)
    add_test(NAME VFXPhotoLabCanvasTests COMMAND VFXPhotoLabCanvasTests)
    set_tests_properties(VFXPhotoLabCanvasTests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(UNIX AND NOT APPLE)
    install(FILES packaging/io.github.MattyGWS.VFXPhotoLab.desktop
        DESTINATION share/applications)
    install(FILES packaging/io.github.MattyGWS.VFXPhotoLab.metainfo.xml
        DESTINATION share/metainfo)
    install(FILES resources/app-icon.png
        DESTINATION share/icons/hicolor/128x128/apps
        RENAME io.github.MattyGWS.VFXPhotoLab.png)
endif()

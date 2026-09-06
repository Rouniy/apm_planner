cmake_minimum_required(VERSION 3.21)
project(APMPlanner3 VERSION 3.0.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(APM_PLANNER_TARGET apmplanner3)
set(APM_INSTALL_RESOURCE_SUBDIR "${CMAKE_INSTALL_DATADIR}/APMPlanner3")
set(APM_INSTALL_RESOURCE_DIR "${CMAKE_INSTALL_FULL_DATADIR}/APMPlanner3")
option(APM_ENABLE_SOURCE_DATA_FALLBACK
    "Allow an uninstalled developer build to load resources from the source tree" ON)

# C++ Standard & Qt features
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep Qt 5 as the reproducible baseline while the remaining legacy APIs are
# removed. CI and developers can select Qt 6 explicitly with APM_QT_MAJOR=6.
set(APM_QT_MAJOR "5" CACHE STRING "Qt major version to use: AUTO, 5, or 6")
set_property(CACHE APM_QT_MAJOR PROPERTY STRINGS AUTO 5 6)
if(APM_QT_MAJOR STREQUAL "5")
    set(_apm_qt_names Qt5)
elseif(APM_QT_MAJOR STREQUAL "6")
    set(_apm_qt_names Qt6)
elseif(APM_QT_MAJOR STREQUAL "AUTO")
    # Prefer the known-green baseline until the explicit Qt 6 build is green.
    set(_apm_qt_names Qt5 Qt6)
else()
    message(FATAL_ERROR "APM_QT_MAJOR must be AUTO, 5, or 6")
endif()

find_package(QT NAMES ${_apm_qt_names} REQUIRED COMPONENTS Core)
if(QT_VERSION_MAJOR EQUAL 5 AND QT_VERSION VERSION_LESS 5.10)
    message(FATAL_ERROR
        "APM Planner requires Qt 5.10 or newer (found ${QT_VERSION})")
endif()
set(APM_QT_PACKAGE Qt${QT_VERSION_MAJOR})
set(_apm_qt_required_components
    Core Gui Widgets Network Svg Xml Sql SerialPort Quick PrintSupport Qml QuickWidgets Concurrent
)
if(BUILD_TESTING)
    list(APPEND _apm_qt_required_components Test)
endif()
if(QT_VERSION_MAJOR EQUAL 6)
    list(APPEND _apm_qt_required_components OpenGL OpenGLWidgets SvgWidgets)
endif()
find_package(${APM_QT_PACKAGE} REQUIRED COMPONENTS ${_apm_qt_required_components})
message(STATUS "APM Planner Qt major: ${QT_VERSION_MAJOR}")

# Cross-platform audio is implemented only through Qt. Silent developer builds
# must be an explicit opt-out, never mistaken for verified speech support.
option(APM_REQUIRE_QT_AUDIO "Fail configure when Qt Multimedia or TextToSpeech is unavailable" ON)
find_package(${APM_QT_PACKAGE} QUIET COMPONENTS Multimedia)
find_package(${APM_QT_PACKAGE} QUIET COMPONENTS TextToSpeech)
set(APM_QT_AUDIO_LIBRARIES "")
if(TARGET Qt::Multimedia)
    list(APPEND APM_QT_AUDIO_LIBRARIES Qt::Multimedia)
    add_compile_definitions(APM_HAS_QT_MULTIMEDIA)
    message(STATUS "Audio playback: Qt Multimedia enabled")
else()
    message(STATUS "Audio playback: Qt Multimedia not found; silent fallback enabled")
endif()
if(TARGET Qt::TextToSpeech)
    list(APPEND APM_QT_AUDIO_LIBRARIES Qt::TextToSpeech)
    add_compile_definitions(APM_HAS_QT_TEXT_TO_SPEECH)
    message(STATUS "Speech synthesis: Qt TextToSpeech enabled")
else()
    message(WARNING "Speech synthesis disabled: Qt TextToSpeech development package was not found. Test Speech and spoken alerts will not work. Install the Qt speech development package and a runtime speech plugin, or set APM_REQUIRE_QT_AUDIO=ON to reject this reduced build.")
endif()
if(APM_REQUIRE_QT_AUDIO AND (NOT TARGET Qt::Multimedia OR NOT TARGET Qt::TextToSpeech))
    message(FATAL_ERROR
        "Audio requires Qt Multimedia and Qt TextToSpeech development modules. On Debian/Ubuntu Qt 5: install qtmultimedia5-dev libqt5texttospeech5-dev. Runtime speech also needs a Qt speech plugin (qtspeech5-speechd-plugin). Only intentionally silent developer builds should use -DAPM_REQUIRE_QT_AUDIO=OFF.")
endif()

# The 3D compass plot is optional at configure time. A QWidget fallback keeps the complete
# calibration workflow available on minimal Linux installations and in packaging jobs.
find_package(${APM_QT_PACKAGE} QUIET COMPONENTS DataVisualization)
if(TARGET Qt::DataVisualization)
    set(APM_DATA_VISUALIZATION_LIBRARY Qt::DataVisualization)
    set(APM_HAS_DATA_VISUALIZATION TRUE)
    message(STATUS "Compass calibration: Qt Data Visualization 3D plot enabled")
else()
    set(APM_DATA_VISUALIZATION_LIBRARY "")
    set(APM_HAS_DATA_VISUALIZATION FALSE)
    message(STATUS "Compass calibration: using built-in 2D plot fallback")
endif()

# Qt does not expose arbitrary flight-stick axes, so SDL is the isolated input
# backend on every supported desktop platform.
find_package(SDL2 REQUIRED)
if(TARGET SDL2::SDL2)
    set(APM_SDL2_TARGET SDL2::SDL2)
else()
    add_library(apm_sdl2 INTERFACE)
    target_include_directories(apm_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(apm_sdl2 INTERFACE ${SDL2_LIBRARIES})
    set(APM_SDL2_TARGET apm_sdl2)
endif()

# MAVLink include
set(MAVLINKPATH "${CMAKE_SOURCE_DIR}/libs/mavlink/include/mavlink/v2.0")
set(MAVLINK_CONF "ardupilotmega")
if(NOT EXISTS "${MAVLINKPATH}/${MAVLINK_CONF}")
    message(
        FATAL_ERROR "MAVLink dialect '${MAVLINK_CONF}' does not exist at '${MAVLINKPATH}'! Run 'git submodule update --init' to fetch it."
    )
endif()

# MAV_CMD is a generated C enum without reflection support. Build the planner
# catalog from the vendored dialect so the command editor always follows the
# exact MAVLink revision compiled into the application.
set(_apm_mav_command_header
    "${MAVLINKPATH}/${MAVLINK_CONF}/${MAVLINK_CONF}.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_apm_mav_command_header}")
file(STRINGS "${_apm_mav_command_header}" _apm_mav_command_lines
    REGEX "^[ \t]*MAV_CMD_[A-Z0-9_]+=[0-9]+,")
set(_apm_mav_command_entries "")
set(_apm_mav_command_count 0)
set(_apm_has_do_return_path_start FALSE)
foreach(_apm_mav_command_line IN LISTS _apm_mav_command_lines)
    string(REGEX MATCH "MAV_CMD_([A-Z0-9_]+)=([0-9]+),"
        _apm_mav_command_match "${_apm_mav_command_line}")
    set(_apm_mav_command_symbol "${CMAKE_MATCH_1}")
    if(_apm_mav_command_symbol STREQUAL "ENUM_END")
        break()
    endif()
    if(_apm_mav_command_symbol STREQUAL "DO_RETURN_PATH_START")
        set(_apm_has_do_return_path_start TRUE)
    endif()
    string(REGEX REPLACE "^NAV_" "" _apm_mav_command_name
        "${_apm_mav_command_symbol}")
    string(APPEND _apm_mav_command_entries
        "    {MAV_CMD_${_apm_mav_command_symbol}, \"${_apm_mav_command_name}\"},\n")
    math(EXPR _apm_mav_command_count "${_apm_mav_command_count} + 1")
endforeach()
# Mission Planner's current dialect defines this WIP location-bearing command,
# while the older generated C dialect vendored by this port does not. Keep the
# planner catalog complete without inventing a C enum symbol that is absent
# from the protocol headers. The guard prevents a duplicate after a future
# dialect update.
if(NOT _apm_has_do_return_path_start)
    string(APPEND _apm_mav_command_entries
        "    {188, \"DO_RETURN_PATH_START\"},\n")
    math(EXPR _apm_mav_command_count "${_apm_mav_command_count} + 1")
endif()
if(_apm_mav_command_count LESS 100)
    message(FATAL_ERROR
        "Only ${_apm_mav_command_count} MAV_CMD entries were read from ${_apm_mav_command_header}")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/MavCommandCatalog.inc"
    "const std::array<CommandEntry, ${_apm_mav_command_count}> kCommands{{\n"
    "${_apm_mav_command_entries}}};\n")

add_library(apm_mission_command_catalog STATIC
    src/ui/flightplanner/MissionCommandCatalog.cpp
    src/ui/flightplanner/MissionCommandCatalog.h
)
target_include_directories(apm_mission_command_catalog
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
)
target_link_libraries(apm_mission_command_catalog PUBLIC Qt::Core)

# Qt has hashes but no authenticated encryption API. Signing-key encryption
# uses OpenSSL on all supported platforms; never fall back to plaintext storage.
find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
add_library(apm_mavlink_signing STATIC
    src/comm/MAVLinkSigningManager.cpp
    src/comm/MAVLinkSigningManager.h
    src/comm/MAVLinkSigningClock.cpp
    src/comm/MAVLinkSigningClock.h
    src/comm/MAVLinkSigningSession.cpp
    src/comm/MAVLinkSigningSession.h
    src/services/MavAuthKeyStore.cpp
    src/services/MavAuthKeyStore.h
    src/services/MavAuthKeyService.cpp
    src/services/MavAuthKeyService.h
    src/services/MavlinkSigningProfiles.cpp
    src/services/MavlinkSigningProfiles.h
    src/services/SigningProvisioningTarget.h
)
target_include_directories(apm_mavlink_signing
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    PRIVATE ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF}
)
target_link_libraries(apm_mavlink_signing PUBLIC Qt::Core PRIVATE OpenSSL::Crypto)

# AGLLIB math library & OPMapControl
add_subdirectory(libs/alglib EXCLUDE_FROM_ALL)
add_subdirectory(libs/opmapcontrol EXCLUDE_FROM_ALL)

# External: quazip (can be via add_subdirectory or find_package if installed)
add_subdirectory(libs/thirdParty/quazip EXCLUDE_FROM_ALL)
find_package(ZLIB REQUIRED)

option(APM_USE_QML_PFD
    "Use the legacy Qt Quick primary flight display instead of the QtWidgets display" OFF)

add_library(apm_parameter_core STATIC
    src/core/VehicleTarget.h
    src/core/parameters/ParameterCodec.cpp
    src/core/parameters/ParameterCodec.h
    src/core/parameters/ParameterMetaData.cpp
    src/core/parameters/ParameterMetaDataParser.cpp
    src/core/parameters/ParameterMetaDataParser.h
    src/core/parameters/ParameterMetaData.h
    src/core/parameters/ParameterMetaDataRepository.cpp
    src/core/parameters/ParameterMetaDataRepository.h
    src/core/parameters/ParameterStore.cpp
    src/core/parameters/ParameterStore.h
)
target_include_directories(apm_parameter_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(apm_parameter_core PUBLIC Qt::Core)

add_library(apm_parameter_regeneration STATIC
    src/core/parameters/ParameterMetaDataRegenerationService.cpp
    src/core/parameters/ParameterMetaDataRegenerationService.h
)
target_link_libraries(apm_parameter_regeneration PUBLIC apm_parameter_core Qt::Network)


# Gather sources/headers/forms/resources
set(SOURCES
    qgroundcontrol.qrc
    src/AppPaths.cpp
    src/AppPaths.h
    src/qml/ApmQmlApi.cpp
    src/qml/ApmQmlApi.h
    src/qml/QmlPluginContext.cpp
    src/qml/QmlPluginContext.h
    src/qml/QmlPluginManager.cpp
    src/qml/QmlPluginManager.h
    src/qml/QmlPluginManifest.cpp
    src/qml/QmlPluginManifest.h
    src/core/parameters/ParameterMetaDataUpdater.cpp
    src/core/parameters/ParameterMetaDataUpdater.h
    src/GAudioOutput.cc
    src/GAudioOutput.h
    src/LogCompressor.cc
    src/LogCompressor.h
    src/MG.h
    src/QGC.cc
    src/QGC.h
    src/QGCCore.cc
    src/QGCCore.h
    src/QGCGeo.cc
    src/QGCGeo.h
    src/Settings.cpp
    src/Settings.h
    src/services/SpeechAnnouncer.cpp
    src/services/SpeechAnnouncer.h
    src/services/SpeechTelemetrySource.cpp
    src/services/SpeechTelemetrySource.h
    src/services/StatusMessageSettings.cpp
    src/services/StatusMessageSettings.h
    src/services/StatusTextPolicy.cpp
    src/services/StatusTextPolicy.h
    src/services/HudDisplaySettings.cpp
    src/services/HudDisplaySettings.h
    src/services/SpeechSettings.cpp
    src/services/SpeechSettings.h
    src/Waypoint.cc
    src/Waypoint.h
    src/audio/QtAudioOutput.cpp
    src/audio/QtAudioOutput.h
    src/audio/QueuedSpeechController.cpp
    src/audio/QueuedSpeechController.h
    src/comm/AbsPositionOverview.cc
    src/comm/AbsPositionOverview.h
    src/comm/DroneCanForwardingBroker.cpp
    src/comm/DroneCanForwardingBroker.h
    src/comm/DroneCanFrameCodec.cpp
    src/comm/DroneCanFrameCodec.h
    src/comm/DroneCanGetNodeInfoClient.cpp
    src/comm/DroneCanGetNodeInfoClient.h
    src/comm/DroneCanGetSetClient.cpp
    src/comm/DroneCanGetSetClient.h
    src/comm/DroneCanMavlinkTransport.cpp
    src/comm/DroneCanMavlinkTransport.h
    src/comm/AdsbIdentificationClient.cpp
    src/comm/AdsbIdentificationClient.h
    src/comm/CompassCalibrationService.cpp
    src/comm/CompassCalibrationService.h
    src/comm/Esp8266ParameterClient.cpp
    src/comm/Esp8266ParameterClient.h
    src/comm/ExactLinkTransmitter.cpp
    src/comm/ExactLinkTransmitter.h
    src/comm/ExactMissionSnapshotService.cpp
    src/comm/ExactMissionSnapshotService.h
    src/comm/GuidedTargetService.cpp
    src/comm/GuidedTargetService.h
    src/comm/FollowMeGpsInput.cpp
    src/comm/FollowMeGpsInput.h
    src/comm/MovingBaseInputTransport.cpp
    src/comm/MovingBaseInputTransport.h
    src/comm/MovingBaseNmeaLog.cpp
    src/comm/MovingBaseNmeaLog.h
    src/comm/MovingBasePositionStore.cpp
    src/comm/MovingBasePositionStore.h
    src/comm/MovingBaseService.cpp
    src/comm/MovingBaseService.h
    src/comm/MavFtpProtocol.cpp
    src/comm/MavFtpProtocol.h
    src/comm/MavFtpService.cpp
    src/comm/MavFtpService.h
    src/comm/MavFtpServiceInterface.h
    src/comm/GpsCorrectionSource.cpp
    src/comm/GpsCorrectionSource.h
    src/comm/GpsRtcmPacketizer.cpp
    src/comm/GpsRtcmPacketizer.h
    src/comm/LinkInterface.cpp
    src/comm/LinkInterface.h
    src/comm/LinkManager.cc
    src/comm/LinkManager.h
    src/comm/LinkManagerFactory.cpp
    src/comm/LinkManagerFactory.h
    src/comm/MAVLinkDecoder.cc
    src/comm/MAVLinkDecoder.h
    src/comm/MAVLinkFrameParser.cpp
    src/comm/MAVLinkFrameParser.h
    src/comm/NmeaGgaParser.cpp
    src/comm/NmeaGgaParser.h
    src/comm/NmeaLineFramer.cpp
    src/comm/NmeaLineFramer.h
    src/comm/MavlinkMirrorOutput.cpp
    src/comm/MavlinkMirrorOutput.h
    src/comm/MavlinkMirrorService.cpp
    src/comm/MavlinkMirrorService.h
    src/comm/CotEventSerializer.cpp
    src/comm/CotEventSerializer.h
    src/comm/CotOutputTransport.cpp
    src/comm/CotOutputTransport.h
    src/comm/CotOutputService.cpp
    src/comm/CotOutputService.h
    src/comm/DeviceOperationService.cpp
    src/comm/DeviceOperationService.h
    src/comm/NmeaSentenceBuilder.cpp
    src/comm/NmeaSentenceBuilder.h
    src/comm/NmeaOutputService.cpp
    src/comm/NmeaOutputService.h
    src/comm/ParameterService.cpp
    src/comm/MavlinkComponentInstanceLease.h
    src/comm/MavlinkComponentRegistry.cpp
    src/comm/MavlinkComponentRegistry.h
    src/comm/Px4FlowFrameAssembler.cpp
    src/comm/Px4FlowFrameAssembler.h
    src/comm/Px4FlowService.cpp
    src/comm/Px4FlowService.h
    src/comm/ParameterService.h
    src/comm/VehicleCommandService.cpp
    src/comm/VehicleCommandService.h
    src/comm/VehicleEndpoint.cpp
    src/comm/VehicleEndpoint.h
    src/comm/SwarmCommandService.cpp
    src/comm/SwarmCommandService.h
    src/comm/SwarmTelemetryRegistry.cpp
    src/comm/SwarmTelemetryRegistry.h
    src/comm/VehicleTargetManager.cpp
    src/comm/VehicleTargetManager.h
    src/comm/RadioStatusMonitor.cpp
    src/comm/RadioStatusMonitor.h
    src/comm/MAVLinkProtocol.cc
    src/comm/MAVLinkProtocol.h
    src/comm/MAVLinkSimulationLink.cc
    src/comm/MAVLinkSimulationLink.h
    src/comm/MAVLinkSimulationMAV.cc
    src/comm/MAVLinkSimulationMAV.h
    src/comm/MAVLinkSimulationWaypointPlanner.cc
    src/comm/MAVLinkSimulationWaypointPlanner.h
    src/comm/MAVLinkSwarmSimulationLink.cc
    src/comm/MAVLinkSwarmSimulationLink.h
    src/comm/MissionItemProtocol.h
    src/comm/MissionProtocolCoordinator.cc
    src/comm/MissionProtocolCoordinator.h
    src/comm/MissionTransferController.cc
    src/comm/MissionTransferController.h
    src/comm/MissionOverview.cc
    src/comm/MissionOverview.h
    src/comm/MissionTransferService.cc
    src/comm/MissionTransferService.h
    src/comm/MissionTransferTransport.h
    src/comm/ProtocolInterface.h
    src/comm/QGCFlightGearLink.cc
    src/comm/QGCFlightGearLink.h
    src/comm/QGCHilLink.h
    src/comm/QGCJSBSimLink.cc
    src/comm/QGCJSBSimLink.h
    src/comm/QGCMAVLink.h
    src/comm/RelPositionOverview.cc
    src/comm/RelPositionOverview.h
    src/comm/Rtcm3Parser.cpp
    src/comm/Rtcm3Parser.h
    src/comm/SerialLinkInterface.h
    src/comm/TCPLink.cc
    src/comm/TCPLink.h
    src/comm/TLogReplayLink.cc
    src/comm/TLogReplayLink.h
    src/comm/UASObject.cc
    src/comm/UASObject.h
    src/comm/UDPClientLink.cc
    src/comm/UDPClientLink.h
    src/comm/UDPLink.cc
    src/comm/UDPLink.h
    src/comm/UdpPeerState.cpp
    src/comm/UdpPeerState.h
    src/comm/LogDownloadTracker.cpp
    src/comm/LogDownloadTracker.h
    src/comm/ExactLogTransferService.cpp
    src/comm/ExactLogTransferService.h
    src/comm/VehicleOverview.cc
    src/comm/VehicleOverview.h
    src/comm/arduino_intelhex.cpp
    src/comm/arduino_intelhex.h
    src/comm/arduinoflash.cc
    src/comm/arduinoflash.h
    src/comm/serialconnection.cc
    src/comm/serialconnection.h
    src/configuration.h
    src/globalobject.cc
    src/globalobject.h
    src/input/JoystickInput.cc
    src/input/JoystickInput.h
    src/logging.h
    src/main.cc
    src/output/kmlcreator.cc
    src/output/kmlcreator.h
    src/output/logdata.cc
    src/output/logdata.h
    src/uas/APMFirmwareVersion.cpp
    src/uas/APMFirmwareVersion.h
    src/uas/ApmUiHelpers.cc
    src/uas/ApmUiHelpers.h
    src/uas/ArduPilotMegaMAV.cc
    src/uas/ArduPilotMegaMAV.h
    src/uas/MAVLinkComponentTracker.h
    src/uas/LogDownloadDialog.cc
    src/uas/LogDownloadDialog.h
    src/uas/LogDownloadDialog.ui
    src/uas/PxQuadMAV.cc
    src/uas/PxQuadMAV.h
    src/uas/Proximity.cpp
    src/uas/Proximity.h
    src/uas/ServoOutputDecoder.cpp
    src/uas/ServoOutputDecoder.h
    src/uas/QGCMAVLinkUASFactory.cc
    src/uas/QGCMAVLinkUASFactory.h
    src/uas/QGCUASParamManager.cc
    src/uas/QGCUASParamManager.h
    src/uas/SlugsMAV.cc
    src/uas/SlugsMAV.h
    src/uas/UAS.cc
    src/uas/UAS.h
    src/uas/UASInterface.h
    src/uas/UASMissionTransferTransport.cpp
    src/uas/UASMissionTransferTransport.h
    src/uas/UASManager.cc
    src/uas/UASManager.h
    src/uas/UASParameter.cpp
    src/uas/UASParameter.h
    src/uas/UASWaypointManager.cc
    src/uas/UASWaypointManager.h
    src/uas/senseSoarMAV.cpp
    src/uas/senseSoarMAV.h
    src/ui/AP2DataPlot2D.cpp
    src/ui/AP2DataPlot2D.h
    src/ui/AP2DataPlot2D.ui
    src/ui/AP2DataPlotAxisDialog.cc
    src/ui/AP2DataPlotAxisDialog.h
    src/ui/AP2DataPlotAxisDialog.ui
    src/ui/AP2DataPlotThread.cc
    src/ui/AP2DataPlotThread.h
    src/ui/AboutDialog.cc
    src/ui/AboutDialog.h
    src/ui/AboutDialog.ui
    src/ui/BackstageView.cpp
    src/ui/BackstageView.h
    src/ui/AddConnectionViewModel.cpp
    src/ui/AddConnectionViewModel.h
    src/ui/AddConnectionWindow.cpp
    src/ui/AddConnectionWindow.h
    src/ui/ConnectionOptionsViewModel.cpp
    src/ui/ConnectionOptionsViewModel.h
    src/ui/ConnectionOptionsWindow.cpp
    src/ui/ConnectionOptionsWindow.h
    src/ui/MainWindowHeader.cpp
    src/ui/MainWindowHeader.h
    src/ui/AudioOutputWidget.cc
    src/ui/AudioOutputWidget.h
    src/ui/AudioOutputWidget.ui
    src/ui/AutoUpdateCheck.cc
    src/ui/AutoUpdateCheck.h
    src/ui/AutoUpdateDialog.cc
    src/ui/AutoUpdateDialog.h
    src/ui/AutoUpdateDialog.ui
    src/ui/CommConfigurationWindow.cc
    src/ui/CommConfigurationWindow.h
    src/ui/CommSettings.ui
    src/ui/DebugOutput.cc
    src/ui/DebugOutput.h
    src/ui/DebugOutput.ui
    src/ui/FlightDataView.cpp
    src/ui/FlightDataView.h
    src/ui/flightdata/FlightDataViewModel.cpp
    src/ui/flightdata/FlightDataViewModel.h
    src/ui/flightdata/FlightDataMapOverlay.cpp
    src/ui/flightdata/FlightDataMapOverlay.h
    src/ui/flightdata/MovingBaseMapController.cpp
    src/ui/flightdata/MovingBaseMapController.h
    src/ui/flightdata/FlightDataMissionProgress.cpp
    src/ui/flightdata/FlightDataMissionProgress.h
    src/ui/flightdata/HudControl.cpp
    src/ui/flightdata/HudControl.h
    src/ui/flightdata/PreflightChecklistModel.cpp
    src/ui/flightdata/PreflightChecklistModel.h
    src/ui/flightdata/PreflightChecklistWidget.cpp
    src/ui/flightdata/PreflightChecklistWidget.h
    src/ui/flightdata/ProximityRadarControl.cpp
    src/ui/flightdata/ProximityRadarControl.h
    src/ui/flightdata/ProximityWindow.cpp
    src/ui/flightdata/ProximityWindow.h
    src/ui/flightdata/SimpleActionsWidget.cpp
    src/ui/flightdata/SimpleActionsWidget.h
    src/ui/flightplanner/FlightPlannerMissionModel.cpp
    src/ui/flightplanner/FlightPlannerMissionModel.h
    src/ui/flightplanner/FlightPlannerMissionCodec.cpp
    src/ui/flightplanner/FlightPlannerMissionCodec.h
    src/ui/flightplanner/FenceRallyFileCodec.cpp
    src/ui/flightplanner/FenceRallyFileCodec.h
    src/ui/flightplanner/FenceRallyModel.cpp
    src/ui/flightplanner/FenceRallyModel.h
    src/ui/flightplanner/FenceRallyController.cpp
    src/ui/flightplanner/FenceRallyController.h
    src/ui/flightplanner/QgcPlanFileCodec.cpp
    src/ui/flightplanner/QgcPlanFileCodec.h
    src/ui/flightplanner/FlightPlannerNavigation.cpp
    src/ui/flightplanner/FlightPlannerNavigation.h
    src/ui/flightplanner/FlightPlannerRouteMetrics.cpp
    src/ui/flightplanner/FlightPlannerRouteMetrics.h
    src/ui/flightplanner/FlightPlannerMeasurement.cpp
    src/ui/flightplanner/FlightPlannerMeasurement.h
    src/ui/flightplanner/MissionElevationProfile.cpp
    src/ui/flightplanner/MissionElevationProfile.h
    src/ui/flightplanner/FlightPlannerPolygonModel.cpp
    src/ui/flightplanner/FlightPlannerPolygonModel.h
    src/ui/flightplanner/SurveyGridGenerator.cpp
    src/ui/flightplanner/SurveyGridGenerator.h
    src/ui/flightplanner/SurveyGridDialog.cpp
    src/ui/flightplanner/SurveyGridDialog.h
    src/ui/flightplanner/SurveyMissionBuilder.cpp
    src/ui/flightplanner/SurveyMissionBuilder.h
    src/ui/flightplanner/FlightPlannerActionPanel.cpp
    src/ui/flightplanner/FlightPlannerActionPanel.h
    src/ui/flightplanner/FlightPlannerViewModel.cpp
    src/ui/flightplanner/FlightPlannerViewModel.h
    src/ui/flightplanner/FlightPlannerPrefetchController.cpp
    src/ui/flightplanner/FlightPlannerPrefetchController.h
    src/ui/flightplanner/FlightPlannerWaypointPanel.cpp
    src/ui/flightplanner/FlightPlannerWaypointPanel.h
    src/ui/flightplanner/WpRow.cpp
    src/ui/flightplanner/WpRow.h
    src/ui/map/PlannerMeasurementOverlay.cpp
    src/ui/map/PlannerMeasurementOverlay.h
    src/ui/FlightPlannerView.cpp
    src/ui/FlightPlannerView.h
    src/ui/HelpView.cpp
    src/ui/HelpView.h
    src/ui/docking/DockableView.cpp
    src/ui/docking/DockableView.h
    src/ui/EKFMonitor.cpp
    src/ui/EKFMonitor.h
    src/ui/GraphTreeWidgetItem.cc
    src/ui/GraphTreeWidgetItem.h
    src/ui/HDDisplay.cc
    src/ui/HDDisplay.h
    src/ui/HDDisplay.ui
    src/ui/HSIDisplay.cc
    src/ui/HSIDisplay.h
    src/ui/HUD.cc
    src/ui/HUD.h
    src/ui/JoystickWidget.cc
    src/ui/JoystickWidget.h
    src/ui/JoystickWidget.ui
    src/ui/Loghandling/AP2DataPlotStatus.cpp
    src/ui/Loghandling/AP2DataPlotStatus.h
    src/ui/Loghandling/AsciiLogParser.cpp
    src/ui/Loghandling/AsciiLogParser.h
    src/ui/Loghandling/BinLogParser.cpp
    src/ui/Loghandling/BinLogParser.h
    src/ui/Loghandling/DataFlashSpectrogramAnalyzer.cpp
    src/ui/Loghandling/DataFlashSpectrogramAnalyzer.h
    src/ui/Loghandling/DataFlashSpectrogramService.cpp
    src/ui/Loghandling/DataFlashSpectrogramService.h
    src/ui/Loghandling/DataFlashKmlExporter.cpp
    src/ui/Loghandling/DataFlashKmlExporter.h
    src/ui/Loghandling/DataFlashFftCore.cpp
    src/ui/Loghandling/DataFlashFftCore.h
    src/ui/Loghandling/DataFlashFftAnalyzer.cpp
    src/ui/Loghandling/DataFlashFftAnalyzer.h
    src/ui/Loghandling/DataFlashFftService.cpp
    src/ui/Loghandling/LogAnonymizer.cpp
    src/ui/Loghandling/LogAnonymizeService.cpp
    src/ui/Loghandling/DataFlashLogAnonymizer.cpp
    src/ui/Loghandling/TelemetryLogAnonymizer.cpp
    src/ui/AnonLogWindow.cpp
    src/services/WarningEngine.cpp
    src/services/DeveloperVehicleToolService.cpp
    src/services/DeveloperVehicleToolService.h
    src/services/WarningTelemetrySource.cpp
    src/ui/WarningManagerWindow.cpp
    src/ui/MavlinkSigningWindow.cpp
    src/ui/flightdata/QuickViewWidget.cpp
    src/ui/Loghandling/DataFlashFftService.h
    src/ui/ConfigFFTWindow.cpp
    src/ui/ConfigFFTWindow.h
    src/ui/configuration/ConfigFFTView.cpp
    src/ui/configuration/ConfigPX4FlowView.cpp
    src/ui/configuration/ConfigPX4FlowView.h
    src/ui/configuration/ConfigFFTView.h
    src/ui/configuration/ConfigFFTViewModel.cpp
    src/ui/configuration/ConfigFFTViewModel.h
    src/ui/configuration/ConfigFFTIntegration.cpp
    src/ui/configuration/ConfigFFTIntegration.h
    src/ui/LogDownloadViewModel.cpp
    src/ui/LogDownloadViewModel.h
    src/ui/LogDownloadWindow.cpp
    src/ui/LogDownloadWindow.h
    src/ui/Loghandling/ILogParser.h
    src/ui/Loghandling/ILogdataSink.h
    src/ui/Loghandling/IParserCallback.h
    src/ui/Loghandling/LogAnalysis.cpp
    src/ui/Loghandling/LogAnalysis.h
    src/ui/Loghandling/LogAnalysis.ui
    src/ui/Loghandling/LogAnalysisMap.cpp
    src/ui/Loghandling/LogAnalysisMap.h
    src/ui/Loghandling/LogAnalysisMap.ui
    src/ui/Loghandling/LogExporter.cpp
    src/ui/Loghandling/LogExporter.h
    src/ui/Loghandling/LogParserBase.cpp
    src/ui/Loghandling/LogParserBase.h
    src/ui/Loghandling/LogdataStorage.cpp
    src/ui/Loghandling/LogdataStorage.h
    src/ui/Loghandling/PresetManager.cpp
    src/ui/Loghandling/PresetManager.h
    src/ui/Loghandling/TlogParser.cpp
    src/ui/Loghandling/TlogParser.h
    src/ui/MAVLinkSettingsWidget.cc
    src/ui/MAVLinkSettingsWidget.h
    src/ui/MAVLinkSettingsWidget.ui
    src/ui/MainWindow.cc
    src/ui/MainWindow.h
    src/ui/MainWindow.ui
    src/ui/MissionElevationDisplay.cpp
    src/ui/MissionElevationDisplay.h
    src/ui/MissionElevationDisplay.ui
    src/ui/ObjectDetectionView.cc
    src/ui/ObjectDetectionView.h
    src/ui/ObjectDetectionView.ui
    src/ui/ParameterInterface.cc
    src/ui/ParameterInterface.h
    src/ui/ParameterInterface.ui
    src/ui/PrimaryFlightDisplay.cc
    src/ui/PrimaryFlightDisplay.h
    src/ui/PrimaryFlightDisplayQML.cpp
    src/ui/PrimaryFlightDisplayQML.h
    src/ui/QGCHilConfiguration.cc
    src/ui/QGCHilConfiguration.h
    src/ui/QGCHilConfiguration.ui
    src/ui/QGCHilFlightGearConfiguration.cc
    src/ui/QGCHilFlightGearConfiguration.h
    src/ui/QGCHilFlightGearConfiguration.ui
    src/ui/QGCHilJSBSimConfiguration.cc
    src/ui/QGCHilJSBSimConfiguration.h
    src/ui/QGCHilJSBSimConfiguration.ui
    src/ui/QGCMAVLinkInspector.cc
    src/ui/QGCMAVLinkInspector.h
    src/ui/QGCMAVLinkInspector.ui
    src/ui/MAVLinkInspectorMessageRelay.cpp
    src/ui/MAVLinkInspectorMessageRelay.h
    src/ui/MAVLinkInspectorWindow.cpp
    src/ui/MAVLinkInspectorWindow.h
    src/ui/MAVLinkInspectorPacketStore.cpp
    src/ui/MAVLinkInspectorPacketStore.h
    src/ui/MAVLinkInspectorTrafficSource.cpp
    src/ui/MAVLinkInspectorTrafficSource.h
    src/ui/MAVLinkInspectorView.cpp
    src/ui/MAVLinkInspectorView.h
    src/ui/MavlinkGraphSampleExtractor.cpp
    src/ui/MavlinkGraphSampleExtractor.h
    src/ui/MavlinkFieldGraphModel.cpp
    src/ui/MavlinkFieldGraphModel.h
    src/ui/MavlinkFieldGraphWindow.cpp
    src/ui/MavlinkFieldGraphWindow.h
    src/ui/MAVLinkReplaySource.cpp
    src/ui/MAVLinkReplaySource.h
    src/comm/MAVLinkMessageMetaType.h
    src/ui/LinkStatsWindow.cpp
    src/ui/LinkStatsWindow.h
    src/ui/LinkStatsSampleSource.cpp
    src/comm/TlogReader.cpp
    src/comm/GpsCorrectionExtractor.cpp
    src/comm/GpsCorrectionExtractor.h
    src/ui/Loghandling/DataFlashLogSplitter.cpp
    src/ui/Loghandling/DataFlashLogSplitter.h
    src/ui/Loghandling/DataFlashRawReader.h
    src/ui/Loghandling/DataFlashModeNames.h
    src/ui/Loghandling/DataFlashDashWareCsvExporter.cpp
    src/ui/Loghandling/DataFlashDashWareCsvExporter.h
    src/ui/tools/ApjDefaultsEmbedder.cpp
    src/ui/tools/ApjDefaultsEmbedder.h
    src/ui/Loghandling/FlightLogClassifier.cpp
    src/ui/Loghandling/FlightLogClassifier.h
    src/ui/Loghandling/FlightLogOrganizer.cpp
    src/ui/Loghandling/FlightLogOrganizer.h
    src/comm/TlogReader.h
    src/comm/TlogExportService.cpp
    src/comm/TlogExportService.h
    src/ui/MavlinkLogWindow.cpp
    src/ui/MavlinkLogWindow.h
    src/ui/SpectrogramWindow.cpp
    src/ui/SpectrogramWindow.h
    src/ui/SpectrogramWindowIntegration.cpp
    src/ui/SerialPassThroughViewModel.cpp
    src/ui/SerialPassThroughViewModel.h
    src/ui/SerialPassThroughWindow.cpp
    src/ui/SerialPassThroughWindow.h
    src/ui/SerialPassThroughWindowIntegration.cpp
    src/ui/SerialOutputNMEAViewModel.cpp
    src/ui/SerialOutputNMEAViewModel.h
    src/ui/SerialOutputNMEAWindow.cpp
    src/ui/SerialOutputNMEAWindow.h
    src/ui/SerialOutputNMEAWindowIntegration.cpp
    src/ui/CotIdentityModel.cpp
    src/ui/CotIdentityModel.h
    src/ui/SerialOutputCotViewModel.cpp
    src/ui/SerialOutputCotViewModel.h
    src/ui/SerialOutputCotWindow.cpp
    src/ui/SerialOutputCotWindow.h
    src/ui/SerialOutputCotWindowIntegration.cpp
    src/ui/DeviceOperationsViewModel.cpp
    src/ui/DeviceOperationsViewModel.h
    src/ui/DeviceOperationsWindow.cpp
    src/ui/DeviceOperationsWindow.h
    src/ui/DeviceOperationsWindowIntegration.cpp
    src/ui/ExternalGuidedWindow.cpp
    src/ui/ExternalGuidedWindow.h
    src/ui/ExternalGuidedWindowIntegration.cpp
    src/ui/FollowMeWindow.cpp
    src/ui/FollowMeWindow.h
    src/ui/FollowMeWindowIntegration.cpp
    src/ui/FormationGridControl.cpp
    src/ui/FormationGridControl.h
    src/ui/SwarmFormationWindow.cpp
    src/ui/SwarmFormationWindow.h
    src/ui/SwarmFormationWindowIntegration.cpp
    src/ui/SwarmFollowPathWindow.cpp
    src/ui/SwarmFollowPathWindow.h
    src/ui/SwarmFollowPathWindowIntegration.cpp
    src/ui/SwarmFollowLeaderWindow.cpp
    src/ui/SwarmFollowLeaderWindow.h
    src/ui/SwarmFollowLeaderWindowIntegration.cpp
    src/ui/WaypointLeaderProfileControl.cpp
    src/ui/WaypointLeaderProfileControl.h
    src/ui/SwarmWaypointLeaderWindow.cpp
    src/ui/SwarmWaypointLeaderWindow.h
    src/ui/SwarmWaypointLeaderWindowAdapter.cpp
    src/ui/SwarmWaypointLeaderWindowAdapter.h
    src/ui/SwarmWaypointLeaderWindowIntegration.cpp
    src/services/SwarmSequenceExecutor.cpp
    src/services/SwarmSequenceExecutor.h
    src/services/SwarmWaypointLeaderExecutor.cpp
    src/services/SwarmWaypointLeaderExecutor.h
    src/comm/SwarmFlightMode.h
    src/ui/MovingBaseWindow.cpp
    src/ui/MovingBaseWindow.h
    src/ui/MovingBaseWindowIntegration.cpp
    src/ui/MissionPlannerToolsMenu.cpp
    src/ui/MissionPlannerToolsMenu.h
    src/ui/OsdVideoOverlayWindow.cpp
    src/ui/OsdVideoOverlayWindow.h
    src/ui/SequenceLayoutControl.cpp
    src/ui/SequenceLayoutControl.h
    src/ui/SwarmSequenceWindow.cpp
    src/ui/SwarmSequenceWindow.h
    src/ui/SwarmSequenceWindowAdapter.cpp
    src/ui/SwarmSequenceWindowAdapter.h
    src/ui/SwarmSequenceWindowIntegration.cpp
    src/ui/Terrain3DWindow.cpp
    src/ui/Terrain3DWindow.h
    src/ui/Terrain3DWindowIntegration.cpp
    src/ui/terrain/Terrain3DCore.cpp
    src/ui/terrain/Terrain3DCore.h
    src/ui/terrain/Terrain3DTelemetrySource.cpp
    src/ui/terrain/Terrain3DTelemetrySource.h
    src/ui/tools/ExternalGuidedFile.cpp
    src/ui/tools/ExternalGuidedFile.h
    src/ui/tools/MjpegAviWriter.cpp
    src/ui/tools/MjpegAviWriter.h
    src/ui/tools/OsdVideoDecoder.cpp
    src/ui/tools/OsdVideoDecoder.h
    src/ui/tools/OsdVideoOverlayCore.cpp
    src/ui/tools/OsdVideoOverlayCore.h
    src/ui/tools/PropagationCore.cpp
    src/ui/tools/PropagationCore.h
    src/ui/tools/SwarmSequenceCore.cpp
    src/ui/tools/SwarmSequenceCore.h
    src/ui/tools/PropagationDistanceEstimator.cpp
    src/ui/tools/PropagationDistanceEstimator.h
    src/ui/tools/PropagationOverlayController.cpp
    src/ui/tools/PropagationOverlayController.h
    src/ui/tools/PropagationSettingsStore.cpp
    src/ui/tools/PropagationSettingsStore.h
    src/ui/tools/PropagationSettingsWindow.cpp
    src/ui/tools/PropagationSettingsWindow.h
    src/ui/tools/SwarmFormationCore.cpp
    src/ui/tools/SwarmFormationCore.h
    src/ui/tools/SwarmFollowPathCore.cpp
    src/ui/tools/SwarmFollowPathCore.h
    src/ui/tools/SwarmFollowLeaderCore.cpp
    src/ui/tools/SwarmFollowLeaderCore.h
    src/ui/tools/SwarmWaypointLeaderCore.cpp
    src/ui/tools/SwarmWaypointLeaderCore.h
    src/ui/tools/PropagationTelemetrySource.cpp
    src/ui/tools/PropagationTelemetrySource.h
    src/ui/QGCMAVLinkLogPlayer.cc
    src/ui/QGCMAVLinkLogPlayer.h
    src/ui/QGCMAVLinkLogPlayer.ui
    src/ui/QGCMainWindowAPConfigurator.cc
    src/ui/QGCMainWindowAPConfigurator.h
    src/ui/QGCParamWidget.cc
    src/ui/QGCParamWidget.h
    src/ui/QGCRGBDView.cc
    src/ui/QGCRGBDView.h
    src/ui/QGCSensorSettingsWidget.cc
    src/ui/QGCSensorSettingsWidget.h
    src/ui/QGCSensorSettingsWidget.ui
    src/ui/QGCSettingsWidget.cc
    src/ui/QGCSettingsWidget.h
    src/ui/QGCSettingsWidget.ui
    src/ui/QGCStatusBar.cc
    src/ui/QGCStatusBar.h
    src/ui/QGCTCPLinkConfiguration.cc
    src/ui/QGCTCPLinkConfiguration.h
    src/ui/QGCTCPLinkConfiguration.ui
    src/ui/QGCTabbedInfoView.cpp
    src/ui/QGCTabbedInfoView.h
    src/ui/QGCTabbedInfoView.ui
    src/ui/QGCToolBar.cc
    src/ui/QGCToolBar.h
    src/ui/QGCUDPClientLinkConfiguration.cc
    src/ui/QGCUDPClientLinkConfiguration.h
    src/ui/QGCUDPClientLinkConfiguration.ui
    src/ui/QGCUDPLinkConfiguration.cc
    src/ui/QGCUDPLinkConfiguration.h
    src/ui/QGCUDPLinkConfiguration.ui
    src/ui/QGCVehicleConfig.cc
    src/ui/QGCVehicleConfig.h
    src/ui/QGCVehicleConfig.ui
    src/ui/QGCWaypointListMulti.cc
    src/ui/QGCWaypointListMulti.h
    src/ui/QGCWaypointListMulti.ui
    src/ui/RadioCalibration/RadioCalibrationData.cc
    src/ui/RadioCalibration/RadioCalibrationData.h
    src/ui/SerialConfigurationWindow.cc
    src/ui/SerialConfigurationWindow.h
    src/ui/SerialSettings.ui
    src/ui/SlugsDataSensorView.cc
    src/ui/SlugsDataSensorView.h
    src/ui/SlugsDataSensorView.ui
    src/ui/SlugsHilSim.cc
    src/ui/SlugsHilSim.h
    src/ui/SlugsHilSim.ui
    src/ui/SlugsPadCameraControl.cpp
    src/ui/SlugsPadCameraControl.h
    src/ui/SlugsPadCameraControl.ui
    src/ui/UASRawStatusView.cpp
    src/ui/UASRawStatusView.h
    src/ui/UASRawStatusView.ui
    src/ui/UnconnectedUASInfoWidget.cc
    src/ui/UnconnectedUASInfoWidget.h
    src/ui/UnconnectedUASInfoWidget.ui
    src/ui/VibrationMonitor.cpp
    src/ui/VibrationMonitor.h
    src/ui/WaypointEditableView.cc
    src/ui/WaypointEditableView.h
    src/ui/WaypointEditableView.ui
    src/ui/WaypointList.cc
    src/ui/WaypointList.h
    src/ui/WaypointList.ui
    src/ui/WaypointNavigation.cc
    src/ui/WaypointNavigation.h
    src/ui/WaypointViewOnlyView.cc
    src/ui/WaypointViewOnlyView.h
    src/ui/WaypointViewOnlyView.ui
    src/ui/configuration/AP2ConfigWidget.cc
    src/ui/configuration/AP2ConfigWidget.h
    src/ui/configuration/APDoubleSpinBox.cc
    src/ui/configuration/APDoubleSpinBox.h
    src/ui/configuration/APSpinBox.cc
    src/ui/configuration/APSpinBox.h
    src/ui/configuration/AccelCalibrationConfig.cc
    src/ui/configuration/AccelCalibrationConfig.h
    src/ui/configuration/AccelCalibrationConfig.ui
    src/ui/configuration/AdvParameterList.cc
    src/ui/configuration/AdvParameterList.h
    src/ui/configuration/AdvParameterList.ui
    src/ui/configuration/AdvancedParamConfig.cc
    src/ui/configuration/AdvancedParamConfig.h
    src/ui/configuration/AdvancedParamConfig.ui
    src/ui/configuration/AirspeedConfig.cc
    src/ui/configuration/AirspeedConfig.h
    src/ui/configuration/AirspeedConfig.ui
    src/ui/configuration/AntennaTrackerGeometry.cpp
    src/ui/configuration/AntennaTrackerGeometry.h
    src/ui/configuration/AntennaTrackerOutputs.cpp
    src/ui/configuration/AntennaTrackerOutputs.h
    src/ui/configuration/AntennaTrackerSerialService.cpp
    src/ui/configuration/AntennaTrackerSerialService.h
    src/ui/configuration/AntennaTrackerAxisPanel.cpp
    src/ui/configuration/AntennaTrackerAxisPanel.h
    src/ui/configuration/AntennaTrackerTelemetrySource.cpp
    src/ui/configuration/AntennaTrackerTelemetrySource.h
    src/ui/configuration/AntennaTrackerUIView.cpp
    src/ui/configuration/AntennaTrackerUIView.h
    src/ui/configuration/AntennaTrackerUIViewModel.cpp
    src/ui/configuration/AntennaTrackerUIViewModel.h
    src/ui/configuration/ConfigAntennaTrackerView.cpp
    src/ui/configuration/ConfigAntennaTrackerView.h
    src/ui/configuration/UasAntennaTrackerTelemetrySource.cpp
    src/ui/configuration/UasAntennaTrackerTelemetrySource.h
    src/ui/configuration/AntennaTrackerConfig.cc
    src/ui/configuration/AntennaTrackerConfig.h
    src/ui/configuration/AntennaTrackerConfig.ui
    src/ui/configuration/ApmCustomFirmwareConfig.cpp
    src/ui/configuration/ApmCustomFirmwareConfig.h
    src/ui/configuration/ApmHardwareConfig.cc
    src/ui/configuration/ApmHardwareConfig.h
    src/ui/configuration/ApmHardwareConfig.ui
    src/ui/configuration/ActionPageView.cpp
    src/ui/configuration/ActionPageView.h
    src/ui/configuration/ConfigAdvancedView.cpp
    src/ui/configuration/ParameterMetaDataRegenerationWindow.cpp
    src/ui/configuration/ParameterMetaDataRegenerationWindow.h
    src/ui/configuration/ConfigAdvancedView.h
    src/ui/configuration/ConfigADSBView.cpp
    src/ui/configuration/ConfigADSBView.h
    src/ui/configuration/ConfigADSBViewModel.cpp
    src/ui/configuration/ConfigADSBViewModel.h
    src/ui/configuration/ConfigDeveloperToolsView.cpp
    src/ui/configuration/ConfigDeveloperToolsView.h
    src/ui/configuration/ConfigDefaultSettingsView.cpp
    src/ui/configuration/ConfigDefaultSettingsView.h
    src/ui/configuration/ConfigHWIDView.cpp
    src/ui/configuration/ConfigHWIDView.h
    src/ui/configuration/ConfigHWIDViewModel.cpp
    src/ui/configuration/ConfigHWIDViewModel.h
    src/ui/configuration/ConfigHWOSDView.cpp
    src/ui/configuration/ConfigHWOSDView.h
    src/ui/configuration/ConfigHWOSDViewModel.cpp
    src/ui/configuration/ConfigHWOSDViewModel.h
    src/ui/configuration/ConfigOSDLayoutCanvas.cpp
    src/ui/configuration/ConfigOSDLayoutCanvas.h
    src/ui/configuration/ConfigOSDView.cpp
    src/ui/configuration/ConfigOSDView.h
    src/ui/configuration/ConfigOSDViewModel.cpp
    src/ui/configuration/ConfigOSDViewModel.h
    src/ui/configuration/DeveloperToolParsers.cpp
    src/ui/configuration/DeveloperToolParsers.h
    src/ui/configuration/Esp8266Settings.cpp
    src/ui/configuration/Esp8266Settings.h
    src/ui/configuration/FrameDefaultCatalogService.cpp
    src/ui/configuration/FrameDefaultCatalogService.h
    src/ui/configuration/QmlPluginManagerView.cpp
    src/ui/configuration/QmlPluginManagerView.h
    src/ui/configuration/SetupView.cpp
    src/ui/configuration/SetupView.h
    src/ui/configuration/ApmHighlighter.cc
    src/ui/configuration/ApmHighlighter.h
    src/ui/configuration/ApmPlaneLevel.cc
    src/ui/configuration/ApmPlaneLevel.h
    src/ui/configuration/ApmPlaneLevel.ui
    src/ui/configuration/ApmSoftwareConfig.cc
    src/ui/configuration/ApmSoftwareConfig.h
    src/ui/configuration/ApmSoftwareConfig.ui
    src/ui/configuration/ConfigView.cpp
    src/ui/configuration/ConfigView.h
    src/ui/configuration/ConfigRouteProfile.cpp
    src/ui/configuration/ConfigRouteProfile.h
    src/ui/configuration/DisplayViewProfile.cpp
    src/ui/configuration/DisplayViewProfile.h
    src/ui/configuration/PlannerStartupUdpOptions.cpp
    src/ui/configuration/PlannerStartupUdpOptions.h
    src/ui/configuration/ConfigFriendlyParamsView.cpp
    src/ui/configuration/ConfigFriendlyParamsView.h
    src/ui/configuration/ConfigFriendlyParamsViewModel.cpp
    src/ui/configuration/ConfigFriendlyParamsViewModel.h
    src/ui/configuration/ConfigRawParams.cpp
    src/ui/configuration/ConfigRawParams.h
    src/ui/configuration/ParamField.h
    src/ui/configuration/ConfigUserDefinedView.cpp
    src/ui/configuration/ConfigUserDefinedView.h
    src/ui/configuration/ConfigUserDefinedViewModel.cpp
    src/ui/configuration/ConfigUserDefinedViewModel.h
    src/ui/configuration/ConfigESCCalibrationView.cpp
    src/ui/configuration/ConfigESCCalibrationView.h
    src/ui/configuration/ConfigESCCalibrationViewModel.cpp
    src/ui/configuration/ConfigESCCalibrationViewModel.h
    src/ui/configuration/ConfigFlightModesView.cpp
    src/ui/configuration/ConfigFlightModesView.h
    src/ui/configuration/ConfigFlightModesViewModel.cpp
    src/ui/configuration/ConfigFlightModesViewModel.h
    src/ui/configuration/ConfigFlightModesPageFactory.cpp
    src/ui/configuration/ConfigFlightModesPageFactory.h
    src/ui/configuration/FlightModeTelemetryMonitor.cpp
    src/ui/configuration/FlightModeTelemetryMonitor.h
    src/ui/configuration/ConfigExtendedTuningView.cpp
    src/ui/configuration/ConfigExtendedTuningView.h
    src/ui/configuration/ConfigExtendedTuningViewModel.cpp
    src/ui/configuration/ConfigExtendedTuningViewModel.h
    src/ui/configuration/ConfigExtendedTuningPageFactory.cpp
    src/ui/configuration/ConfigExtendedTuningPageFactory.h
    src/ui/configuration/ConfigHWBTSerialService.cpp
    src/ui/configuration/ConfigHWBTSerialService.h
    src/ui/configuration/ConfigHWBTView.cpp
    src/ui/configuration/ConfigHWBTView.h
    src/ui/configuration/ConfigHWBTViewModel.cpp
    src/ui/configuration/ConfigHWBTViewModel.h
    src/ui/configuration/ConfigHWESP8266View.cpp
    src/ui/configuration/ConfigHWESP8266View.h
    src/ui/configuration/ConfigHWESP8266ViewModel.cpp
    src/ui/configuration/ConfigHWESP8266ViewModel.h
    src/ui/configuration/ConfigMotorTestView.cpp
    src/ui/configuration/ConfigMotorTestView.h
    src/ui/configuration/ConfigMotorTestViewModel.cpp
    src/ui/configuration/ConfigMotorTestViewModel.h
    src/ui/configuration/ConfigParachuteView.cpp
    src/ui/configuration/ConfigParachuteView.h
    src/ui/configuration/ConfigParachuteViewModel.cpp
    src/ui/configuration/ConfigParachuteViewModel.h
    src/ui/configuration/ConfigTradHeliView.cpp
    src/ui/configuration/ConfigTradHeliView.h
    src/ui/configuration/ConfigTradHeliViewModel.cpp
    src/ui/configuration/ConfigTradHeliViewModel.h
    src/ui/configuration/ConfigTradHeli4View.cpp
    src/ui/configuration/ConfigTradHeli4View.h
    src/ui/configuration/ConfigTradHeli4ViewModel.cpp
    src/ui/configuration/ConfigTradHeli4ViewModel.h
    src/ui/configuration/ConfigCompassView.cpp
    src/ui/configuration/ConfigCompassView.h
    src/ui/configuration/ConfigCompassViewModel.cpp
    src/ui/configuration/ConfigCompassViewModel.h
    src/ui/configuration/ConfigCompassMotView.cpp
    src/ui/configuration/ConfigCompassMotView.h
    src/ui/configuration/ConfigFrameClassTypeView.cpp
    src/ui/configuration/ConfigFrameClassTypeView.h
    src/ui/configuration/ConfigFrameClassTypeViewModel.cpp
    src/ui/configuration/ConfigFrameClassTypeViewModel.h
    src/ui/configuration/ConfigFrameTypeView.cpp
    src/ui/configuration/ConfigFrameTypeView.h
    src/ui/configuration/ConfigFrameTypeViewModel.cpp
    src/ui/configuration/ConfigFrameTypeViewModel.h
    src/ui/configuration/HeliCollectivePlot.cpp
    src/ui/configuration/HeliCollectivePlot.h
    src/ui/configuration/HeliVisualization.cpp
    src/ui/configuration/HeliVisualization.h
    src/ui/configuration/ConfigGpsInjectView.cpp
    src/ui/configuration/ConfigGpsInjectView.h
    src/ui/configuration/ConfigGpsInjectViewModel.cpp
    src/ui/configuration/ConfigGpsInjectViewModel.h
    src/ui/configuration/ConfigDroneCanView.cpp
    src/ui/configuration/ConfigDroneCanView.h
    src/ui/configuration/ConfigDroneCanViewModel.cpp
    src/ui/configuration/ConfigDroneCanViewModel.h
    src/ui/configuration/ConfigHWCANView.cpp
    src/ui/configuration/ConfigHWCANView.h
    src/ui/configuration/ConfigHWCANViewModel.cpp
    src/ui/configuration/ConfigHWCANViewModel.h
    src/ui/configuration/ConfigGPSOrderView.cpp
    src/ui/configuration/ConfigGPSOrderView.h
    src/ui/configuration/ConfigGPSOrderViewModel.cpp
    src/ui/configuration/ConfigGPSOrderViewModel.h
    src/ui/configuration/ConfigRadioOutputView.cpp
    src/ui/configuration/ConfigRadioOutputView.h
    src/ui/configuration/ConfigRadioOutputViewModel.cpp
    src/ui/configuration/ConfigRadioOutputViewModel.h
    src/ui/configuration/ConfigSerialView.cpp
    src/ui/configuration/ConfigSerialView.h
    src/ui/configuration/ConfigSerialViewModel.cpp
    src/ui/configuration/ConfigSerialViewModel.h
    src/ui/configuration/ConfigInitialParamsView.cpp
    src/ui/configuration/ConfigInitialParamsView.h
    src/ui/configuration/ConfigInitialParamsViewModel.cpp
    src/ui/configuration/ConfigInitialParamsViewModel.h
    src/ui/configuration/ConfigPlannerAdvView.cpp
    src/ui/configuration/ConfigPlannerAdvView.h
    src/ui/configuration/ConfigPlannerAdvViewModel.cpp
    src/ui/configuration/ConfigPlannerAdvViewModel.h
    src/ui/configuration/ConfigPlannerView.cpp
    src/ui/configuration/ConfigPlannerView.h
    src/ui/configuration/ConfigPlannerViewIntegration.cpp
    src/ui/configuration/ConfigPlannerViewIntegration.h
    src/ui/configuration/ConfigPlannerViewModel.cpp
    src/ui/configuration/ConfigPlannerViewModel.h
    src/ui/configuration/ConfigElevationSourcesView.cpp
    src/ui/configuration/ConfigElevationSourcesView.h
    src/ui/configuration/ConfigElevationSourcesViewModel.cpp
    src/ui/configuration/ConfigElevationSourcesViewModel.h
    src/ui/configuration/ConfigMavCommandView.cpp
    src/ui/configuration/ConfigMavCommandView.h
    src/ui/configuration/ConfigMavCommandViewModel.cpp
    src/ui/configuration/ConfigMavCommandViewModel.h
    src/ui/configuration/MavFTPUIView.cpp
    src/ui/configuration/MavFTPUIView.h
    src/ui/configuration/MavFtpFileDownload.cpp
    src/ui/configuration/MavFtpFileDownload.h
    src/ui/configuration/ElevationSourceService.cpp
    src/ui/configuration/ElevationSourceService.h
    src/ui/configuration/SrtmElevationSource.cpp
    src/ui/configuration/SrtmElevationSource.h
    src/ui/configuration/ConfigParamLoadingView.cpp
    src/ui/configuration/ConfigParamLoadingView.h
    src/ui/configuration/ConfigParamLoadingViewModel.cpp
    src/ui/configuration/ConfigParamLoadingViewModel.h
    src/ui/configuration/ArduCopterPidConfig.cc
    src/ui/configuration/ArduCopterPidConfig.h
    src/ui/configuration/ArduCopterPidConfig.ui
    src/ui/configuration/ArduPlanePidConfig.cc
    src/ui/configuration/ArduPlanePidConfig.h
    src/ui/configuration/ArduPlanePidConfig.ui
    src/ui/configuration/ArduRoverPidConfig.cc
    src/ui/configuration/ArduRoverPidConfig.h
    src/ui/configuration/ArduRoverPidConfig.ui
    src/ui/configuration/BasicPidConfig.cc
    src/ui/configuration/BasicPidConfig.h
    src/ui/configuration/BasicPidConfig.ui
    src/ui/configuration/BatteryMonitorConfig.cc
    src/ui/configuration/BatteryMonitorConfig.h
    src/ui/configuration/BatteryMonitorConfig.ui
    src/ui/configuration/BatteryMonitorInstanceModel.cpp
    src/ui/configuration/BatteryMonitorInstanceModel.h
    src/ui/configuration/ConfigBatteryMonitoring2View.cpp
    src/ui/configuration/ConfigBatteryMonitoring2View.h
    src/ui/configuration/ConfigBatteryMonitoring2ViewModel.cpp
    src/ui/configuration/ConfigBatteryMonitoring2ViewModel.h
    src/ui/configuration/CameraGimbalConfig.cc
    src/ui/configuration/CameraGimbalConfig.h
    src/ui/configuration/CameraGimbalConfig.ui
    src/ui/configuration/CompassConfig.cc
    src/ui/configuration/CompassConfig.h
    src/ui/configuration/CompassConfig.ui
    src/ui/configuration/CompassMotorCalibrationDialog.cpp
    src/ui/configuration/CompassMotorCalibrationDialog.h
    src/ui/configuration/CompassMotorCalibrationDialog.ui
    src/ui/configuration/CompassOnboardCalibrationDialog.cpp
    src/ui/configuration/CompassOnboardCalibrationDialog.h
    src/ui/configuration/CompassOnboardCalibrationDialog.ui
    src/ui/configuration/Console.cc
    src/ui/configuration/Console.h
    src/ui/configuration/CopterPidConfig.cc
    src/ui/configuration/CopterPidConfig.h
    src/ui/configuration/CopterPidConfig.ui
    src/ui/configuration/DownloadRemoteParamsDialog.cc
    src/ui/configuration/DownloadRemoteParamsDialog.h
    src/ui/configuration/DownloadRemoteParamsDialog.ui
    src/ui/configuration/FailSafeConfig.cc
    src/ui/configuration/FailSafeConfig.h
    src/ui/configuration/FailSafeConfig.ui
    src/ui/configuration/FlightModeConfig.cc
    src/ui/configuration/FlightModeConfig.h
    src/ui/configuration/FlightModeConfig.ui
    src/ui/configuration/FrameTypeConfig.cc
    src/ui/configuration/FrameTypeConfig.h
    src/ui/configuration/FrameTypeConfig.ui
    src/ui/configuration/FrameTypeConfigNew.ui
    src/ui/configuration/FrameTypeConfigOld.ui
    src/ui/configuration/GeoFenceConfig.cc
    src/ui/configuration/GeoFenceConfig.h
    src/ui/configuration/GeoFenceConfig.ui
    src/ui/configuration/LogConsole.cc
    src/ui/configuration/LogConsole.h
    src/ui/configuration/LogConsole.ui
    src/ui/configuration/OpticalFlowConfig.cc
    src/ui/configuration/OpticalFlowConfig.h
    src/ui/configuration/OpticalFlowConfig.ui
    src/ui/configuration/OsdConfig.cc
    src/ui/configuration/OsdConfig.h
    src/ui/configuration/OsdConfig.ui
    src/ui/configuration/PX4FirmwareUploader.cc
    src/ui/configuration/PX4FirmwareUploader.h
    src/ui/configuration/ParamCompareDialog.cpp
    src/ui/configuration/ParamCompareDialog.h
    src/ui/configuration/ParamCompareDialog.ui
    src/ui/configuration/ParamWidget.cc
    src/ui/configuration/ParamWidget.h
    src/ui/configuration/ParamWidget.ui
    src/ui/configuration/Radio3DRConfig.cc
    src/ui/configuration/Radio3DRConfig.h
    src/ui/configuration/Radio3DRConfig.ui
    src/ui/configuration/Radio3DRSettings.cc
    src/ui/configuration/Radio3DRSettings.h
    src/ui/configuration/RadioCalibrationConfig.cc
    src/ui/configuration/RadioCalibrationConfig.h
    src/ui/configuration/RadioCalibrationConfig.ui
    src/ui/configuration/RadioFlashWizard.cpp
    src/ui/configuration/RadioFlashWizard.h
    src/ui/configuration/RadioFlashWizard.ui
    src/ui/configuration/RangeFinderConfig.cc
    src/ui/configuration/RangeFinderConfig.h
    src/ui/configuration/RangeFinderConfig.ui
    src/ui/configuration/SerialSettingsDialog.cc
    src/ui/configuration/SerialSettingsDialog.h
    src/ui/configuration/SerialSettingsDialog.ui
    src/ui/configuration/StandardParamConfig.cc
    src/ui/configuration/StandardParamConfig.h
    src/ui/configuration/StandardParamConfig.ui
    src/ui/configuration/TerminalConsole.cc
    src/ui/configuration/TerminalConsole.h
    src/ui/configuration/TerminalConsole.ui
    src/ui/configuration/apmcustomfirmwareconfig.ui
    src/ui/dataselectionscreen.cpp
    src/ui/dataselectionscreen.h
    src/ui/dataselectionscreen.ui
    src/ui/designer/QGCActionButton.ui
    src/ui/designer/QGCComboBox.cc
    src/ui/designer/QGCComboBox.h
    src/ui/designer/QGCComboBox.ui
    src/ui/designer/QGCCommandButton.cc
    src/ui/designer/QGCCommandButton.h
    src/ui/designer/QGCCommandButton.ui
    src/ui/designer/QGCMouseWheelEventFilter.cc
    src/ui/designer/QGCMouseWheelEventFilter.h
    src/ui/designer/QGCParamSlider.cc
    src/ui/designer/QGCParamSlider.h
    src/ui/designer/QGCParamSlider.ui
    src/ui/designer/QGCRadioChannelDisplay.cpp
    src/ui/designer/QGCRadioChannelDisplay.h
    src/ui/designer/QGCTextLabel.cc
    src/ui/designer/QGCTextLabel.h
    src/ui/designer/QGCTextLabel.ui
    src/ui/designer/QGCToolWidget.cc
    src/ui/designer/QGCToolWidget.h
    src/ui/designer/QGCToolWidget.ui
    src/ui/designer/QGCToolWidgetItem.cc
    src/ui/designer/QGCToolWidgetItem.h
    src/ui/dockwidgettitlebareventfilter.cpp
    src/ui/dockwidgettitlebareventfilter.h
    src/ui/map/MAV2DIcon.cc
    src/ui/map/MAV2DIcon.h
    src/ui/map/QGCMapTool.cc
    src/ui/map/QGCMapTool.h
    src/ui/map/QGCMapTool.ui
    src/ui/map/QGCMapToolBar.cc
    src/ui/map/QGCMapToolBar.h
    src/ui/map/QGCMapToolBar.ui
    src/ui/map/AbstractMapWidget.h
    src/ui/map/CompiledMapBackends.cpp
    src/ui/map/CompiledMapBackends.h
    src/ui/map/MapCacheManager.cpp
    src/ui/map/MapCacheManager.h
    src/ui/map/MapCacheView.cpp
    src/ui/map/MapCacheView.h
    src/ui/map/MapPrefetchService.cpp
    src/ui/map/MapPrefetchService.h
    src/ui/map/MapWidgetFactory.cpp
    src/ui/map/MapWidgetFactory.h
    src/ui/map/MovingBaseMapMarkerItem.cpp
    src/ui/map/MovingBaseMapMarkerItem.h
    src/ui/map/OPMapBackendWidget.cpp
    src/ui/map/OPMapBackendWidget.h
    src/ui/map/MapTileSourceFactory.cpp
    src/ui/map/MapTileSourceFactory.h
    src/ui/map/NativeGdalMapService.cpp
    src/ui/map/NativeGdalMapService.h
    src/ui/map/QGCMapWidget.cc
    src/ui/map/QGCMapWidget.h
    src/ui/map/Waypoint2DIcon.cc
    src/ui/map/Waypoint2DIcon.h
    src/ui/mission/QGCMissionConditionDelay.cc
    src/ui/mission/QGCMissionConditionDelay.h
    src/ui/mission/QGCMissionConditionDelay.ui
    src/ui/mission/QGCMissionConditionDistance.cc
    src/ui/mission/QGCMissionConditionDistance.h
    src/ui/mission/QGCMissionConditionDistance.ui
    src/ui/mission/QGCMissionConditionYaw.cc
    src/ui/mission/QGCMissionConditionYaw.h
    src/ui/mission/QGCMissionConditionYaw.ui
    src/ui/mission/QGCMissionDoChangeSpeed.cc
    src/ui/mission/QGCMissionDoChangeSpeed.h
    src/ui/mission/QGCMissionDoChangeSpeed.ui
    src/ui/mission/QGCMissionDoDigicamControl.cc
    src/ui/mission/QGCMissionDoDigicamControl.h
    src/ui/mission/QGCMissionDoDigicamControl.ui
    src/ui/mission/QGCMissionDoFinishSearch.cc
    src/ui/mission/QGCMissionDoFinishSearch.h
    src/ui/mission/QGCMissionDoFinishSearch.ui
    src/ui/mission/QGCMissionDoJump.cc
    src/ui/mission/QGCMissionDoJump.h
    src/ui/mission/QGCMissionDoJump.ui
    src/ui/mission/QGCMissionDoMountControl.cc
    src/ui/mission/QGCMissionDoMountControl.h
    src/ui/mission/QGCMissionDoMountControl.ui
    src/ui/mission/QGCMissionDoRepeatRelay.cc
    src/ui/mission/QGCMissionDoRepeatRelay.h
    src/ui/mission/QGCMissionDoRepeatRelay.ui
    src/ui/mission/QGCMissionDoRepeatServo.cc
    src/ui/mission/QGCMissionDoRepeatServo.h
    src/ui/mission/QGCMissionDoRepeatServo.ui
    src/ui/mission/QGCMissionDoSetCamTriggDist.cc
    src/ui/mission/QGCMissionDoSetCamTriggDist.h
    src/ui/mission/QGCMissionDoSetCamTriggDist.ui
    src/ui/mission/QGCMissionDoSetHome.cc
    src/ui/mission/QGCMissionDoSetHome.h
    src/ui/mission/QGCMissionDoSetHome.ui
    src/ui/mission/QGCMissionDoSetROI.cc
    src/ui/mission/QGCMissionDoSetROI.h
    src/ui/mission/QGCMissionDoSetROI.ui
    src/ui/mission/QGCMissionDoSetRelay.cc
    src/ui/mission/QGCMissionDoSetRelay.h
    src/ui/mission/QGCMissionDoSetRelay.ui
    src/ui/mission/QGCMissionDoSetReverse.cc
    src/ui/mission/QGCMissionDoSetReverse.h
    src/ui/mission/QGCMissionDoSetReverse.ui
    src/ui/mission/QGCMissionDoSetServo.cc
    src/ui/mission/QGCMissionDoSetServo.h
    src/ui/mission/QGCMissionDoSetServo.ui
    src/ui/mission/QGCMissionDoStartSearch.cc
    src/ui/mission/QGCMissionDoStartSearch.h
    src/ui/mission/QGCMissionDoStartSearch.ui
    src/ui/mission/QGCMissionNavContinueChangeAlt.cc
    src/ui/mission/QGCMissionNavContinueChangeAlt.h
    src/ui/mission/QGCMissionNavContinueChangeAlt.ui
    src/ui/mission/QGCMissionNavLand.cc
    src/ui/mission/QGCMissionNavLand.h
    src/ui/mission/QGCMissionNavLand.ui
    src/ui/mission/QGCMissionNavLoiterTime.cc
    src/ui/mission/QGCMissionNavLoiterTime.h
    src/ui/mission/QGCMissionNavLoiterTime.ui
    src/ui/mission/QGCMissionNavLoiterToAlt.cc
    src/ui/mission/QGCMissionNavLoiterToAlt.h
    src/ui/mission/QGCMissionNavLoiterToAlt.ui
    src/ui/mission/QGCMissionNavLoiterTurns.cc
    src/ui/mission/QGCMissionNavLoiterTurns.h
    src/ui/mission/QGCMissionNavLoiterTurns.ui
    src/ui/mission/QGCMissionNavLoiterUnlim.cc
    src/ui/mission/QGCMissionNavLoiterUnlim.h
    src/ui/mission/QGCMissionNavLoiterUnlim.ui
    src/ui/mission/QGCMissionNavReturnToLaunch.cc
    src/ui/mission/QGCMissionNavReturnToLaunch.h
    src/ui/mission/QGCMissionNavReturnToLaunch.ui
    src/ui/mission/QGCMissionNavSweep.cc
    src/ui/mission/QGCMissionNavSweep.h
    src/ui/mission/QGCMissionNavSweep.ui
    src/ui/mission/QGCMissionNavTakeoff.cc
    src/ui/mission/QGCMissionNavTakeoff.h
    src/ui/mission/QGCMissionNavTakeoff.ui
    src/ui/mission/QGCMissionNavWaypoint.cc
    src/ui/mission/QGCMissionNavWaypoint.h
    src/ui/mission/QGCMissionNavWaypoint.ui
    src/ui/mission/QGCMissionOther.cc
    src/ui/mission/QGCMissionOther.h
    src/ui/mission/QGCMissionOther.ui
    src/ui/qcustomplot.cpp
    src/ui/qcustomplot.h
    src/ui/submainwindow.cpp
    src/ui/submainwindow.h
    src/ui/uas/PreFlightCalibrationDialog.cpp
    src/ui/uas/PreFlightCalibrationDialog.h
    src/ui/uas/PreFlightCalibrationDialog.ui
    src/ui/uas/QGCMessageView.cc
    src/ui/uas/QGCMessageView.h
    src/ui/uas/QGCMessageView.ui
    src/ui/uas/QGCUnconnectedInfoWidget.cc
    src/ui/uas/QGCUnconnectedInfoWidget.h
    src/ui/uas/QGCUnconnectedInfoWidget.ui
    src/ui/uas/UASActionsLayout.h
    src/ui/uas/UASActionCatalog.h
    src/ui/uas/UASActionsWidget.cpp
    src/ui/uas/UASActionsWidget.h
    src/ui/uas/UASActionsWidget.ui
    src/ui/uas/UASControl.ui
    src/ui/uas/UASControlWidget.cc
    src/ui/uas/UASControlWidget.h
    src/ui/uas/UASInfo.ui
    src/ui/uas/UASInfoWidget.cc
    src/ui/uas/UASInfoWidget.h
    src/ui/uas/UASList.ui
    src/ui/uas/UASListWidget.cc
    src/ui/uas/UASListWidget.h
    src/ui/uas/UASQuickView.cc
    src/ui/uas/UASQuickView.h
    src/ui/uas/UASQuickView.ui
    src/ui/uas/UASQuickViewGaugeItem.cc
    src/ui/uas/UASQuickViewGaugeItem.h
    src/ui/uas/UASQuickViewItem.cc
    src/ui/uas/UASQuickViewItem.h
    src/ui/uas/UASQuickViewItemSelect.cc
    src/ui/uas/UASQuickViewItemSelect.h
    src/ui/uas/UASQuickViewItemSelect.ui
    src/ui/uas/UASQuickViewTextItem.cc
    src/ui/uas/UASQuickViewTextItem.h
    src/ui/uas/UASQuickViewTextLabel.cc
    src/ui/uas/UASQuickViewTextLabel.h
    src/ui/uas/UASView.cc
    src/ui/uas/UASView.h
    src/ui/uas/UASView.ui
    src/ui/watchdog/WatchdogControl.cc
    src/ui/watchdog/WatchdogControl.h
    src/ui/watchdog/WatchdogControl.ui
    src/ui/watchdog/WatchdogProcessView.cc
    src/ui/watchdog/WatchdogProcessView.h
    src/ui/watchdog/WatchdogProcessView.ui
    src/ui/watchdog/WatchdogView.cc
    src/ui/watchdog/WatchdogView.h
    src/ui/watchdog/WatchdogView.ui
#    src/comm/QGCXPlaneLink.cc
#    src/comm/QGCXPlaneLink.h
#    src/ui/QGCHilXPlaneConfiguration.cc
#    src/ui/QGCHilXPlaneConfiguration.h
#    src/ui/QGCHilXPlaneConfiguration.ui
)

file(GLOB QML_FILES
    qml/*.qml
    qml/components/*.qml
    qml/resources/apmplanner/toolbar/*.png
    qml/VibrationMonitor.qml
    qml/EKFMonitor.qml
    qml/ApmToolBar.qml
)

# Platform-specific sources/defs/libs
if(WIN32)
    list(APPEND SOURCES
        # Add Windows-specific sources here
    )
    add_definitions(-D__STDC_LIMIT_MACROS)
    include_directories(libs/lib/msinttypes)
    set(ADDITIONAL_LIBS "")
elseif(APPLE)
    list(APPEND SOURCES
        # Add Mac-specific sources here
    )
    add_definitions(-D__STDC_LIMIT_MACROS)
    set(ADDITIONAL_LIBS "")
elseif(UNIX)
    add_definitions(-D__STDC_LIMIT_MACROS)
    set(ADDITIONAL_LIBS "")
endif()

# Optional: Google Earth (add logic to enable/disable as needed)
option(ENABLE_GOOGLE_EARTH "Enable Google Earth support" OFF)
if(ENABLE_GOOGLE_EARTH)
    list(APPEND HEADERS src/ui/map3D/QGCGoogleEarthView.h)
    list(APPEND SOURCES src/ui/map3D/QGCGoogleEarthView.cc)
    if(WIN32)
        add_definitions(-DGOOGLEEARTH)
    endif()
endif()

# Optional: Magellan 3DxWare support (Linux/Mac/Windows)
option(ENABLE_3DMOUSE "Enable 3DConnexion 3D mouse support" OFF)
if(ENABLE_3DMOUSE)
    if(UNIX AND EXISTS "/usr/local/lib/libxdrvlib.so")
        add_definitions(-DMOUSE_ENABLED_LINUX -DParameterCheck)
        include_directories(/usr/local/include)
        list(APPEND HEADERS src/input/Mouse6dofInput.h)
        list(APPEND SOURCES src/input/Mouse6dofInput.cpp)
        list(APPEND ADDITIONAL_LIBS xdrvlib)
    elseif(WIN32)
        add_definitions(-DMOUSE_ENABLED_WIN)
        include_directories(libs/thirdParty/3DMouse/win)
        list(APPEND HEADERS
            libs/thirdParty/3DMouse/win/I3dMouseParams.h
            libs/thirdParty/3DMouse/win/MouseParameters.h
            libs/thirdParty/3DMouse/win/Mouse3DInput.h
            src/input/Mouse6dofInput.h
        )
        list(APPEND SOURCES
            libs/thirdParty/3DMouse/win/MouseParameters.cpp
            libs/thirdParty/3DMouse/win/Mouse3DInput.cpp
            src/input/Mouse6dofInput.cpp
        )
    endif()
endif()

option(ENABLE_CAMERAVIEW "Enable Camera View support" OFF)
if(ENABLE_CAMERAVIEW)
    message(STATUS "Including support for Camera View")
    set(CAMERA_VIEW_SOURCES src/ui/CameraView.cc)
    set(CAMERA_VIEW_HEADERS src/ui/CameraView.h)
    add_definitions(-DENABLE_CAMRAVIEW)
else()
    message(STATUS "Skipping support for Camera View")
    set(CAMERA_VIEW_SOURCES "")
    set(CAMERA_VIEW_HEADERS "")
endif()

# Main target
add_executable(${APM_PLANNER_TARGET}
    ${SOURCES}
    ${HEADERS}
    ${FORMS}
    ${QML_FILES}
    ${CAMERA_VIEW_SOURCES}
)

# Includes
target_include_directories(${APM_PLANNER_TARGET} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include/ui
    ${MAVLINKPATH}
    ${MAVLINKPATH}/${MAVLINK_CONF}
    libs/utils libs libs/opmapcontrol
    ${SDL2_INCLUDE_DIR}
)
target_include_directories(${APM_PLANNER_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/Loghandling
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/linechart
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/uas
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
    ${CMAKE_CURRENT_SOURCE_DIR}/src/uas
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/qmapcontrol
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/mavlink
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/param
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/watchdog
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map3D
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/mission
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/designer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    ${CMAKE_CURRENT_SOURCE_DIR}/src/output
)

# Link Qt and additional libs
target_link_libraries(${APM_PLANNER_TARGET}
    Threads::Threads
    Qt::Widgets
    Qt::Concurrent
    Qt::Network
    Qt::Svg
    Qt::Xml
    Qt::Sql
    Qt::SerialPort
    Qt::Quick
    Qt::PrintSupport
    Qt::Qml
    Qt::QuickWidgets
    ${APM_DATA_VISUALIZATION_LIBRARY}
    ZLIB::ZLIB
    quazip
    alglib
    opmapcontrol
    apm_parameter_core
    apm_parameter_regeneration
    apm_mavlink_signing
    apm_mission_command_catalog
    ${ADDITIONAL_LIBS}
    ${APM_QT_AUDIO_LIBRARIES}
    ${APM_SDL2_TARGET}
)

# Definitions
target_compile_definitions(${APM_PLANNER_TARGET} PRIVATE
    DISABLE_3DMOUSE
    _TTY_NOWARN_
    $<$<CONFIG:Release>:QT_NO_DEBUG>
    QGC_USE_${MAVLINK_CONF}_MESSAGES
    NOMINMAX
    APM_INSTALL_DATA_DIR="${APM_INSTALL_RESOURCE_DIR}"
)

if(APM_ENABLE_SOURCE_DATA_FALLBACK)
    target_compile_definitions(${APM_PLANNER_TARGET} PRIVATE
        APM_DEVELOPMENT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

if(APM_USE_QML_PFD)
    target_compile_definitions(${APM_PLANNER_TARGET} PRIVATE PFD_QML=1)
endif()

if(APM_HAS_DATA_VISUALIZATION)
    target_compile_definitions(${APM_PLANNER_TARGET} PRIVATE APM_HAS_QT_DATA_VISUALIZATION)
endif()

# Platform-specific properties and post-build
if(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE "icon.icns")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum OS X deployment version")

    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/files/APMIcons/icon.icns"
        PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )

    set_target_properties(${APM_PLANNER_TARGET} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE ${MACOSX_BUNDLE_ICON_FILE}
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/files/macOS/Info.plist"
    )

    add_custom_command(TARGET ${APM_PLANNER_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../Resources/APMPlanner3/data"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/files"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../Resources/APMPlanner3/files"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/qml"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../Resources/APMPlanner3/qml"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/sik_uploader"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../Resources/APMPlanner3/sik_uploader"
    )

    # Ensure .icns is included in the app bundle for debug builds
    add_custom_command(TARGET ${APM_PLANNER_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/files/APMIcons/icon.icns"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../Resources/icon.icns"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/files/macOS/Info.plist"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/files/macOS/PkgInfo.in"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/../PkgInfo"

    )

    install(FILES "${CMAKE_SOURCE_DIR}/files/APMIcons/icon.icns"
            DESTINATION "APM Planner 3.0.app/Contents/Resources")

    configure_file("${CMAKE_SOURCE_DIR}/files/macOS/PkgInfo.in" ${CMAKE_BINARY_DIR}/PkgInfo @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/PkgInfo
        DESTINATION "APM Planner 3.0.app/Contents")


elseif(WIN32)
    add_custom_command(TARGET ${APM_PLANNER_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/files"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/files"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/qml"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/qml"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/sik_uploader"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/sik_uploader"
        # Add DLLs if needed, e.g. SDL2.dll, libxbee.dll, etc.
    )
else() # Linux
    add_custom_command(TARGET ${APM_PLANNER_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/files"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/files"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/qml"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/qml"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/sik_uploader"
            "$<TARGET_FILE_DIR:${APM_PLANNER_TARGET}>/sik_uploader"
    )
endif()

# Install rules for Linux packaging
if(UNIX AND NOT APPLE)
    install(TARGETS ${APM_PLANNER_TARGET} DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES ${CMAKE_SOURCE_DIR}/sik_uploader/sik_uploader.py
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/files
        DESTINATION "${APM_INSTALL_RESOURCE_SUBDIR}")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/data
        DESTINATION "${APM_INSTALL_RESOURCE_SUBDIR}")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/qml
        DESTINATION "${APM_INSTALL_RESOURCE_SUBDIR}")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/sik_uploader
        DESTINATION "${APM_INSTALL_RESOURCE_SUBDIR}")
    install(FILES ${CMAKE_SOURCE_DIR}/common/apmplanner3.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES ${CMAKE_SOURCE_DIR}/files/APMIcons/ap_rc.png
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps"
        RENAME apmplanner3.png)
endif()

if(BUILD_TESTING)
    # Run the complete SETUP factory audit inside the production executable so
    # it reuses the application's already-compiled object graph. The audit mode
    # reuses QGCCore's MainWindow, blocks page lifecycle side effects and exits
    # before the normal application event loop starts.
    target_sources(${APM_PLANNER_TARGET} PRIVATE
        tests/SetupRouteRuntimeAudit.cpp
        tests/SetupRouteRuntimeAudit.h
        tests/SigningTransportRuntimeAudit.cpp
        tests/SigningTransportRuntimeAudit.h
        tests/DeveloperVehicleToolRuntimeAudit.cpp
        tests/DeveloperVehicleToolRuntimeAudit.h
        tests/InspectorRuntimeAudit.cpp
        tests/InspectorRuntimeAudit.h
        tests/LogDownloadRuntimeAudit.cpp
        tests/LogDownloadRuntimeAudit.h
        tests/FirmwarePageLifetimeAudit.cpp
        tests/FirmwarePageLifetimeAudit.h
    )
    target_include_directories(${APM_PLANNER_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(${APM_PLANNER_TARGET} PRIVATE
        APM_SETUP_ROUTE_RUNTIME_AUDIT=1
    )
    add_custom_target(setupviewroutes_tests
        DEPENDS ${APM_PLANNER_TARGET}
    )
    add_test(NAME setupviewroutes_tests
        COMMAND $<TARGET_FILE:${APM_PLANNER_TARGET}> --setup-route-audit
    )
    set_tests_properties(setupviewroutes_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        RUN_SERIAL TRUE
        TIMEOUT 30
    )
    add_test(NAME signing_transport_runtime_tests
        COMMAND $<TARGET_FILE:${APM_PLANNER_TARGET}> --signing-transport-audit)
    set_tests_properties(signing_transport_runtime_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        RUN_SERIAL TRUE TIMEOUT 30)
    add_test(NAME developer_vehicle_runtime_tests
        COMMAND $<TARGET_FILE:${APM_PLANNER_TARGET}> --developer-vehicle-tools-audit)
    set_tests_properties(developer_vehicle_runtime_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        RUN_SERIAL TRUE TIMEOUT 30)

    # Explicit desktop diagnostic, not an automatic/noisy CTest. The default
    # invocation only probes readiness; --speak exercises actual synthesis.
    add_executable(speech_backend_probe
        tests/speech_backend_probe.cpp
        src/audio/QtAudioOutput.cpp
        src/audio/QtAudioOutput.h
        src/audio/QueuedSpeechController.cpp
        src/audio/QueuedSpeechController.h
    )
    target_include_directories(speech_backend_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(speech_backend_probe PRIVATE
        Qt::Core ${APM_QT_AUDIO_LIBRARIES})

    add_executable(queuedspeechcontroller_tests
        tests/test_queuedspeechcontroller.cpp
        src/audio/QueuedSpeechController.cpp
        src/audio/QueuedSpeechController.h
    )
    target_include_directories(queuedspeechcontroller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(queuedspeechcontroller_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME queuedspeechcontroller_tests
        COMMAND queuedspeechcontroller_tests)

    add_executable(speechsettings_tests
        tests/test_speechsettings.cpp
        src/services/SpeechSettings.cpp
        src/services/SpeechSettings.h
    )
    target_include_directories(speechsettings_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(speechsettings_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME speechsettings_tests
        COMMAND speechsettings_tests)

    add_executable(statusmessagesettings_tests
        tests/test_statusmessagesettings.cpp
        src/services/StatusMessageSettings.cpp
        src/services/StatusMessageSettings.h
        src/services/StatusTextPolicy.cpp
        src/services/StatusTextPolicy.h
        src/uas/APMFirmwareVersion.cpp
        src/uas/APMFirmwareVersion.h
    )
    target_include_directories(statusmessagesettings_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(statusmessagesettings_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME statusmessagesettings_tests
        COMMAND statusmessagesettings_tests)

    add_executable(speechannouncer_tests
        tests/test_speechannouncer.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/services/SpeechAnnouncer.cpp
        src/services/SpeechAnnouncer.h
        src/services/SpeechSettings.cpp
        src/services/SpeechSettings.h
        src/services/SpeechTelemetrySource.cpp
        src/services/SpeechTelemetrySource.h
        src/services/StatusMessageSettings.cpp
        src/services/StatusMessageSettings.h
        src/services/StatusTextPolicy.cpp
        src/services/StatusTextPolicy.h
        src/uas/APMFirmwareVersion.cpp
        src/uas/APMFirmwareVersion.h
        src/ui/configuration/BatteryMonitorInstanceModel.cpp
        src/ui/configuration/BatteryMonitorInstanceModel.h
    )
    target_include_directories(speechannouncer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/uas
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(speechannouncer_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME speechannouncer_tests
        COMMAND speechannouncer_tests)

    add_executable(speechtelemetrysource_tests
        tests/test_speechtelemetrysource.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/services/SpeechTelemetrySource.cpp
        src/services/SpeechTelemetrySource.h
        src/services/StatusTextPolicy.cpp
        src/services/StatusTextPolicy.h
        src/uas/APMFirmwareVersion.cpp
        src/uas/APMFirmwareVersion.h
    )
    target_include_directories(speechtelemetrysource_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(speechtelemetrysource_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME speechtelemetrysource_tests
        COMMAND speechtelemetrysource_tests)

    add_executable(applicationidentity_tests
        tests/test_applicationidentity.cpp
    )
    target_include_directories(applicationidentity_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(applicationidentity_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME applicationidentity_tests
        COMMAND applicationidentity_tests)

    add_executable(dronecanforwardingbroker_tests
        tests/test_dronecanforwardingbroker.cpp
        src/comm/DroneCanForwardingBroker.cpp
        src/comm/DroneCanForwardingBroker.h
    )
    target_include_directories(dronecanforwardingbroker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(dronecanforwardingbroker_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME dronecanforwardingbroker_tests
        COMMAND dronecanforwardingbroker_tests)

    add_executable(dronecangetnodeinfo_tests
        tests/test_dronecangetnodeinfo.cpp
        src/comm/DroneCanGetNodeInfoClient.cpp
        src/comm/DroneCanGetNodeInfoClient.h
    )
    target_include_directories(dronecangetnodeinfo_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(dronecangetnodeinfo_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME dronecangetnodeinfo_tests
        COMMAND dronecangetnodeinfo_tests)

    add_executable(dronecangetset_tests
        tests/test_dronecangetset.cpp
        src/comm/DroneCanGetSetClient.cpp
        src/comm/DroneCanGetSetClient.h
    )
    target_include_directories(dronecangetset_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(dronecangetset_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME dronecangetset_tests
        COMMAND dronecangetset_tests)

    add_executable(gpscorrectioncore_tests
        tests/test_gpscorrectioncore.cpp
        src/comm/GpsCorrectionSource.cpp
        src/comm/GpsCorrectionSource.h
        src/comm/GpsRtcmPacketizer.cpp
        src/comm/GpsRtcmPacketizer.h
        src/comm/Rtcm3Parser.cpp
        src/comm/Rtcm3Parser.h
    )
    target_include_directories(gpscorrectioncore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(gpscorrectioncore_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME gpscorrectioncore_tests
        COMMAND gpscorrectioncore_tests)

    add_executable(missionprotocolcoordinator_tests
        tests/test_missionprotocolcoordinator.cpp
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
    )
    target_include_directories(missionprotocolcoordinator_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(missionprotocolcoordinator_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME missionprotocolcoordinator_tests
        COMMAND missionprotocolcoordinator_tests)

    add_executable(missiontransferservice_tests
        tests/test_missiontransferservice.cpp
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
    )
    target_include_directories(missiontransferservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(missiontransferservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME missiontransferservice_tests
        COMMAND missiontransferservice_tests)

    add_executable(missiontransfercontroller_tests
        tests/test_missiontransfercontroller.cpp
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferController.cc
        src/comm/MissionTransferController.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/MissionTransferTransport.h
    )
    target_include_directories(missiontransfercontroller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(missiontransfercontroller_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME missiontransfercontroller_tests
        COMMAND missiontransfercontroller_tests)

    add_executable(tilecache_tests
        tests/test_tilecache.cpp
    )
    target_link_libraries(tilecache_tests PRIVATE
        opmapcontrol
        Qt::Core
        Qt::Network
        Qt::Test
    )
    add_test(NAME tilecache_tests COMMAND tilecache_tests)
    set_tests_properties(tilecache_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mapcachemanager_tests
        tests/test_mapcachemanager.cpp
        src/ui/map/MapCacheManager.cpp
        src/ui/map/MapCacheManager.h
    )
    target_include_directories(mapcachemanager_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(mapcachemanager_tests PRIVATE
        opmapcontrol
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Test
    )
    add_test(NAME mapcachemanager_tests COMMAND mapcachemanager_tests)
    set_tests_properties(mapcachemanager_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mapprefetchservice_tests
        tests/test_mapprefetchservice.cpp
        src/ui/map/MapPrefetchService.cpp
        src/ui/map/MapPrefetchService.h
    )
    target_include_directories(mapprefetchservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(mapprefetchservice_tests PRIVATE
        opmapcontrol
        Threads::Threads
        Qt::Core
        Qt::Network
        Qt::Test
    )
    add_test(NAME mapprefetchservice_tests COMMAND mapprefetchservice_tests)
    set_tests_properties(mapprefetchservice_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mapwidgetfactory_tests
        tests/test_mapwidgetfactory.cpp
        src/ui/map/AbstractMapWidget.h
        src/ui/map/MapWidgetFactory.cpp
        src/ui/map/MapWidgetFactory.h
    )
    target_include_directories(mapwidgetfactory_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(mapwidgetfactory_tests PRIVATE
        opmapcontrol
        Qt::Core
        Qt::Network
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME mapwidgetfactory_tests COMMAND mapwidgetfactory_tests)
    set_tests_properties(mapwidgetfactory_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mapcacheview_tests
        tests/test_mapcacheview.cpp
        src/ui/configuration/ElevationSourceService.cpp
        src/ui/configuration/ElevationSourceService.h
        src/ui/configuration/SrtmElevationSource.cpp
        src/ui/configuration/SrtmElevationSource.h
        src/ui/map/MapCacheManager.cpp
        src/ui/map/MapCacheManager.h
        src/ui/map/MapCacheView.cpp
        src/ui/map/MapCacheView.h
        src/ui/map/MapTileSourceFactory.cpp
        src/ui/map/MapTileSourceFactory.h
    )
    target_include_directories(mapcacheview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(mapcacheview_tests PRIVATE
        opmapcontrol
        quazip
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME mapcacheview_tests COMMAND mapcacheview_tests)
    set_tests_properties(mapcacheview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(opmapwidget_tests
        tests/test_opmapwidget.cpp
    )
    target_link_libraries(opmapwidget_tests PRIVATE
        opmapcontrol
        Qt::Core
        Qt::Network
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME opmapwidget_tests COMMAND opmapwidget_tests)
    set_tests_properties(opmapwidget_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(maptilesourcefactory_tests
        tests/test_maptilesourcefactory.cpp
        src/ui/configuration/ElevationSourceService.cpp
        src/ui/configuration/ElevationSourceService.h
        src/ui/configuration/SrtmElevationSource.cpp
        src/ui/configuration/SrtmElevationSource.h
        src/ui/map/MapTileSourceFactory.cpp
        src/ui/map/MapTileSourceFactory.h
        src/ui/map/NativeGdalMapService.cpp
        src/ui/map/NativeGdalMapService.h
    )
    target_include_directories(maptilesourcefactory_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(maptilesourcefactory_tests PRIVATE
        opmapcontrol
        quazip
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Test
    )
    add_test(NAME maptilesourcefactory_tests
        COMMAND maptilesourcefactory_tests)
    set_tests_properties(maptilesourcefactory_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(parameter_core_tests
        tests/test_parametercore.cpp
        tests/parameter_metadata_test.qrc
        src/core/parameters/ParameterMetaDataUpdater.cpp
        src/core/parameters/ParameterMetaDataUpdater.h
        src/uas/APMFirmwareVersion.cpp
        src/uas/APMFirmwareVersion.h
    )
    target_link_libraries(parameter_core_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Network
        Qt::Test
    )
    target_compile_definitions(parameter_core_tests PRIVATE
        APM_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME parameter_core_tests COMMAND parameter_core_tests)

    add_executable(parametermetadataparser_tests tests/test_parametermetadataparser.cpp)
    target_link_libraries(parametermetadataparser_tests PRIVATE apm_parameter_core Qt::Test)
    add_test(NAME parametermetadataparser_tests COMMAND parametermetadataparser_tests)

    add_executable(parametermetadataregenerationservice_tests
        tests/test_parametermetadataregenerationservice.cpp)
    target_link_libraries(parametermetadataregenerationservice_tests PRIVATE
        apm_parameter_regeneration Qt::Test)
    add_test(NAME parametermetadataregenerationservice_tests COMMAND parametermetadataregenerationservice_tests)
    set_tests_properties(parametermetadataregenerationservice_tests PROPERTIES TIMEOUT 60)

    add_executable(parameter_metadata_regeneration_probe tests/parameter_metadata_regeneration_probe.cpp)
    target_link_libraries(parameter_metadata_regeneration_probe PRIVATE apm_parameter_regeneration)

    add_executable(parametermetadataregenerationwindow_tests
        tests/test_parametermetadataregenerationwindow.cpp
        src/ui/configuration/ParameterMetaDataRegenerationWindow.cpp
        src/ui/configuration/ParameterMetaDataRegenerationWindow.h)
    target_link_libraries(parametermetadataregenerationwindow_tests PRIVATE
        apm_parameter_regeneration Qt::Widgets Qt::Test)
    add_test(NAME parametermetadataregenerationwindow_tests COMMAND parametermetadataregenerationwindow_tests)
    set_tests_properties(parametermetadataregenerationwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(qgcparamid_tests
        tests/test_qgcparamid.cpp
        src/comm/QGCParamID.cc
        src/comm/QGCParamID.h
    )
    target_include_directories(qgcparamid_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(qgcparamid_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME qgcparamid_tests COMMAND qgcparamid_tests)

    add_executable(configrouteprofile_tests
        tests/test_configrouteprofile.cpp
        src/ui/configuration/ConfigRouteProfile.cpp
        src/ui/configuration/ConfigRouteProfile.h
    )
    target_include_directories(configrouteprofile_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(configrouteprofile_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME configrouteprofile_tests COMMAND configrouteprofile_tests)

    add_executable(friendlyparams_tests
        tests/test_configfriendlyparamsview.cpp
        src/ui/configuration/ConfigFriendlyParamsView.cpp
        src/ui/configuration/ConfigFriendlyParamsView.h
        src/ui/configuration/ConfigFriendlyParamsViewModel.cpp
        src/ui/configuration/ConfigFriendlyParamsViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(friendlyparams_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(friendlyparams_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME friendlyparams_tests COMMAND friendlyparams_tests)
    set_tests_properties(friendlyparams_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configrawparams_tests
        tests/test_configrawparams.cpp
        src/ui/configuration/ConfigRawParams.cpp
        src/ui/configuration/ConfigRawParams.h
    )
    target_include_directories(configrawparams_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configrawparams_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configrawparams_tests COMMAND configrawparams_tests)
    set_tests_properties(configrawparams_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(framedefaultcatalogservice_tests
        tests/test_framedefaultcatalogservice.cpp
        src/ui/configuration/FrameDefaultCatalogService.cpp
        src/ui/configuration/FrameDefaultCatalogService.h
    )
    target_include_directories(framedefaultcatalogservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(framedefaultcatalogservice_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::Test
    )
    add_test(NAME framedefaultcatalogservice_tests
        COMMAND framedefaultcatalogservice_tests)

    add_executable(configdefaultsettingsview_tests
        tests/test_configdefaultsettingsview.cpp
        src/ui/configuration/ConfigDefaultSettingsView.cpp
        src/ui/configuration/ConfigDefaultSettingsView.h
        src/ui/configuration/ConfigRawParams.cpp
        src/ui/configuration/ConfigRawParams.h
        src/ui/configuration/FrameDefaultCatalogService.cpp
        src/ui/configuration/FrameDefaultCatalogService.h
    )
    target_include_directories(configdefaultsettingsview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configdefaultsettingsview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Network
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configdefaultsettingsview_tests
        COMMAND configdefaultsettingsview_tests)
    set_tests_properties(configdefaultsettingsview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(confighwidview_tests
        tests/test_confighwidview.cpp
        src/ui/configuration/ConfigHWIDView.cpp
        src/ui/configuration/ConfigHWIDView.h
        src/ui/configuration/ConfigHWIDViewModel.cpp
        src/ui/configuration/ConfigHWIDViewModel.h
        src/ui/configuration/DeveloperToolParsers.cpp
        src/ui/configuration/DeveloperToolParsers.h
    )
    target_include_directories(confighwidview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(confighwidview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME confighwidview_tests COMMAND confighwidview_tests)
    set_tests_properties(confighwidview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configadsbview_tests
        tests/test_configadsbview.cpp
        src/ui/configuration/ConfigADSBView.cpp
        src/ui/configuration/ConfigADSBView.h
        src/ui/configuration/ConfigADSBViewModel.cpp
        src/ui/configuration/ConfigADSBViewModel.h
        src/ui/configuration/ConfigFriendlyParamsView.cpp
        src/ui/configuration/ConfigFriendlyParamsView.h
        src/ui/configuration/ConfigFriendlyParamsViewModel.cpp
        src/ui/configuration/ConfigFriendlyParamsViewModel.h
        src/ui/configuration/ParamField.h
        src/comm/AdsbIdentificationClient.cpp
        src/comm/AdsbIdentificationClient.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(configadsbview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(configadsbview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configadsbview_tests COMMAND configadsbview_tests)
    set_tests_properties(configadsbview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configuserdefinedview_tests
        tests/test_configuserdefinedview.cpp
        src/ui/configuration/ConfigUserDefinedView.cpp
        src/ui/configuration/ConfigUserDefinedView.h
        src/ui/configuration/ConfigUserDefinedViewModel.cpp
        src/ui/configuration/ConfigUserDefinedViewModel.h
        src/ui/configuration/ConfigFriendlyParamsView.cpp
        src/ui/configuration/ConfigFriendlyParamsView.h
        src/ui/configuration/ConfigFriendlyParamsViewModel.cpp
        src/ui/configuration/ConfigFriendlyParamsViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configuserdefinedview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configuserdefinedview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configuserdefinedview_tests
        COMMAND configuserdefinedview_tests)
    set_tests_properties(configuserdefinedview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configplanneradvview_tests
        tests/test_configplanneradvview.cpp
        src/ui/configuration/ConfigPlannerAdvView.cpp
        src/ui/configuration/ConfigPlannerAdvView.h
        src/ui/configuration/ConfigPlannerAdvViewModel.cpp
        src/ui/configuration/ConfigPlannerAdvViewModel.h
    )
    target_include_directories(configplanneradvview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configplanneradvview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configplanneradvview_tests
        COMMAND configplanneradvview_tests)
    set_tests_properties(configplanneradvview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configplannerview_tests
        tests/test_configplannerview.cpp
        src/services/HudDisplaySettings.cpp
        src/services/HudDisplaySettings.h
        src/services/SpeechSettings.cpp
        src/services/SpeechSettings.h
        src/services/StatusMessageSettings.cpp
        src/services/StatusMessageSettings.h
        src/services/StatusTextPolicy.cpp
        src/services/StatusTextPolicy.h
        src/uas/APMFirmwareVersion.cpp
        src/uas/APMFirmwareVersion.h
        src/ui/configuration/ConfigPlannerView.cpp
        src/ui/configuration/ConfigPlannerView.h
        src/ui/configuration/ConfigPlannerViewModel.cpp
        src/ui/configuration/ConfigPlannerViewModel.h
        src/ui/configuration/DisplayViewProfile.cpp
        src/ui/configuration/DisplayViewProfile.h
        src/ui/configuration/PlannerStartupUdpOptions.cpp
        src/ui/configuration/PlannerStartupUdpOptions.h
    )
    target_include_directories(configplannerview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(configplannerview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configplannerview_tests
        COMMAND configplannerview_tests)
    set_tests_properties(configplannerview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(displayviewprofile_tests
        tests/test_displayviewprofile.cpp
        src/ui/configuration/DisplayViewProfile.cpp
        src/ui/configuration/DisplayViewProfile.h
    )
    target_include_directories(displayviewprofile_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(displayviewprofile_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME displayviewprofile_tests
        COMMAND displayviewprofile_tests)

    add_executable(plannerstartupudpoptions_tests
        tests/test_plannerstartupudpoptions.cpp
        src/ui/configuration/PlannerStartupUdpOptions.cpp
        src/ui/configuration/PlannerStartupUdpOptions.h
    )
    target_include_directories(plannerstartupudpoptions_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(plannerstartupudpoptions_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME plannerstartupudpoptions_tests
        COMMAND plannerstartupudpoptions_tests)

    add_executable(configelevationsourcesview_tests
        tests/test_configelevationsourcesview.cpp
        src/ui/configuration/ConfigElevationSourcesView.cpp
        src/ui/configuration/ConfigElevationSourcesView.h
        src/ui/configuration/ConfigElevationSourcesViewModel.cpp
        src/ui/configuration/ConfigElevationSourcesViewModel.h
        src/ui/configuration/ElevationSourceService.cpp
        src/ui/configuration/ElevationSourceService.h
        src/ui/configuration/SrtmElevationSource.cpp
        src/ui/configuration/SrtmElevationSource.h
    )
    target_include_directories(configelevationsourcesview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configelevationsourcesview_tests PRIVATE
        quazip
        Qt::Core
        Qt::Network
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configelevationsourcesview_tests
        COMMAND configelevationsourcesview_tests)
    set_tests_properties(configelevationsourcesview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configmavcommandview_tests
        tests/test_configmavcommandview.cpp
        src/ui/configuration/ConfigMavCommandView.cpp
        src/ui/configuration/ConfigMavCommandView.h
        src/ui/configuration/ConfigMavCommandViewModel.cpp
        src/ui/configuration/ConfigMavCommandViewModel.h
    )
    target_include_directories(configmavcommandview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configmavcommandview_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configmavcommandview_tests
        COMMAND configmavcommandview_tests)
    set_tests_properties(configmavcommandview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configadvancedview_tests
        tests/test_configadvancedview.cpp
        src/ui/configuration/ActionPageView.cpp
        src/ui/configuration/ActionPageView.h
        src/ui/configuration/ConfigAdvancedView.cpp
        src/ui/configuration/ConfigAdvancedView.h
    )
    target_include_directories(configadvancedview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(configadvancedview_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME configadvancedview_tests
        COMMAND configadvancedview_tests)
    set_tests_properties(configadvancedview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(externalguidedfile_tests
        tests/test_externalguidedfile.cpp
        src/ui/tools/ExternalGuidedFile.cpp
        src/ui/tools/ExternalGuidedFile.h
    )
    target_include_directories(externalguidedfile_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(externalguidedfile_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME externalguidedfile_tests
        COMMAND externalguidedfile_tests)

    add_executable(guidedtargetservice_tests
        tests/test_guidedtargetservice.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/GuidedTargetService.cpp
        src/comm/GuidedTargetService.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(guidedtargetservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(guidedtargetservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME guidedtargetservice_tests
        COMMAND guidedtargetservice_tests)

    add_executable(swarmtelemetryregistry_tests
        tests/test_swarmtelemetryregistry.cpp
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmtelemetryregistry_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmtelemetryregistry_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmtelemetryregistry_tests
        COMMAND swarmtelemetryregistry_tests)

    add_executable(exactmissionsnapshotservice_tests
        tests/test_exactmissionsnapshotservice.cpp
        src/comm/ExactMissionSnapshotService.cpp
        src/comm/ExactMissionSnapshotService.h
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(exactmissionsnapshotservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(exactmissionsnapshotservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME exactmissionsnapshotservice_tests
        COMMAND exactmissionsnapshotservice_tests)

    add_executable(swarmcommandservice_tests
        tests/test_swarmcommandservice.cpp
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmcommandservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmcommandservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmcommandservice_tests COMMAND swarmcommandservice_tests)

    add_executable(swarmformationcore_tests
        tests/test_swarmformationcore.cpp
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmformationcore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmformationcore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmformationcore_tests COMMAND swarmformationcore_tests)

    add_executable(formationgridcontrol_tests
        tests/test_formationgridcontrol.cpp
        src/ui/FormationGridControl.cpp
        src/ui/FormationGridControl.h
    )
    target_include_directories(formationgridcontrol_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(formationgridcontrol_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME formationgridcontrol_tests COMMAND formationgridcontrol_tests)
    set_tests_properties(formationgridcontrol_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmformationwindow_tests
        tests/test_swarmformationwindow.cpp
        src/ui/SwarmFormationWindow.cpp
        src/ui/SwarmFormationWindow.h
        src/ui/FormationGridControl.cpp
        src/ui/FormationGridControl.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmformationwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmformationwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME swarmformationwindow_tests
        COMMAND swarmformationwindow_tests)
    set_tests_properties(swarmformationwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmfollowpathcore_tests
        tests/test_swarmfollowpathcore.cpp
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmFlightMode.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmfollowpathcore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmfollowpathcore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmfollowpathcore_tests
        COMMAND swarmfollowpathcore_tests)

    add_executable(swarmfollowpathwindow_tests
        tests/test_swarmfollowpathwindow.cpp
        src/ui/SwarmFollowPathWindow.cpp
        src/ui/SwarmFollowPathWindow.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmfollowpathwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmfollowpathwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME swarmfollowpathwindow_tests
        COMMAND swarmfollowpathwindow_tests)
    set_tests_properties(swarmfollowpathwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmfollowleadercore_tests
        tests/test_swarmfollowleadercore.cpp
        src/ui/tools/SwarmFollowLeaderCore.cpp
        src/ui/tools/SwarmFollowLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmFlightMode.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmfollowleadercore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmfollowleadercore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmfollowleadercore_tests
        COMMAND swarmfollowleadercore_tests)

    add_executable(swarmwaypointleadercore_tests
        tests/test_swarmwaypointleadercore.cpp
        src/ui/tools/SwarmWaypointLeaderCore.cpp
        src/ui/tools/SwarmWaypointLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmFlightMode.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmwaypointleadercore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmwaypointleadercore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmwaypointleadercore_tests
        COMMAND swarmwaypointleadercore_tests)

    add_executable(swarmwaypointleaderexecutor_tests
        tests/test_swarmwaypointleaderexecutor.cpp
        src/services/SwarmWaypointLeaderExecutor.cpp
        src/services/SwarmWaypointLeaderExecutor.h
        src/ui/tools/SwarmWaypointLeaderCore.cpp
        src/ui/tools/SwarmWaypointLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/ExactMissionSnapshotService.cpp
        src/comm/ExactMissionSnapshotService.h
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/ParameterService.cpp
        src/comm/ParameterService.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/uas/QGCUASParamManager.cc
        src/uas/QGCUASParamManager.h
    )
    target_include_directories(swarmwaypointleaderexecutor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmwaypointleaderexecutor_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmwaypointleaderexecutor_tests
        COMMAND swarmwaypointleaderexecutor_tests)

    add_executable(swarmwaypointleaderwindowadapter_tests
        tests/test_swarmwaypointleaderwindowadapter.cpp
        src/ui/SwarmWaypointLeaderWindowAdapter.cpp
        src/ui/SwarmWaypointLeaderWindowAdapter.h
        src/services/SwarmWaypointLeaderExecutor.cpp
        src/services/SwarmWaypointLeaderExecutor.h
        src/ui/tools/SwarmWaypointLeaderCore.cpp
        src/ui/tools/SwarmWaypointLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/ExactMissionSnapshotService.cpp
        src/comm/ExactMissionSnapshotService.h
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/ParameterService.cpp
        src/comm/ParameterService.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/uas/QGCUASParamManager.cc
        src/uas/QGCUASParamManager.h
    )
    target_include_directories(swarmwaypointleaderwindowadapter_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmwaypointleaderwindowadapter_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME swarmwaypointleaderwindowadapter_tests
        COMMAND swarmwaypointleaderwindowadapter_tests)

    add_executable(waypointleaderprofilecontrol_tests
        tests/test_waypointleaderprofilecontrol.cpp
        src/ui/WaypointLeaderProfileControl.cpp
        src/ui/WaypointLeaderProfileControl.h
        src/ui/tools/SwarmWaypointLeaderCore.cpp
        src/ui/tools/SwarmWaypointLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmFlightMode.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(waypointleaderprofilecontrol_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(waypointleaderprofilecontrol_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME waypointleaderprofilecontrol_tests
        COMMAND waypointleaderprofilecontrol_tests)
    set_tests_properties(waypointleaderprofilecontrol_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmwaypointleaderwindow_tests
        tests/test_swarmwaypointleaderwindow.cpp
        src/ui/SwarmWaypointLeaderWindow.cpp
        src/ui/SwarmWaypointLeaderWindow.h
        src/ui/WaypointLeaderProfileControl.cpp
        src/ui/WaypointLeaderProfileControl.h
        src/ui/tools/SwarmWaypointLeaderCore.cpp
        src/ui/tools/SwarmWaypointLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmFlightMode.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmwaypointleaderwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmwaypointleaderwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME swarmwaypointleaderwindow_tests
        COMMAND swarmwaypointleaderwindow_tests)
    set_tests_properties(swarmwaypointleaderwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmfollowleaderwindow_tests
        tests/test_swarmfollowleaderwindow.cpp
        src/ui/SwarmFollowLeaderWindow.cpp
        src/ui/SwarmFollowLeaderWindow.h
        src/ui/tools/SwarmFollowLeaderCore.cpp
        src/ui/tools/SwarmFollowLeaderCore.h
        src/ui/tools/SwarmFollowPathCore.cpp
        src/ui/tools/SwarmFollowPathCore.h
        src/ui/tools/SwarmFormationCore.cpp
        src/ui/tools/SwarmFormationCore.h
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmfollowleaderwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmfollowleaderwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME swarmfollowleaderwindow_tests
        COMMAND swarmfollowleaderwindow_tests)
    set_tests_properties(swarmfollowleaderwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmsequencecore_tests
        tests/test_swarmsequencecore.cpp
        src/ui/tools/SwarmSequenceCore.cpp
        src/ui/tools/SwarmSequenceCore.h
    )
    target_include_directories(swarmsequencecore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(swarmsequencecore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmsequencecore_tests COMMAND swarmsequencecore_tests)

    add_executable(swarmsequenceexecutor_tests
        tests/test_swarmsequenceexecutor.cpp
        src/services/SwarmSequenceExecutor.cpp
        src/services/SwarmSequenceExecutor.h
        src/ui/tools/SwarmSequenceCore.cpp
        src/ui/tools/SwarmSequenceCore.h
        src/comm/SwarmCommandService.cpp
        src/comm/SwarmCommandService.h
        src/comm/SwarmFlightMode.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(swarmsequenceexecutor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmsequenceexecutor_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME swarmsequenceexecutor_tests
        COMMAND swarmsequenceexecutor_tests)

    add_executable(sequencelayoutcontrol_tests
        tests/test_sequencelayoutcontrol.cpp
        src/ui/SequenceLayoutControl.cpp
        src/ui/SequenceLayoutControl.h
    )
    target_include_directories(sequencelayoutcontrol_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(sequencelayoutcontrol_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME sequencelayoutcontrol_tests
        COMMAND sequencelayoutcontrol_tests)
    set_tests_properties(sequencelayoutcontrol_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(swarmsequencewindow_tests
        tests/test_swarmsequencewindow.cpp
        src/ui/SwarmSequenceWindow.cpp
        src/ui/SwarmSequenceWindow.h
        src/ui/SequenceLayoutControl.cpp
        src/ui/SequenceLayoutControl.h
        src/ui/tools/SwarmSequenceCore.cpp
        src/ui/tools/SwarmSequenceCore.h
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/SwarmTelemetryRegistry.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(swarmsequencewindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(swarmsequencewindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME swarmsequencewindow_tests
        COMMAND swarmsequencewindow_tests)
    set_tests_properties(swarmsequencewindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(nmeaggaparser_tests
        tests/test_nmeaggaparser.cpp
        src/comm/NmeaGgaParser.cpp
        src/comm/NmeaGgaParser.h
    )
    target_include_directories(nmeaggaparser_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(nmeaggaparser_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME nmeaggaparser_tests COMMAND nmeaggaparser_tests)

    add_executable(nmealineframer_tests
        tests/test_nmealineframer.cpp
        src/comm/NmeaLineFramer.cpp
        src/comm/NmeaLineFramer.h
    )
    target_include_directories(nmealineframer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(nmealineframer_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME nmealineframer_tests COMMAND nmealineframer_tests)

    add_executable(followmegpsinput_tests
        tests/test_followmegpsinput.cpp
        src/comm/FollowMeGpsInput.cpp
        src/comm/FollowMeGpsInput.h
        src/comm/NmeaGgaParser.cpp
        src/comm/NmeaGgaParser.h
        src/comm/NmeaLineFramer.cpp
        src/comm/NmeaLineFramer.h
    )
    target_include_directories(followmegpsinput_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(followmegpsinput_tests PRIVATE
        Qt::Core
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME followmegpsinput_tests COMMAND followmegpsinput_tests)

    add_executable(followmewindow_tests
        tests/test_followmewindow.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/FollowMeGpsInput.cpp
        src/comm/FollowMeGpsInput.h
        src/comm/GuidedTargetService.cpp
        src/comm/GuidedTargetService.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/NmeaGgaParser.cpp
        src/comm/NmeaGgaParser.h
        src/comm/NmeaLineFramer.cpp
        src/comm/NmeaLineFramer.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/FollowMeWindow.cpp
        src/ui/FollowMeWindow.h
    )
    target_include_directories(followmewindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(followmewindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::SerialPort
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME followmewindow_tests COMMAND followmewindow_tests)
    set_tests_properties(followmewindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(movingbasepositionstore_tests
        tests/test_movingbasepositionstore.cpp
        src/comm/MovingBasePositionStore.cpp
        src/comm/MovingBasePositionStore.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(movingbasepositionstore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(movingbasepositionstore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME movingbasepositionstore_tests
        COMMAND movingbasepositionstore_tests)

    add_executable(movingbasemapmarker_tests
        tests/test_movingbasemapmarker.cpp
        src/ui/map/MovingBaseMapMarkerItem.cpp
        src/ui/map/MovingBaseMapMarkerItem.h
    )
    target_include_directories(movingbasemapmarker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/map
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(movingbasemapmarker_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME movingbasemapmarker_tests COMMAND movingbasemapmarker_tests)
    set_tests_properties(movingbasemapmarker_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(movingbaseinputtransport_tests
        tests/test_movingbaseinputtransport.cpp
        src/comm/MovingBaseInputTransport.cpp
        src/comm/MovingBaseInputTransport.h
        src/comm/NmeaGgaParser.cpp
        src/comm/NmeaGgaParser.h
        src/comm/NmeaLineFramer.cpp
        src/comm/NmeaLineFramer.h
    )
    target_include_directories(movingbaseinputtransport_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(movingbaseinputtransport_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME movingbaseinputtransport_tests
        COMMAND movingbaseinputtransport_tests)

    add_executable(movingbasenmealog_tests
        tests/test_movingbasenmealog.cpp
        src/comm/MovingBaseNmeaLog.cpp
        src/comm/MovingBaseNmeaLog.h
    )
    target_include_directories(movingbasenmealog_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(movingbasenmealog_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME movingbasenmealog_tests COMMAND movingbasenmealog_tests)

    add_executable(movingbaseservice_tests
        tests/test_movingbaseservice.cpp
        src/comm/MovingBasePositionStore.cpp
        src/comm/MovingBasePositionStore.h
        src/comm/MovingBaseService.cpp
        src/comm/MovingBaseService.h
        src/comm/NmeaGgaParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(movingbaseservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(movingbaseservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME movingbaseservice_tests COMMAND movingbaseservice_tests)

    add_executable(movingbasemapcontroller_tests
        tests/test_movingbasemapcontroller.cpp
        src/comm/MovingBasePositionStore.cpp
        src/comm/MovingBasePositionStore.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/flightdata/MovingBaseMapController.cpp
        src/ui/flightdata/MovingBaseMapController.h
        src/ui/map/AbstractMapWidget.h
    )
    target_include_directories(movingbasemapcontroller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(movingbasemapcontroller_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME movingbasemapcontroller_tests
        COMMAND movingbasemapcontroller_tests)

    add_executable(movingbasewindow_tests
        tests/test_movingbasewindow.cpp
        src/comm/MovingBaseInputTransport.cpp
        src/comm/MovingBaseInputTransport.h
        src/comm/MovingBaseNmeaLog.cpp
        src/comm/MovingBaseNmeaLog.h
        src/comm/MovingBasePositionStore.cpp
        src/comm/MovingBasePositionStore.h
        src/comm/MovingBaseService.cpp
        src/comm/MovingBaseService.h
        src/comm/NmeaGgaParser.cpp
        src/comm/NmeaGgaParser.h
        src/comm/NmeaLineFramer.cpp
        src/comm/NmeaLineFramer.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/MovingBaseWindow.cpp
        src/ui/MovingBaseWindow.h
    )
    target_include_directories(movingbasewindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(movingbasewindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::SerialPort
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME movingbasewindow_tests COMMAND movingbasewindow_tests)
    set_tests_properties(movingbasewindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(externalguidedwindow_tests
        tests/test_externalguidedwindow.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/GuidedTargetService.cpp
        src/comm/GuidedTargetService.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/ExternalGuidedWindow.cpp
        src/ui/ExternalGuidedWindow.h
        src/ui/tools/ExternalGuidedFile.cpp
        src/ui/tools/ExternalGuidedFile.h
    )
    target_include_directories(externalguidedwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(externalguidedwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME externalguidedwindow_tests
        COMMAND externalguidedwindow_tests)
    set_tests_properties(externalguidedwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    set(LOG_DOWNLOAD_CORE_SOURCES
        src/comm/MAVLinkFrameParser.cpp
        src/comm/LogDownloadTracker.cpp
        src/comm/ExactLogTransferService.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/VehicleEndpoint.cpp
    )
    set(DATAFLASH_KML_SOURCES
        tests/LogDownloadTestLogging.cpp
        src/ui/Loghandling/DataFlashKmlExporter.cpp
        src/ui/Loghandling/BinLogParser.cpp
        src/ui/Loghandling/AsciiLogParser.cpp
        src/ui/Loghandling/LogParserBase.cpp
        src/ui/Loghandling/AP2DataPlotStatus.cpp
    )
    add_executable(logdownloadtracker_tests
        tests/test_logdownloadtracker.cpp src/comm/LogDownloadTracker.cpp)
    add_executable(udplinkpeers_tests
        tests/test_udplinkpeers.cpp src/comm/UdpPeerState.cpp)
    target_link_libraries(udplinkpeers_tests PRIVATE Qt::Network)
    add_executable(exactlogtransferservice_tests
        tests/test_exactlogtransferservice.cpp ${LOG_DOWNLOAD_CORE_SOURCES})
    add_executable(dataflashkmlexporter_tests
        tests/test_dataflashkmlexporter.cpp ${DATAFLASH_KML_SOURCES})
    add_executable(logdownloadviewmodel_tests
        tests/test_logdownloadviewmodel.cpp src/ui/LogDownloadViewModel.cpp
        ${LOG_DOWNLOAD_CORE_SOURCES} ${DATAFLASH_KML_SOURCES})
    add_executable(logdownloadwindow_tests
        tests/test_logdownloadwindow.cpp src/ui/LogDownloadWindow.cpp
        src/ui/LogDownloadViewModel.cpp
        ${LOG_DOWNLOAD_CORE_SOURCES} ${DATAFLASH_KML_SOURCES})
    foreach(log_test IN ITEMS logdownloadtracker_tests udplinkpeers_tests
            exactlogtransferservice_tests dataflashkmlexporter_tests
            logdownloadviewmodel_tests logdownloadwindow_tests)
        target_include_directories(${log_test} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/Loghandling
            ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
        target_link_libraries(${log_test} PRIVATE Qt::Core Qt::Widgets Qt::Test)
        add_test(NAME ${log_test} COMMAND ${log_test})
        set_tests_properties(${log_test} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()

    add_library(fft_analysis_test_support STATIC
        src/ui/Loghandling/DataFlashFftCore.cpp
        src/ui/Loghandling/DataFlashFftAnalyzer.cpp
        src/ui/Loghandling/DataFlashFftService.cpp
        src/ui/Loghandling/BinLogParser.cpp
        src/ui/Loghandling/AsciiLogParser.cpp
        src/ui/Loghandling/LogParserBase.cpp
        src/ui/Loghandling/AP2DataPlotStatus.cpp)
    target_include_directories(fft_analysis_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/Loghandling
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(fft_analysis_test_support PUBLIC Qt::Widgets)
    add_library(fft_view_test_support STATIC
        src/ui/configuration/ConfigFFTView.cpp
        src/ui/configuration/ConfigFFTViewModel.cpp
        src/ui/ConfigFFTWindow.cpp
        src/ui/qcustomplot.cpp)
    target_include_directories(fft_view_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration)
    target_link_libraries(fft_view_test_support PUBLIC
        fft_analysis_test_support apm_parameter_core Qt::PrintSupport)
    foreach(fft_part IN ITEMS core analyzer service)
        add_executable(dataflashfft${fft_part}_tests
            tests/test_dataflashfft${fft_part}.cpp)
        target_link_libraries(dataflashfft${fft_part}_tests PRIVATE
            fft_analysis_test_support Qt::Test)
    endforeach()
    foreach(fft_part IN ITEMS view viewmodel)
        add_executable(configfft${fft_part}_tests
            tests/test_configfft${fft_part}.cpp tests/LogDownloadTestLogging.cpp)
        target_link_libraries(configfft${fft_part}_tests PRIVATE
            fft_view_test_support Qt::Test)
    endforeach()
    add_executable(configfftintegration_tests
        tests/test_configfftintegration.cpp tests/LogDownloadTestLogging.cpp
        src/ui/configuration/ConfigFFTIntegration.cpp
        src/comm/ParameterService.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/MAVLinkFrameParser.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleTargetManager.cpp
        src/comm/SwarmTelemetryRegistry.cpp)
    target_link_libraries(configfftintegration_tests PRIVATE
        fft_view_test_support Qt::Test)
    add_executable(parameterservicesinglevehicle_tests
        tests/test_parameterservicesinglevehicle.cpp
        src/comm/ParameterService.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/MAVLinkFrameParser.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleTargetManager.cpp
        src/comm/SwarmTelemetryRegistry.cpp)
    add_library(px4flow_test_support STATIC
        src/comm/ParameterService.cpp
        src/comm/SwarmTelemetryRegistry.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/MAVLinkFrameParser.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleTargetManager.cpp
        src/comm/MavlinkComponentRegistry.cpp
        src/comm/Px4FlowFrameAssembler.cpp
        src/comm/Px4FlowService.cpp
        src/ui/configuration/ConfigPX4FlowView.cpp)
    target_include_directories(px4flow_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(px4flow_test_support PUBLIC
        apm_parameter_core Qt::Gui Qt::Widgets)
    foreach(px4flow_test IN ITEMS px4flowframeassembler mavlinkcomponentregistry
            parametercomponenttransactions px4flowservice configpx4flowview)
        add_executable(${px4flow_test}_tests tests/test_${px4flow_test}.cpp)
        target_link_libraries(${px4flow_test}_tests PRIVATE px4flow_test_support Qt::Test)
        add_test(NAME ${px4flow_test}_tests COMMAND ${px4flow_test}_tests)
        set_tests_properties(${px4flow_test}_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endforeach()
    target_include_directories(parameterservicesinglevehicle_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(parameterservicesinglevehicle_tests PRIVATE
        apm_parameter_core Qt::Test)
    foreach(fft_test IN ITEMS dataflashfftcore_tests dataflashfftanalyzer_tests
            dataflashfftservice_tests configfftview_tests configfftviewmodel_tests
            configfftintegration_tests parameterservicesinglevehicle_tests)
        add_test(NAME ${fft_test} COMMAND ${fft_test})
        set_tests_properties(${fft_test} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()

    add_executable(dataflashspectrogramanalyzer_tests
        tests/test_dataflashspectrogramanalyzer.cpp
        src/ui/Loghandling/DataFlashSpectrogramAnalyzer.cpp
        src/ui/Loghandling/DataFlashSpectrogramAnalyzer.h
    )
    target_include_directories(dataflashspectrogramanalyzer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(dataflashspectrogramanalyzer_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
    )
    add_test(NAME dataflashspectrogramanalyzer_tests
        COMMAND dataflashspectrogramanalyzer_tests)

    add_executable(dataflashspectrogramservice_tests
        tests/test_dataflashspectrogramservice.cpp
        src/ui/Loghandling/AP2DataPlotStatus.cpp
        src/ui/Loghandling/AP2DataPlotStatus.h
        src/ui/Loghandling/AsciiLogParser.cpp
        src/ui/Loghandling/AsciiLogParser.h
        src/ui/Loghandling/BinLogParser.cpp
        src/ui/Loghandling/BinLogParser.h
        src/ui/Loghandling/DataFlashSpectrogramAnalyzer.cpp
        src/ui/Loghandling/DataFlashSpectrogramAnalyzer.h
        src/ui/Loghandling/DataFlashSpectrogramService.cpp
        src/ui/Loghandling/DataFlashSpectrogramService.h
        src/ui/Loghandling/ILogParser.h
        src/ui/Loghandling/ILogdataSink.h
        src/ui/Loghandling/IParserCallback.h
        src/ui/Loghandling/LogParserBase.cpp
        src/ui/Loghandling/LogParserBase.h
    )
    target_include_directories(dataflashspectrogramservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/Loghandling
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(dataflashspectrogramservice_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME dataflashspectrogramservice_tests
        COMMAND dataflashspectrogramservice_tests)
    set_tests_properties(dataflashspectrogramservice_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(spectrogramwindow_tests
        tests/test_spectrogramwindow.cpp
        src/ui/SpectrogramWindow.cpp
        src/ui/SpectrogramWindow.h
    )
    target_include_directories(spectrogramwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(spectrogramwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME spectrogramwindow_tests
        COMMAND spectrogramwindow_tests)
    set_tests_properties(spectrogramwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(terrain3dcore_tests
        tests/test_terrain3dcore.cpp
        src/ui/terrain/Terrain3DCore.cpp
        src/ui/terrain/Terrain3DCore.h
    )
    target_include_directories(terrain3dcore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(terrain3dcore_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
    )
    add_test(NAME terrain3dcore_tests COMMAND terrain3dcore_tests)

    add_executable(terrain3dtelemetrysource_tests
        tests/test_terrain3dtelemetrysource.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/terrain/Terrain3DTelemetrySource.cpp
        src/ui/terrain/Terrain3DTelemetrySource.h
    )
    target_include_directories(terrain3dtelemetrysource_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(terrain3dtelemetrysource_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME terrain3dtelemetrysource_tests
             COMMAND terrain3dtelemetrysource_tests)

    add_executable(propagationcore_tests
        tests/test_propagationcore.cpp
        src/ui/tools/PropagationCore.cpp
        src/ui/tools/PropagationCore.h
    )
    target_include_directories(propagationcore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(propagationcore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME propagationcore_tests COMMAND propagationcore_tests)

    add_executable(propagationdistanceestimator_tests
        tests/test_propagationdistanceestimator.cpp
        src/ui/tools/PropagationDistanceEstimator.cpp
        src/ui/tools/PropagationDistanceEstimator.h
    )
    target_include_directories(propagationdistanceestimator_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(propagationdistanceestimator_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME propagationdistanceestimator_tests
             COMMAND propagationdistanceestimator_tests)

    add_executable(propagationtelemetrysource_tests
        tests/test_propagationtelemetrysource.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/tools/PropagationDistanceEstimator.cpp
        src/ui/tools/PropagationDistanceEstimator.h
        src/ui/tools/PropagationTelemetrySource.cpp
        src/ui/tools/PropagationTelemetrySource.h
    )
    target_include_directories(propagationtelemetrysource_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(propagationtelemetrysource_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME propagationtelemetrysource_tests
             COMMAND propagationtelemetrysource_tests)

    add_executable(propagationoverlaycontroller_tests
        tests/test_propagationoverlaycontroller.cpp
        src/ui/map/AbstractMapWidget.h
        src/ui/tools/PropagationCore.cpp
        src/ui/tools/PropagationCore.h
        src/ui/tools/PropagationOverlayController.cpp
        src/ui/tools/PropagationOverlayController.h
        src/ui/tools/PropagationSettingsStore.cpp
        src/ui/tools/PropagationSettingsStore.h
    )
    target_include_directories(propagationoverlaycontroller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(propagationoverlaycontroller_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME propagationoverlaycontroller_tests
             COMMAND propagationoverlaycontroller_tests)
    set_tests_properties(propagationoverlaycontroller_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(propagationsettingswindow_tests
        tests/test_propagationsettingswindow.cpp
        src/ui/tools/PropagationSettingsStore.cpp
        src/ui/tools/PropagationSettingsStore.h
        src/ui/tools/PropagationSettingsWindow.cpp
        src/ui/tools/PropagationSettingsWindow.h
    )
    target_include_directories(propagationsettingswindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(propagationsettingswindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME propagationsettingswindow_tests
             COMMAND propagationsettingswindow_tests)
    set_tests_properties(propagationsettingswindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(terrain3dwindow_tests
        tests/test_terrain3dwindow.cpp
        src/ui/Terrain3DWindow.cpp
        src/ui/Terrain3DWindow.h
        src/ui/terrain/Terrain3DCore.cpp
        src/ui/terrain/Terrain3DCore.h
    )
    target_include_directories(terrain3dwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(terrain3dwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME terrain3dwindow_tests COMMAND terrain3dwindow_tests)
    set_tests_properties(terrain3dwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(developertoolparsers_tests
        tests/test_developertoolparsers.cpp
        src/ui/configuration/DeveloperToolParsers.cpp
        src/ui/configuration/DeveloperToolParsers.h
    )
    target_include_directories(developertoolparsers_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(developertoolparsers_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME developertoolparsers_tests
        COMMAND developertoolparsers_tests)

    add_library(developer_vehicle_tool_test_support STATIC
        src/services/DeveloperVehicleToolService.cpp
        src/services/DeveloperVehicleToolService.h
        src/comm/ParameterService.cpp
        src/comm/VehicleCommandService.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/MAVLinkFrameParser.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleTargetManager.cpp
        src/comm/SwarmTelemetryRegistry.cpp)
    target_include_directories(developer_vehicle_tool_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(developer_vehicle_tool_test_support PUBLIC
        apm_parameter_core Qt::Core)
    add_executable(developervehicletoolservice_tests tests/test_developervehicletoolservice.cpp)
    target_link_libraries(developervehicletoolservice_tests PRIVATE
        developer_vehicle_tool_test_support Qt::Test)
    add_test(NAME developervehicletoolservice_tests COMMAND developervehicletoolservice_tests)
    set_tests_properties(developervehicletoolservice_tests PROPERTIES TIMEOUT 60)

    add_library(gps_correction_test_support STATIC
        src/comm/GpsCorrectionExtractor.cpp
        src/comm/GpsCorrectionExtractor.h
        src/comm/TlogReader.cpp
        src/comm/MAVLinkFrameParser.cpp)
    target_include_directories(gps_correction_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(gps_correction_test_support PUBLIC Qt::Core)
    add_executable(gpscorrectionextractor_tests tests/test_gpscorrectionextractor.cpp)
    target_link_libraries(gpscorrectionextractor_tests PRIVATE
        gps_correction_test_support Qt::Test)
    add_test(NAME gpscorrectionextractor_tests COMMAND gpscorrectionextractor_tests)
    set_tests_properties(gpscorrectionextractor_tests PROPERTIES TIMEOUT 60)

    add_library(dataflash_splitter_test_support STATIC
        src/ui/Loghandling/DataFlashLogSplitter.cpp
        src/ui/Loghandling/DataFlashLogSplitter.h)
    target_include_directories(dataflash_splitter_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dataflash_splitter_test_support PUBLIC Qt::Core)
    add_executable(dataflashlogsplitter_tests tests/test_dataflashlogsplitter.cpp)
    target_link_libraries(dataflashlogsplitter_tests PRIVATE
        dataflash_splitter_test_support Qt::Test)
    add_test(NAME dataflashlogsplitter_tests COMMAND dataflashlogsplitter_tests)
    set_tests_properties(dataflashlogsplitter_tests PROPERTIES TIMEOUT 60)
    add_executable(dataflash_log_splitter_probe tests/DataFlashLogSplitterProbe.cpp)
    target_link_libraries(dataflash_log_splitter_probe PRIVATE dataflash_splitter_test_support)

    add_library(dataflash_dashware_test_support STATIC
        src/ui/Loghandling/DataFlashRawReader.h
        src/ui/Loghandling/DataFlashModeNames.h
        src/ui/Loghandling/DataFlashDashWareCsvExporter.cpp
        src/ui/Loghandling/DataFlashDashWareCsvExporter.h)
    target_include_directories(dataflash_dashware_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(dataflash_dashware_test_support PUBLIC Qt::Core)
    add_executable(dataflashdashwarecsvexporter_tests tests/test_dataflashdashwarecsvexporter.cpp)
    target_link_libraries(dataflashdashwarecsvexporter_tests PRIVATE
        dataflash_dashware_test_support Qt::Test)
    add_test(NAME dataflashdashwarecsvexporter_tests COMMAND dataflashdashwarecsvexporter_tests)
    set_tests_properties(dataflashdashwarecsvexporter_tests PROPERTIES TIMEOUT 60)
    add_executable(dataflash_dashware_probe tests/DataFlashDashWareProbe.cpp)
    target_link_libraries(dataflash_dashware_probe PRIVATE dataflash_dashware_test_support)

    add_library(mavftp_download_ui_test_support STATIC
        src/ui/configuration/MavFtpFileDownload.cpp
        src/ui/configuration/MavFtpFileDownload.h
        src/comm/MavFtpServiceInterface.h)
    target_include_directories(mavftp_download_ui_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(mavftp_download_ui_test_support PUBLIC
        developer_vehicle_tool_test_support Qt::Widgets Qt::Concurrent)
    add_executable(mavftpfiledownload_tests tests/test_mavftpfiledownload.cpp)
    target_link_libraries(mavftpfiledownload_tests PRIVATE
        mavftp_download_ui_test_support Qt::Test)
    add_test(NAME mavftpfiledownload_tests COMMAND mavftpfiledownload_tests)
    set_tests_properties(mavftpfiledownload_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    add_library(apj_defaults_test_support STATIC
        src/ui/tools/ApjDefaultsEmbedder.cpp
        src/ui/tools/ApjDefaultsEmbedder.h)
    target_include_directories(apj_defaults_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(apj_defaults_test_support PUBLIC Qt::Core ZLIB::ZLIB)
    add_executable(apjdefaultsembedder_tests tests/test_apjdefaultsembedder.cpp)
    target_link_libraries(apjdefaultsembedder_tests PRIVATE
        apj_defaults_test_support Qt::Test)
    add_test(NAME apjdefaultsembedder_tests COMMAND apjdefaultsembedder_tests)
    set_tests_properties(apjdefaultsembedder_tests PROPERTIES TIMEOUT 60)
    add_executable(apj_defaults_probe tests/ApjDefaultsEmbedderProbe.cpp)
    target_link_libraries(apj_defaults_probe PRIVATE apj_defaults_test_support)

    add_library(flight_log_organizer_test_support STATIC
        src/ui/Loghandling/FlightLogClassifier.cpp
        src/ui/Loghandling/FlightLogClassifier.h
        src/ui/Loghandling/FlightLogOrganizer.cpp
        src/ui/Loghandling/FlightLogOrganizer.h)
    target_link_libraries(flight_log_organizer_test_support PUBLIC
        gps_correction_test_support Qt::Core)
    add_executable(flightlogclassifier_tests tests/test_flightlogclassifier.cpp)
    target_link_libraries(flightlogclassifier_tests PRIVATE
        flight_log_organizer_test_support Qt::Test)
    add_test(NAME flightlogclassifier_tests COMMAND flightlogclassifier_tests)
    set_tests_properties(flightlogclassifier_tests PROPERTIES TIMEOUT 60)
    add_executable(flightlogorganizer_tests tests/test_flightlogorganizer.cpp)
    target_link_libraries(flightlogorganizer_tests PRIVATE
        flight_log_organizer_test_support Qt::Test)
    add_test(NAME flightlogorganizer_tests COMMAND flightlogorganizer_tests)
    set_tests_properties(flightlogorganizer_tests PROPERTIES TIMEOUT 60)
    add_executable(flight_log_organizer_probe tests/FlightLogOrganizerProbe.cpp)
    target_link_libraries(flight_log_organizer_probe PRIVATE
        flight_log_organizer_test_support)

    add_executable(configdevelopertoolsview_tests
        tests/test_configdevelopertoolsview.cpp
        src/ui/configuration/ActionPageView.cpp
        src/ui/configuration/ActionPageView.h
        src/ui/configuration/ConfigDeveloperToolsView.cpp
        src/ui/configuration/ConfigDeveloperToolsView.h
        src/ui/configuration/DeveloperToolParsers.cpp
        src/ui/configuration/DeveloperToolParsers.h
    )
    target_include_directories(configdevelopertoolsview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(configdevelopertoolsview_tests PRIVATE
        developer_vehicle_tool_test_support
        gps_correction_test_support
        dataflash_splitter_test_support
        dataflash_dashware_test_support
        mavftp_download_ui_test_support
        apj_defaults_test_support
        flight_log_organizer_test_support
        Qt::Concurrent
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME configdevelopertoolsview_tests
        COMMAND configdevelopertoolsview_tests)
    set_tests_properties(configdevelopertoolsview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(srtmelevationsource_tests
        tests/test_srtmelevationsource.cpp
        src/ui/configuration/SrtmElevationSource.cpp
        src/ui/configuration/SrtmElevationSource.h
    )
    target_include_directories(srtmelevationsource_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(srtmelevationsource_tests PRIVATE
        quazip
        Qt::Core
        Qt::Network
        Qt::Test
    )
    add_test(NAME srtmelevationsource_tests
        COMMAND srtmelevationsource_tests)
    set_tests_properties(srtmelevationsource_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configserialview_tests
        tests/test_configserialview.cpp
        src/ui/configuration/ConfigSerialView.cpp
        src/ui/configuration/ConfigSerialView.h
        src/ui/configuration/ConfigSerialViewModel.cpp
        src/ui/configuration/ConfigSerialViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configserialview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configserialview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configserialview_tests COMMAND configserialview_tests)
    set_tests_properties(configserialview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configradiooutputview_tests
        tests/test_configradiooutputview.cpp
        src/ui/configuration/ConfigRadioOutputView.cpp
        src/ui/configuration/ConfigRadioOutputView.h
        src/ui/configuration/ConfigRadioOutputViewModel.cpp
        src/ui/configuration/ConfigRadioOutputViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configradiooutputview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configradiooutputview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configradiooutputview_tests
        COMMAND configradiooutputview_tests)
    set_tests_properties(configradiooutputview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configesccalibrationview_tests
        tests/test_configesccalibrationview.cpp
        src/ui/configuration/ConfigESCCalibrationView.cpp
        src/ui/configuration/ConfigESCCalibrationView.h
        src/ui/configuration/ConfigESCCalibrationViewModel.cpp
        src/ui/configuration/ConfigESCCalibrationViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configesccalibrationview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configesccalibrationview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configesccalibrationview_tests
        COMMAND configesccalibrationview_tests)
    set_tests_properties(configesccalibrationview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configmotortestview_tests
        tests/test_configmotortestview.cpp
        src/ui/configuration/ConfigMotorTestView.cpp
        src/ui/configuration/ConfigMotorTestView.h
        src/ui/configuration/ConfigMotorTestViewModel.cpp
        src/ui/configuration/ConfigMotorTestViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configmotortestview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configmotortestview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configmotortestview_tests
        COMMAND configmotortestview_tests)
    set_tests_properties(configmotortestview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configparachuteview_tests
        tests/test_configparachuteview.cpp
        src/ui/configuration/ConfigParachuteView.cpp
        src/ui/configuration/ConfigParachuteView.h
        src/ui/configuration/ConfigParachuteViewModel.cpp
        src/ui/configuration/ConfigParachuteViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configparachuteview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configparachuteview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configparachuteview_tests
        COMMAND configparachuteview_tests)
    set_tests_properties(configparachuteview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(helivisualization_tests
        tests/test_helivisualization.cpp
        src/ui/configuration/HeliVisualization.cpp
        src/ui/configuration/HeliVisualization.h
    )
    target_include_directories(helivisualization_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(helivisualization_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME helivisualization_tests
        COMMAND helivisualization_tests)

    add_executable(helicollectiveplot_tests
        tests/test_helicollectiveplot.cpp
        src/ui/configuration/HeliCollectivePlot.cpp
        src/ui/configuration/HeliCollectivePlot.h
        src/ui/configuration/HeliVisualization.cpp
        src/ui/configuration/HeliVisualization.h
    )
    target_include_directories(helicollectiveplot_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(helicollectiveplot_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME helicollectiveplot_tests
        COMMAND helicollectiveplot_tests)
    set_tests_properties(helicollectiveplot_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configtradheliviewmodel_tests
        tests/test_configtradheliviewmodel.cpp
        src/ui/configuration/ConfigTradHeliViewModel.cpp
        src/ui/configuration/ConfigTradHeliViewModel.h
        src/ui/configuration/HeliVisualization.cpp
        src/ui/configuration/HeliVisualization.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configtradheliviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configtradheliviewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME configtradheliviewmodel_tests
        COMMAND configtradheliviewmodel_tests)

    add_executable(configtradheliview_tests
        tests/test_configtradheliview.cpp
        src/ui/configuration/ConfigTradHeliView.cpp
        src/ui/configuration/ConfigTradHeliView.h
        src/ui/configuration/ConfigTradHeliViewModel.cpp
        src/ui/configuration/ConfigTradHeliViewModel.h
        src/ui/configuration/HeliCollectivePlot.cpp
        src/ui/configuration/HeliCollectivePlot.h
        src/ui/configuration/HeliVisualization.cpp
        src/ui/configuration/HeliVisualization.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configtradheliview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configtradheliview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configtradheliview_tests
        COMMAND configtradheliview_tests)
    set_tests_properties(configtradheliview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configtradheli4viewmodel_tests
        tests/test_configtradheli4viewmodel.cpp
        src/ui/configuration/ConfigTradHeli4ViewModel.cpp
        src/ui/configuration/ConfigTradHeli4ViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configtradheli4viewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configtradheli4viewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME configtradheli4viewmodel_tests
        COMMAND configtradheli4viewmodel_tests)

    add_executable(configtradheli4view_tests
        tests/test_configtradheli4view.cpp
        src/ui/configuration/ConfigTradHeli4View.cpp
        src/ui/configuration/ConfigTradHeli4View.h
        src/ui/configuration/ConfigTradHeli4ViewModel.cpp
        src/ui/configuration/ConfigTradHeli4ViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configtradheli4view_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configtradheli4view_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configtradheli4view_tests
        COMMAND configtradheli4view_tests)
    set_tests_properties(configtradheli4view_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configframeclasstypeviewmodel_tests
        tests/test_configframeclasstypeviewmodel.cpp
        src/ui/configuration/ConfigFrameClassTypeViewModel.cpp
        src/ui/configuration/ConfigFrameClassTypeViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configframeclasstypeviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configframeclasstypeviewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME configframeclasstypeviewmodel_tests
        COMMAND configframeclasstypeviewmodel_tests)

    add_executable(configframetypeviewmodel_tests
        tests/test_configframetypeviewmodel.cpp
        src/ui/configuration/ConfigFrameTypeViewModel.cpp
        src/ui/configuration/ConfigFrameTypeViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configframetypeviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configframetypeviewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME configframetypeviewmodel_tests
        COMMAND configframetypeviewmodel_tests)

    add_executable(configframeviews_tests
        tests/test_configframeviews.cpp
        src/ui/configuration/ConfigFrameClassTypeView.cpp
        src/ui/configuration/ConfigFrameClassTypeView.h
        src/ui/configuration/ConfigFrameClassTypeViewModel.cpp
        src/ui/configuration/ConfigFrameClassTypeViewModel.h
        src/ui/configuration/ConfigFrameTypeView.cpp
        src/ui/configuration/ConfigFrameTypeView.h
        src/ui/configuration/ConfigFrameTypeViewModel.cpp
        src/ui/configuration/ConfigFrameTypeViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configframeviews_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configframeviews_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configframeviews_tests COMMAND configframeviews_tests)
    set_tests_properties(configframeviews_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configflightmodesviewmodel_tests
        tests/test_configflightmodesviewmodel.cpp
        src/ui/configuration/ConfigFlightModesViewModel.cpp
        src/ui/configuration/ConfigFlightModesViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configflightmodesviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configflightmodesviewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME configflightmodesviewmodel_tests
        COMMAND configflightmodesviewmodel_tests)

    add_executable(configflightmodesview_tests
        tests/test_configflightmodesview.cpp
        src/ui/configuration/ConfigFlightModesView.cpp
        src/ui/configuration/ConfigFlightModesView.h
        src/ui/configuration/ConfigFlightModesViewModel.cpp
        src/ui/configuration/ConfigFlightModesViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configflightmodesview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configflightmodesview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configflightmodesview_tests
        COMMAND configflightmodesview_tests)
    set_tests_properties(configflightmodesview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configextendedtuningviewmodel_tests
        tests/test_configextendedtuningviewmodel.cpp
        src/ui/configuration/ConfigExtendedTuningViewModel.cpp
        src/ui/configuration/ConfigExtendedTuningViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configextendedtuningviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configextendedtuningviewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME configextendedtuningviewmodel_tests
        COMMAND configextendedtuningviewmodel_tests)

    add_executable(configextendedtuningview_tests
        tests/test_configextendedtuningview.cpp
        src/ui/configuration/ConfigExtendedTuningView.cpp
        src/ui/configuration/ConfigExtendedTuningView.h
        src/ui/configuration/ConfigExtendedTuningViewModel.cpp
        src/ui/configuration/ConfigExtendedTuningViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configextendedtuningview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configextendedtuningview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configextendedtuningview_tests
        COMMAND configextendedtuningview_tests)
    set_tests_properties(configextendedtuningview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(flightmodetelemetrymonitor_tests
        tests/test_flightmodetelemetrymonitor.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/configuration/FlightModeTelemetryMonitor.cpp
        src/ui/configuration/FlightModeTelemetryMonitor.h
    )
    target_include_directories(flightmodetelemetrymonitor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(flightmodetelemetrymonitor_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightmodetelemetrymonitor_tests
        COMMAND flightmodetelemetrymonitor_tests)

    add_executable(configcompassviewmodel_tests
        tests/test_configcompassviewmodel.cpp
        src/ui/configuration/ConfigCompassViewModel.cpp
        src/ui/configuration/ConfigCompassViewModel.h
        src/ui/configuration/ConfigHWIDViewModel.cpp
        src/ui/configuration/ConfigHWIDViewModel.h
        src/ui/configuration/DeveloperToolParsers.cpp
        src/ui/configuration/DeveloperToolParsers.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configcompassviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(configcompassviewmodel_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configcompassviewmodel_tests
        COMMAND configcompassviewmodel_tests)
    set_tests_properties(configcompassviewmodel_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configcompassview_tests
        tests/test_configcompassview.cpp
        src/comm/CompassCalibrationService.cpp
        src/comm/CompassCalibrationService.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/configuration/ConfigCompassView.cpp
        src/ui/configuration/ConfigCompassView.h
        src/ui/configuration/ConfigCompassViewModel.cpp
        src/ui/configuration/ConfigCompassViewModel.h
        src/ui/configuration/ConfigHWIDViewModel.cpp
        src/ui/configuration/ConfigHWIDViewModel.h
        src/ui/configuration/DeveloperToolParsers.cpp
        src/ui/configuration/DeveloperToolParsers.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configcompassview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(configcompassview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configcompassview_tests COMMAND configcompassview_tests)
    set_tests_properties(configcompassview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configcompassmotview_tests
        tests/test_configcompassmotview.cpp
        src/comm/CompassCalibrationService.cpp
        src/comm/CompassCalibrationService.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/configuration/ConfigCompassMotView.cpp
        src/ui/configuration/ConfigCompassMotView.h
        src/ui/qcustomplot.cpp
        src/ui/qcustomplot.h
    )
    target_include_directories(configcompassmotview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(configcompassmotview_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::PrintSupport
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configcompassmotview_tests
        COMMAND configcompassmotview_tests)
    set_tests_properties(configcompassmotview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configgpsinjectview_tests
        tests/test_configgpsinjectviewmodel.cpp
        src/ui/configuration/ConfigGpsInjectView.cpp
        src/ui/configuration/ConfigGpsInjectView.h
        src/ui/configuration/ConfigGpsInjectViewModel.cpp
        src/ui/configuration/ConfigGpsInjectViewModel.h
        src/comm/GpsCorrectionSource.cpp
        src/comm/GpsCorrectionSource.h
        src/comm/Rtcm3Parser.cpp
        src/comm/Rtcm3Parser.h
    )
    target_include_directories(configgpsinjectview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configgpsinjectview_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configgpsinjectview_tests
        COMMAND configgpsinjectview_tests)
    set_tests_properties(configgpsinjectview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(confighwcanview_tests
        tests/test_confighwcanview.cpp
        src/ui/configuration/ConfigHWCANView.cpp
        src/ui/configuration/ConfigHWCANView.h
        src/ui/configuration/ConfigHWCANViewModel.cpp
        src/ui/configuration/ConfigHWCANViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(confighwcanview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(confighwcanview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME confighwcanview_tests
        COMMAND confighwcanview_tests)
    set_tests_properties(confighwcanview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configdronecanview_tests
        tests/test_configdronecanview.cpp
        src/comm/DroneCanFrameCodec.cpp
        src/comm/DroneCanFrameCodec.h
        src/comm/DroneCanGetSetClient.cpp
        src/comm/DroneCanGetSetClient.h
        src/ui/configuration/ConfigDroneCanView.cpp
        src/ui/configuration/ConfigDroneCanView.h
        src/ui/configuration/ConfigDroneCanViewModel.cpp
        src/ui/configuration/ConfigDroneCanViewModel.h
    )
    target_include_directories(configdronecanview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configdronecanview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configdronecanview_tests
        COMMAND configdronecanview_tests)
    set_tests_properties(configdronecanview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configgpsorderview_tests
        tests/test_configgpsorderview.cpp
        src/ui/configuration/ConfigGPSOrderView.cpp
        src/ui/configuration/ConfigGPSOrderView.h
        src/ui/configuration/ConfigGPSOrderViewModel.cpp
        src/ui/configuration/ConfigGPSOrderViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configgpsorderview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configgpsorderview_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configgpsorderview_tests
        COMMAND configgpsorderview_tests)
    set_tests_properties(configgpsorderview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(confighwbtview_tests
        tests/test_confighwbtview.cpp
        src/ui/configuration/ConfigHWBTView.cpp
        src/ui/configuration/ConfigHWBTView.h
        src/ui/configuration/ConfigHWBTViewModel.cpp
        src/ui/configuration/ConfigHWBTViewModel.h
    )
    target_include_directories(confighwbtview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(confighwbtview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME confighwbtview_tests
        COMMAND confighwbtview_tests)
    set_tests_properties(confighwbtview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configinitialparamsview_tests
        tests/test_configinitialparamsview.cpp
        src/ui/configuration/ConfigInitialParamsView.cpp
        src/ui/configuration/ConfigInitialParamsView.h
        src/ui/configuration/ConfigInitialParamsViewModel.cpp
        src/ui/configuration/ConfigInitialParamsViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configinitialparamsview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(configinitialparamsview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configinitialparamsview_tests
        COMMAND configinitialparamsview_tests)
    set_tests_properties(configinitialparamsview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(batterymonitor_tests
        tests/test_batterymonitor.cpp
        src/core/parameters/ParameterMetaData.cpp
        src/core/parameters/ParameterMetaData.h
        src/services/SpeechSettings.cpp
        src/services/SpeechSettings.h
        src/ui/configuration/BatteryMonitorInstanceModel.cpp
        src/ui/configuration/BatteryMonitorInstanceModel.h
        src/ui/configuration/ConfigBatteryMonitoring2View.cpp
        src/ui/configuration/ConfigBatteryMonitoring2View.h
        src/ui/configuration/ConfigBatteryMonitoring2ViewModel.cpp
        src/ui/configuration/ConfigBatteryMonitoring2ViewModel.h
    )
    target_include_directories(batterymonitor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(batterymonitor_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME batterymonitor_tests COMMAND batterymonitor_tests)
    set_tests_properties(batterymonitor_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mavlink_component_tracker_tests
        tests/test_mavlinkcomponenttracker.cpp
        src/uas/MAVLinkComponentTracker.h
    )
    target_include_directories(mavlink_component_tracker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(mavlink_component_tracker_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME mavlink_component_tracker_tests COMMAND mavlink_component_tracker_tests)

    add_executable(mavlink_frame_parser_tests
        tests/test_mavlinkframeparser.cpp
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
    )
    target_include_directories(mavlink_frame_parser_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(mavlink_frame_parser_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME mavlink_frame_parser_tests
             COMMAND mavlink_frame_parser_tests)

    add_executable(exact_link_transmitter_tests
        tests/test_exactlinktransmitter.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
    )
    target_include_directories(exact_link_transmitter_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(exact_link_transmitter_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME exact_link_transmitter_tests
             COMMAND exact_link_transmitter_tests)

    add_executable(adsb_identification_client_tests
        tests/test_adsbidentificationclient.cpp
        src/comm/AdsbIdentificationClient.cpp
        src/comm/AdsbIdentificationClient.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(adsb_identification_client_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(adsb_identification_client_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME adsb_identification_client_tests
             COMMAND adsb_identification_client_tests)

    add_executable(esp8266settings_tests
        tests/test_esp8266settings.cpp
        src/ui/configuration/Esp8266Settings.cpp
        src/ui/configuration/Esp8266Settings.h
    )
    target_include_directories(esp8266settings_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(esp8266settings_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME esp8266settings_tests COMMAND esp8266settings_tests)

    add_executable(esp8266_parameter_client_tests
        tests/test_esp8266parameterclient.cpp
        src/comm/Esp8266ParameterClient.cpp
        src/comm/Esp8266ParameterClient.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/configuration/Esp8266Settings.cpp
        src/ui/configuration/Esp8266Settings.h
    )
    target_include_directories(esp8266_parameter_client_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(esp8266_parameter_client_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME esp8266_parameter_client_tests
             COMMAND esp8266_parameter_client_tests)

    add_executable(confighwesp8266view_tests
        tests/test_confighwesp8266view.cpp
        src/ui/configuration/ConfigHWESP8266View.cpp
        src/ui/configuration/ConfigHWESP8266View.h
        src/ui/configuration/ConfigHWESP8266ViewModel.cpp
        src/ui/configuration/ConfigHWESP8266ViewModel.h
        src/ui/configuration/Esp8266Settings.cpp
        src/ui/configuration/Esp8266Settings.h
        src/comm/Esp8266ParameterClient.cpp
        src/comm/Esp8266ParameterClient.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(confighwesp8266view_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(confighwesp8266view_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME confighwesp8266view_tests
             COMMAND confighwesp8266view_tests)
    set_tests_properties(confighwesp8266view_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(confighwosdview_tests
        tests/test_confighwosdview.cpp
        src/ui/configuration/ConfigHWOSDView.cpp
        src/ui/configuration/ConfigHWOSDView.h
        src/ui/configuration/ConfigHWOSDViewModel.cpp
        src/ui/configuration/ConfigHWOSDViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(confighwosdview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/configuration
    )
    target_link_libraries(confighwosdview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME confighwosdview_tests
             COMMAND confighwosdview_tests)
    set_tests_properties(confighwosdview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configosdviewmodel_tests
        tests/test_configosdviewmodel.cpp
        src/ui/configuration/ConfigOSDViewModel.cpp
        src/ui/configuration/ConfigOSDViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configosdviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(configosdviewmodel_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME configosdviewmodel_tests
             COMMAND configosdviewmodel_tests)

    add_executable(configosdlayoutcanvas_tests
        tests/test_configosdlayoutcanvas.cpp
        src/ui/configuration/ConfigOSDLayoutCanvas.cpp
        src/ui/configuration/ConfigOSDLayoutCanvas.h
    )
    target_include_directories(configosdlayoutcanvas_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(configosdlayoutcanvas_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configosdlayoutcanvas_tests
             COMMAND configosdlayoutcanvas_tests)
    set_tests_properties(configosdlayoutcanvas_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(configosdview_tests
        tests/test_configosdview.cpp
        src/ui/configuration/ConfigOSDLayoutCanvas.cpp
        src/ui/configuration/ConfigOSDLayoutCanvas.h
        src/ui/configuration/ConfigOSDView.cpp
        src/ui/configuration/ConfigOSDView.h
        src/ui/configuration/ConfigOSDViewModel.cpp
        src/ui/configuration/ConfigOSDViewModel.h
        src/ui/configuration/ParamField.h
    )
    target_include_directories(configosdview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(configosdview_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME configosdview_tests COMMAND configosdview_tests)
    set_tests_properties(configosdview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(antennatrackeroutputs_tests
        tests/test_antennatrackeroutputs.cpp
        src/ui/configuration/AntennaTrackerOutputs.cpp
        src/ui/configuration/AntennaTrackerOutputs.h
    )
    target_include_directories(antennatrackeroutputs_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(antennatrackeroutputs_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME antennatrackeroutputs_tests
             COMMAND antennatrackeroutputs_tests)

    add_executable(antennatrackergeometry_tests
        tests/test_antennatrackergeometry.cpp
        src/ui/configuration/AntennaTrackerGeometry.cpp
        src/ui/configuration/AntennaTrackerGeometry.h
    )
    target_include_directories(antennatrackergeometry_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(antennatrackergeometry_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME antennatrackergeometry_tests
             COMMAND antennatrackergeometry_tests)

    add_executable(antennatrackerserialservice_tests
        tests/test_antennatrackerserialservice.cpp
        src/ui/configuration/AntennaTrackerSerialService.cpp
        src/ui/configuration/AntennaTrackerSerialService.h
        src/ui/configuration/AntennaTrackerOutputs.cpp
        src/ui/configuration/AntennaTrackerOutputs.h
    )
    target_include_directories(antennatrackerserialservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(antennatrackerserialservice_tests PRIVATE
        Qt::Core
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME antennatrackerserialservice_tests
             COMMAND antennatrackerserialservice_tests)

    add_executable(antennatrackeruiviewmodel_tests
        tests/test_antennatrackeruiviewmodel.cpp
        tests/AntennaTrackerTestFakes.h
        src/ui/configuration/AntennaTrackerUIViewModel.cpp
        src/ui/configuration/AntennaTrackerUIViewModel.h
        src/ui/configuration/AntennaTrackerTelemetrySource.cpp
        src/ui/configuration/AntennaTrackerTelemetrySource.h
        src/ui/configuration/AntennaTrackerSerialService.cpp
        src/ui/configuration/AntennaTrackerSerialService.h
        src/ui/configuration/AntennaTrackerOutputs.cpp
        src/ui/configuration/AntennaTrackerOutputs.h
        src/ui/configuration/AntennaTrackerGeometry.cpp
        src/ui/configuration/AntennaTrackerGeometry.h
    )
    target_include_directories(antennatrackeruiviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(antennatrackeruiviewmodel_tests PRIVATE
        Qt::Core
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME antennatrackeruiviewmodel_tests
             COMMAND antennatrackeruiviewmodel_tests)

    add_executable(antennatrackerviews_tests
        tests/test_antennatrackerviews.cpp
        tests/AntennaTrackerTestFakes.h
        src/ui/configuration/AntennaTrackerUIView.cpp
        src/ui/configuration/AntennaTrackerUIView.h
        src/ui/configuration/ConfigAntennaTrackerView.cpp
        src/ui/configuration/ConfigAntennaTrackerView.h
        src/ui/configuration/AntennaTrackerAxisPanel.cpp
        src/ui/configuration/AntennaTrackerAxisPanel.h
        src/ui/configuration/AntennaTrackerUIViewModel.cpp
        src/ui/configuration/AntennaTrackerUIViewModel.h
        src/ui/configuration/AntennaTrackerTelemetrySource.cpp
        src/ui/configuration/AntennaTrackerTelemetrySource.h
        src/ui/configuration/AntennaTrackerSerialService.cpp
        src/ui/configuration/AntennaTrackerSerialService.h
        src/ui/configuration/AntennaTrackerOutputs.cpp
        src/ui/configuration/AntennaTrackerOutputs.h
        src/ui/configuration/AntennaTrackerGeometry.cpp
        src/ui/configuration/AntennaTrackerGeometry.h
    )
    target_include_directories(antennatrackerviews_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(antennatrackerviews_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME antennatrackerviews_tests
             COMMAND antennatrackerviews_tests)
    set_tests_properties(antennatrackerviews_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(radiostatusmonitor_tests
        tests/test_radiostatusmonitor.cpp
        src/comm/RadioStatusMonitor.cpp
        src/comm/RadioStatusMonitor.h
    )
    target_include_directories(radiostatusmonitor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(radiostatusmonitor_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME radiostatusmonitor_tests
             COMMAND radiostatusmonitor_tests)

    add_executable(linkstatswindow_tests
        tests/test_linkstatswindow.cpp
        src/ui/LinkStatsWindow.cpp
        src/ui/LinkStatsWindow.h
    )
    target_include_directories(linkstatswindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(linkstatswindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME linkstatswindow_tests
             COMMAND linkstatswindow_tests)
    set_tests_properties(linkstatswindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(missionplannertoolsmenu_tests
        tests/test_missionplannertoolsmenu.cpp
        src/ui/MissionPlannerToolsMenu.cpp
        src/ui/MissionPlannerToolsMenu.h
    )
    target_include_directories(missionplannertoolsmenu_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(missionplannertoolsmenu_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME missionplannertoolsmenu_tests
             COMMAND missionplannertoolsmenu_tests)
    set_tests_properties(missionplannertoolsmenu_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mavlinkmirroroutput_tests
        tests/test_mavlinkmirroroutput.cpp
        src/comm/MavlinkMirrorOutput.cpp
        src/comm/MavlinkMirrorOutput.h
    )
    target_include_directories(mavlinkmirroroutput_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(mavlinkmirroroutput_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME mavlinkmirroroutput_tests
             COMMAND mavlinkmirroroutput_tests)

    add_executable(mavlinkmirrorservice_tests
        tests/test_mavlinkmirrorservice.cpp
        src/comm/MavlinkMirrorOutput.cpp
        src/comm/MavlinkMirrorOutput.h
        src/comm/MavlinkMirrorService.cpp
        src/comm/MavlinkMirrorService.h
    )
    target_include_directories(mavlinkmirrorservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(mavlinkmirrorservice_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME mavlinkmirrorservice_tests
             COMMAND mavlinkmirrorservice_tests)

    add_executable(nmeasentencebuilder_tests
        tests/test_nmeasentencebuilder.cpp
        src/comm/NmeaSentenceBuilder.cpp
        src/comm/NmeaSentenceBuilder.h
    )
    target_include_directories(nmeasentencebuilder_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(nmeasentencebuilder_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME nmeasentencebuilder_tests
             COMMAND nmeasentencebuilder_tests)

    add_executable(nmeaoutputservice_tests
        tests/test_nmeaoutputservice.cpp
        src/comm/MavlinkMirrorOutput.cpp
        src/comm/MavlinkMirrorOutput.h
        src/comm/NmeaSentenceBuilder.cpp
        src/comm/NmeaSentenceBuilder.h
        src/comm/NmeaOutputService.cpp
        src/comm/NmeaOutputService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(nmeaoutputservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(nmeaoutputservice_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME nmeaoutputservice_tests
             COMMAND nmeaoutputservice_tests)

    add_executable(serialoutputnmeawindow_tests
        tests/test_serialoutputnmeawindow.cpp
        src/comm/MavlinkMirrorOutput.cpp
        src/comm/MavlinkMirrorOutput.h
        src/comm/NmeaSentenceBuilder.cpp
        src/comm/NmeaSentenceBuilder.h
        src/comm/NmeaOutputService.cpp
        src/comm/NmeaOutputService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/ui/SerialOutputNMEAViewModel.cpp
        src/ui/SerialOutputNMEAViewModel.h
        src/ui/SerialOutputNMEAWindow.cpp
        src/ui/SerialOutputNMEAWindow.h
    )
    target_include_directories(serialoutputnmeawindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(serialoutputnmeawindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::SerialPort
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME serialoutputnmeawindow_tests
             COMMAND serialoutputnmeawindow_tests)
    set_tests_properties(serialoutputnmeawindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(coteventserializer_tests
        tests/test_coteventserializer.cpp
        src/comm/CotEventSerializer.cpp
        src/comm/CotEventSerializer.h
    )
    target_include_directories(coteventserializer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(coteventserializer_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME coteventserializer_tests
             COMMAND coteventserializer_tests)

    add_executable(cotidentitymodel_tests
        tests/test_cotidentitymodel.cpp
        src/ui/CotIdentityModel.cpp
        src/ui/CotIdentityModel.h
    )
    target_include_directories(cotidentitymodel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(cotidentitymodel_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME cotidentitymodel_tests
             COMMAND cotidentitymodel_tests)

    add_executable(cotoutputtransport_tests
        tests/test_cotoutputtransport.cpp
        src/comm/CotOutputTransport.cpp
        src/comm/CotOutputTransport.h
    )
    target_include_directories(cotoutputtransport_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(cotoutputtransport_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME cotoutputtransport_tests
             COMMAND cotoutputtransport_tests)

    add_executable(cotoutputservice_tests
        tests/test_cotoutputservice.cpp
        src/comm/CotEventSerializer.cpp
        src/comm/CotEventSerializer.h
        src/comm/CotOutputTransport.cpp
        src/comm/CotOutputTransport.h
        src/comm/CotOutputService.cpp
        src/comm/CotOutputService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
    )
    target_include_directories(cotoutputservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(cotoutputservice_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME cotoutputservice_tests
             COMMAND cotoutputservice_tests)

    add_executable(serialoutputcotwindow_tests
        tests/test_serialoutputcotwindow.cpp
        src/comm/CotEventSerializer.cpp
        src/comm/CotEventSerializer.h
        src/comm/CotOutputTransport.cpp
        src/comm/CotOutputTransport.h
        src/comm/CotOutputService.cpp
        src/comm/CotOutputService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/ui/CotIdentityModel.cpp
        src/ui/CotIdentityModel.h
        src/ui/SerialOutputCotViewModel.cpp
        src/ui/SerialOutputCotViewModel.h
        src/ui/SerialOutputCotWindow.cpp
        src/ui/SerialOutputCotWindow.h
    )
    target_include_directories(serialoutputcotwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(serialoutputcotwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::SerialPort
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME serialoutputcotwindow_tests
             COMMAND serialoutputcotwindow_tests)
    set_tests_properties(serialoutputcotwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(deviceoperationservice_tests
        tests/test_deviceoperationservice.cpp
        src/comm/DeviceOperationService.cpp
        src/comm/DeviceOperationService.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(deviceoperationservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(deviceoperationservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME deviceoperationservice_tests
             COMMAND deviceoperationservice_tests)

    add_executable(deviceoperationswindow_tests
        tests/test_deviceoperationswindow.cpp
        src/comm/DeviceOperationService.cpp
        src/comm/DeviceOperationService.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/ui/DeviceOperationsViewModel.cpp
        src/ui/DeviceOperationsViewModel.h
        src/ui/DeviceOperationsWindow.cpp
        src/ui/DeviceOperationsWindow.h
    )
    target_include_directories(deviceoperationswindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(deviceoperationswindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME deviceoperationswindow_tests
             COMMAND deviceoperationswindow_tests)
    set_tests_properties(deviceoperationswindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(serialpassthroughwindow_tests
        tests/test_serialpassthroughwindow.cpp
        src/comm/MavlinkMirrorOutput.cpp
        src/comm/MavlinkMirrorOutput.h
        src/comm/MavlinkMirrorService.cpp
        src/comm/MavlinkMirrorService.h
        src/ui/SerialPassThroughViewModel.cpp
        src/ui/SerialPassThroughViewModel.h
        src/ui/SerialPassThroughWindow.cpp
        src/ui/SerialPassThroughWindow.h
    )
    target_include_directories(serialpassthroughwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(serialpassthroughwindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::SerialPort
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME serialpassthroughwindow_tests
             COMMAND serialpassthroughwindow_tests)
    set_tests_properties(serialpassthroughwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(tlogreader_tests
        tests/test_tlogreader.cpp
        src/comm/TlogReader.cpp
        src/comm/TlogReader.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
    )
    target_include_directories(tlogreader_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(tlogreader_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME tlogreader_tests COMMAND tlogreader_tests)

    add_executable(osdvideooverlaycore_tests
        tests/test_osdvideooverlaycore.cpp
        src/ui/tools/OsdVideoOverlayCore.cpp
        src/ui/tools/OsdVideoOverlayCore.h
        src/comm/TlogReader.cpp
        src/comm/TlogReader.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
    )
    target_include_directories(osdvideooverlaycore_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(osdvideooverlaycore_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME osdvideooverlaycore_tests
             COMMAND osdvideooverlaycore_tests)

    add_executable(mjpegaviwriter_tests
        tests/test_mjpegaviwriter.cpp
        src/ui/tools/MjpegAviWriter.cpp
        src/ui/tools/MjpegAviWriter.h
    )
    target_include_directories(mjpegaviwriter_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(mjpegaviwriter_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME mjpegaviwriter_tests COMMAND mjpegaviwriter_tests)

    add_executable(osdvideodecoder_tests
        tests/test_osdvideodecoder.cpp
        src/ui/tools/MjpegAviWriter.cpp
        src/ui/tools/MjpegAviWriter.h
        src/ui/tools/OsdVideoDecoder.cpp
        src/ui/tools/OsdVideoDecoder.h
    )
    target_include_directories(osdvideodecoder_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(osdvideodecoder_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Test
    )
    if(TARGET Qt::Multimedia)
        target_link_libraries(osdvideodecoder_tests PRIVATE Qt::Multimedia)
    endif()
    add_test(NAME osdvideodecoder_tests COMMAND osdvideodecoder_tests)

    add_executable(osdvideooverlaywindow_tests
        tests/test_osdvideooverlaywindow.cpp
        src/ui/OsdVideoOverlayWindow.cpp
        src/ui/OsdVideoOverlayWindow.h
        src/ui/tools/MjpegAviWriter.cpp
        src/ui/tools/MjpegAviWriter.h
        src/ui/tools/OsdVideoDecoder.cpp
        src/ui/tools/OsdVideoDecoder.h
        src/ui/tools/OsdVideoOverlayCore.cpp
        src/ui/tools/OsdVideoOverlayCore.h
        src/ui/flightdata/HudControl.cpp
        src/ui/flightdata/HudControl.h
        src/services/HudDisplaySettings.cpp
        src/services/HudDisplaySettings.h
        src/comm/TlogReader.cpp
        src/comm/TlogReader.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
    )
    target_include_directories(osdvideooverlaywindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/flightdata
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(osdvideooverlaywindow_tests PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    if(TARGET Qt::Multimedia)
        target_link_libraries(osdvideooverlaywindow_tests PRIVATE Qt::Multimedia)
    endif()
    add_test(NAME osdvideooverlaywindow_tests
             COMMAND osdvideooverlaywindow_tests)
    set_tests_properties(osdvideooverlaywindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(tlogexportservice_tests
        tests/test_tlogexportservice.cpp
        src/comm/TlogExportService.cpp
        src/comm/TlogExportService.h
        src/comm/TlogReader.cpp
        src/comm/TlogReader.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
    )
    target_include_directories(tlogexportservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(tlogexportservice_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME tlogexportservice_tests COMMAND tlogexportservice_tests)

    add_executable(mavlinksigningwindow_tests
        tests/test_mavlinksigningwindow.cpp
        src/ui/MavlinkSigningWindow.cpp)
    target_link_libraries(mavlinksigningwindow_tests PRIVATE
        apm_mavlink_signing Qt::Widgets Qt::Test OpenSSL::Crypto)
    add_test(NAME mavlinksigningwindow_tests COMMAND mavlinksigningwindow_tests)
    set_tests_properties(mavlinksigningwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

    add_library(warning_manager_test_support STATIC
        src/services/WarningEngine.cpp
        src/services/WarningTelemetrySource.cpp
        src/ui/WarningManagerWindow.cpp
        src/ui/flightdata/QuickViewWidget.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleTargetManager.cpp)
    target_include_directories(warning_manager_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(warning_manager_test_support PUBLIC Qt::Widgets)
    foreach(warning_test IN ITEMS warningengine warningtelemetrysource
            warningmanagerwindow quickviewwidget)
        add_executable(${warning_test}_tests tests/test_${warning_test}.cpp)
        target_link_libraries(${warning_test}_tests PRIVATE
            warning_manager_test_support Qt::Test)
        add_test(NAME ${warning_test}_tests COMMAND ${warning_test}_tests)
        set_tests_properties(${warning_test}_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endforeach()

    add_library(log_anonymizer_test_support STATIC
        src/ui/Loghandling/LogAnonymizer.cpp
        src/ui/Loghandling/LogAnonymizeService.cpp
        src/ui/Loghandling/DataFlashLogAnonymizer.cpp
        src/ui/Loghandling/TelemetryLogAnonymizer.cpp
        src/ui/AnonLogWindow.cpp
        src/comm/TlogReader.cpp
        src/comm/MAVLinkFrameParser.cpp)
    target_include_directories(log_anonymizer_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_link_libraries(log_anonymizer_test_support PUBLIC
        Qt::Core Qt::Gui Qt::Widgets Qt::Concurrent)
    add_executable(log_anonymizer_probe tests/manual/log_anonymizer_probe.cpp)
    target_link_libraries(log_anonymizer_probe PRIVATE log_anonymizer_test_support)
    foreach(anon_test IN ITEMS dataflashloganonymizer telemetryloganonymizer
            loganonymizer anonlogwindow)
        add_executable(${anon_test}_tests tests/test_${anon_test}.cpp)
        target_link_libraries(${anon_test}_tests PRIVATE
            log_anonymizer_test_support Qt::Test)
        add_test(NAME ${anon_test}_tests COMMAND ${anon_test}_tests)
        set_tests_properties(${anon_test}_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endforeach()

    add_executable(mavlinklogwindow_tests
        tests/test_mavlinklogwindow.cpp
        src/ui/MavlinkLogWindow.cpp
        src/ui/MavlinkLogWindow.h
        src/comm/TlogExportService.cpp
        src/comm/TlogExportService.h
        src/comm/TlogReader.cpp
        src/comm/TlogReader.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/AppPaths.cpp
        src/AppPaths.h
        src/globalobject.cc
        src/globalobject.h
    )
    target_include_directories(mavlinklogwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(mavlinklogwindow_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME mavlinklogwindow_tests COMMAND mavlinklogwindow_tests)
    set_tests_properties(mavlinklogwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(mavlinkinspectorpacketstore_tests
        tests/test_mavlinkinspectorpacketstore.cpp
        src/ui/MAVLinkInspectorPacketStore.cpp
        src/ui/MAVLinkInspectorPacketStore.h
    )
    add_executable(mavlinkinspectorsource_tests
        tests/test_mavlinkinspectorsource.cpp
        src/ui/MAVLinkInspectorTrafficSource.cpp
        src/ui/MAVLinkInspectorTrafficSource.h
    )
    add_executable(mavlinkreplaysource_tests
        tests/test_mavlinkreplaysource.cpp
        src/ui/MAVLinkReplaySource.cpp
        src/ui/MAVLinkReplaySource.h
    )
    add_executable(mavlinkinspectorview_tests
        tests/test_mavlinkinspectorview.cpp
        src/ui/MAVLinkInspectorPacketStore.cpp
        src/ui/MAVLinkInspectorPacketStore.h
        src/ui/MAVLinkInspectorTrafficSource.cpp
        src/ui/MAVLinkInspectorTrafficSource.h
        src/ui/MAVLinkInspectorView.cpp
        src/ui/MAVLinkInspectorView.h
        src/ui/MAVLinkInspectorWindow.cpp
        src/ui/MAVLinkInspectorWindow.h
        src/ui/MavlinkGraphSampleExtractor.cpp
    )
    add_executable(mavlinkgraph_tests
        tests/test_mavlinkgraph.cpp
        src/ui/MavlinkGraphSampleExtractor.cpp
        src/ui/MavlinkFieldGraphModel.cpp
    )
    add_executable(mavlinkfieldgraphwindow_tests
        tests/test_mavlinkfieldgraphwindow.cpp
        src/ui/MavlinkGraphSampleExtractor.cpp
        src/ui/MavlinkFieldGraphModel.cpp
        src/ui/MavlinkFieldGraphWindow.cpp
        src/ui/MavlinkFieldGraphWindow.h
        src/ui/qcustomplot.cpp
        src/ui/qcustomplot.h
    )
    target_link_libraries(mavlinkfieldgraphwindow_tests PRIVATE Qt::PrintSupport)
    foreach(inspector_test IN ITEMS mavlinkinspectorpacketstore_tests
            mavlinkinspectorsource_tests mavlinkreplaysource_tests
            mavlinkinspectorview_tests mavlinkgraph_tests
            mavlinkfieldgraphwindow_tests)
        target_include_directories(${inspector_test} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
            ${MAVLINKPATH}
            ${MAVLINKPATH}/${MAVLINK_CONF}
        )
        target_link_libraries(${inspector_test} PRIVATE Qt::Core Qt::Widgets Qt::Test)
        add_test(NAME ${inspector_test} COMMAND ${inspector_test})
        set_tests_properties(${inspector_test} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()

    add_executable(mavlinkinspectorwindow_tests
        tests/test_mavlinkinspectorwindow.cpp
        src/ui/MAVLinkInspectorMessageRelay.cpp
        src/ui/MAVLinkInspectorMessageRelay.h
        src/ui/MAVLinkInspectorWindow.cpp
        src/ui/MAVLinkInspectorWindow.h
        src/comm/MAVLinkMessageMetaType.h
    )
    target_include_directories(mavlinkinspectorwindow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(mavlinkinspectorwindow_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME mavlinkinspectorwindow_tests
             COMMAND mavlinkinspectorwindow_tests)
    set_tests_properties(mavlinkinspectorwindow_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(vehicle_target_manager_tests
        tests/test_vehicletargetmanager.cpp
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(vehicle_target_manager_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(vehicle_target_manager_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME vehicle_target_manager_tests
             COMMAND vehicle_target_manager_tests)

    add_executable(vehicle_command_service_tests
        tests/test_vehiclecommandservice.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(vehicle_command_service_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(vehicle_command_service_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME vehicle_command_service_tests
             COMMAND vehicle_command_service_tests)

    add_executable(compass_calibration_service_tests
        tests/test_compasscalibrationservice.cpp
        src/comm/CompassCalibrationService.cpp
        src/comm/CompassCalibrationService.h
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(compass_calibration_service_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(compass_calibration_service_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME compass_calibration_service_tests
             COMMAND compass_calibration_service_tests)

    add_executable(parameter_service_tests
        tests/test_parameterservice.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/ParameterService.cpp
        src/comm/ParameterService.h
        src/comm/VehicleCommandService.cpp
        src/comm/VehicleCommandService.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
        src/uas/QGCUASParamManager.cc
        src/uas/QGCUASParamManager.h
    )
    target_include_directories(parameter_service_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(parameter_service_tests PRIVATE
        apm_parameter_core
        Qt::Core
        Qt::Test
    )
    add_test(NAME parameter_service_tests
             COMMAND parameter_service_tests)

    add_executable(mavftpprotocol_tests
        tests/test_mavftpprotocol.cpp
        src/comm/MavFtpProtocol.cpp
        src/comm/MavFtpProtocol.h
    )
    target_include_directories(mavftpprotocol_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(mavftpprotocol_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME mavftpprotocol_tests COMMAND mavftpprotocol_tests)

    add_executable(mavftpservice_tests
        tests/test_mavftpservice.cpp
        src/comm/ExactLinkTransmitter.cpp
        src/comm/ExactLinkTransmitter.h
        src/comm/MAVLinkFrameParser.cpp
        src/comm/MAVLinkFrameParser.h
        src/comm/MavFtpProtocol.cpp
        src/comm/MavFtpProtocol.h
        src/comm/MavFtpService.cpp
        src/comm/MavFtpService.h
        src/comm/MavFtpServiceInterface.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/comm/VehicleTargetManager.cpp
        src/comm/VehicleTargetManager.h
    )
    target_include_directories(mavftpservice_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(mavftpservice_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME mavftpservice_tests COMMAND mavftpservice_tests)

    add_executable(mavftpuiview_tests
        tests/test_mavftpuiview.cpp
        src/comm/MavFtpProtocol.h
        src/comm/MavFtpServiceInterface.h
        src/comm/VehicleEndpoint.cpp
        src/comm/VehicleEndpoint.h
        src/ui/configuration/MavFTPUIView.cpp
        src/ui/configuration/MavFTPUIView.h
    )
    target_include_directories(mavftpuiview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(mavftpuiview_tests PRIVATE
        Qt::Core
        Qt::Test
        Qt::Widgets
    )
    add_test(NAME mavftpuiview_tests COMMAND mavftpuiview_tests)
    set_tests_properties(mavftpuiview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )

    add_executable(backstageview_tests
        tests/test_backstageview.cpp
        src/ui/BackstageView.cpp
        src/ui/BackstageView.h
        src/ui/configuration/ConfigParamLoadingView.cpp
        src/ui/configuration/ConfigParamLoadingView.h
        src/ui/configuration/ConfigParamLoadingViewModel.cpp
        src/ui/configuration/ConfigParamLoadingViewModel.h
    )
    target_include_directories(backstageview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(backstageview_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME backstageview_tests COMMAND backstageview_tests)
    set_tests_properties(backstageview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(connectionoptions_tests
        tests/test_connectionoptionswindow.cpp
        src/comm/SerialLinkInterface.h
        src/AppPaths.cpp
        src/AppPaths.h
        src/comm/LinkInterface.cpp
        src/comm/LinkInterface.h
        src/comm/serialconnection.cc
        src/comm/serialconnection.h
        src/globalobject.cc
        src/globalobject.h
        src/ui/AddConnectionViewModel.cpp
        src/ui/AddConnectionViewModel.h
        src/ui/AddConnectionWindow.cpp
        src/ui/AddConnectionWindow.h
        src/ui/ConnectionOptionsViewModel.cpp
        src/ui/ConnectionOptionsViewModel.h
        src/ui/ConnectionOptionsWindow.cpp
        src/ui/ConnectionOptionsWindow.h
    )
    target_include_directories(connectionoptions_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(connectionoptions_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        Qt::Test
    )
    add_test(NAME connectionoptions_tests COMMAND connectionoptions_tests)
    set_tests_properties(connectionoptions_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(helpview_tests
        tests/test_helpview.cpp
        src/ui/HelpView.cpp
        src/ui/HelpView.h
    )
    target_include_directories(helpview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(helpview_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME helpview_tests COMMAND helpview_tests)
    set_tests_properties(helpview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(autoupdatecheck_tests
        tests/test_autoupdatecheck.cpp
        src/ui/AutoUpdateCheck.cc
        src/ui/AutoUpdateCheck.h
    )
    target_include_directories(autoupdatecheck_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(autoupdatecheck_tests PRIVATE
        Qt::Core
        Qt::Network
        Qt::Test
    )
    add_test(NAME autoupdatecheck_tests COMMAND autoupdatecheck_tests)

    add_executable(dataselectionscreen_tests
        tests/test_dataselectionscreen.cpp
        src/ui/dataselectionscreen.cpp
        src/ui/dataselectionscreen.h
        src/ui/dataselectionscreen.ui
        src/ui/GraphTreeWidgetItem.cc
        src/ui/GraphTreeWidgetItem.h
    )
    target_include_directories(dataselectionscreen_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    )
    target_link_libraries(dataselectionscreen_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME dataselectionscreen_tests COMMAND dataselectionscreen_tests)
    set_tests_properties(dataselectionscreen_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(hudcontrol_tests
        tests/test_hudcontrol.cpp
        src/ui/flightdata/HudControl.cpp
        src/ui/flightdata/HudControl.h
        src/services/HudDisplaySettings.cpp
        src/services/HudDisplaySettings.h
    )
    target_include_directories(hudcontrol_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/flightdata
    )
    target_link_libraries(hudcontrol_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME hudcontrol_tests COMMAND hudcontrol_tests)
    set_tests_properties(hudcontrol_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(proximityradar_tests
        tests/test_proximityradar.cpp
        src/uas/Proximity.cpp
        src/uas/Proximity.h
        src/ui/flightdata/ProximityRadarControl.cpp
        src/ui/flightdata/ProximityRadarControl.h
        src/ui/flightdata/ProximityWindow.cpp
        src/ui/flightdata/ProximityWindow.h
    )
    target_include_directories(proximityradar_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/uas
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/flightdata
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(proximityradar_tests PRIVATE
        Threads::Threads
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME proximityradar_tests COMMAND proximityradar_tests)
    set_tests_properties(proximityradar_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(servooutputdecoder_tests
        tests/test_servooutputdecoder.cpp
        src/uas/ServoOutputDecoder.cpp
        src/uas/ServoOutputDecoder.h
    )
    target_include_directories(servooutputdecoder_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(servooutputdecoder_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME servooutputdecoder_tests
        COMMAND servooutputdecoder_tests)

    add_executable(missioncommandcatalog_tests
        tests/test_missioncommandcatalog.cpp
    )
    target_link_libraries(missioncommandcatalog_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME missioncommandcatalog_tests
             COMMAND missioncommandcatalog_tests)

    add_executable(flightplannermissionmodel_tests
        tests/test_flightplannermissionmodel.cpp
        src/ui/flightplanner/FlightPlannerMissionModel.cpp
        src/ui/flightplanner/FlightPlannerMissionModel.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(flightplannermissionmodel_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(flightplannermissionmodel_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightplannermissionmodel_tests
             COMMAND flightplannermissionmodel_tests)

    add_executable(flightplannermissioncodec_tests
        tests/test_flightplannermissioncodec.cpp
        src/ui/flightplanner/FlightPlannerMissionCodec.cpp
        src/ui/flightplanner/FlightPlannerMissionCodec.h
        src/ui/flightplanner/FlightPlannerMissionModel.cpp
        src/ui/flightplanner/FlightPlannerMissionModel.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(flightplannermissioncodec_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(flightplannermissioncodec_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightplannermissioncodec_tests
             COMMAND flightplannermissioncodec_tests)

    add_executable(flightplannernavigation_tests
        tests/test_flightplannernavigation.cpp
        src/ui/flightplanner/FlightPlannerNavigation.cpp
        src/ui/flightplanner/FlightPlannerNavigation.h
    )
    target_include_directories(flightplannernavigation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(flightplannernavigation_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightplannernavigation_tests
             COMMAND flightplannernavigation_tests)

    add_executable(flightplannerroutemetrics_tests
        tests/test_flightplannerroutemetrics.cpp
        src/ui/flightplanner/FlightPlannerRouteMetrics.cpp
        src/ui/flightplanner/FlightPlannerRouteMetrics.h
    )
    target_include_directories(flightplannerroutemetrics_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(flightplannerroutemetrics_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightplannerroutemetrics_tests
             COMMAND flightplannerroutemetrics_tests)

    add_executable(flightplannermeasurement_tests
        tests/test_flightplannermeasurement.cpp
        src/ui/flightplanner/FlightPlannerMeasurement.cpp
        src/ui/flightplanner/FlightPlannerMeasurement.h
        src/ui/map/PlannerMeasurementOverlay.cpp
        src/ui/map/PlannerMeasurementOverlay.h
    )
    target_include_directories(flightplannermeasurement_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(flightplannermeasurement_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME flightplannermeasurement_tests
             COMMAND flightplannermeasurement_tests)
    set_tests_properties(flightplannermeasurement_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(missionelevationprofile_tests
        tests/test_missionelevationprofile.cpp
        src/ui/flightplanner/MissionElevationProfile.cpp
        src/ui/flightplanner/MissionElevationProfile.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(missionelevationprofile_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(missionelevationprofile_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME missionelevationprofile_tests
             COMMAND missionelevationprofile_tests)

    add_executable(surveygridgenerator_tests
        tests/test_surveygridgenerator.cpp
        src/ui/flightplanner/SurveyGridGenerator.cpp
        src/ui/flightplanner/SurveyGridGenerator.h
    )
    target_include_directories(surveygridgenerator_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(surveygridgenerator_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME surveygridgenerator_tests
             COMMAND surveygridgenerator_tests)

    add_executable(surveymissionbuilder_tests
        tests/test_surveymissionbuilder.cpp
        src/ui/flightplanner/SurveyGridGenerator.cpp
        src/ui/flightplanner/SurveyGridGenerator.h
        src/ui/flightplanner/SurveyMissionBuilder.cpp
        src/ui/flightplanner/SurveyMissionBuilder.h
    )
    target_include_directories(surveymissionbuilder_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(surveymissionbuilder_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME surveymissionbuilder_tests
             COMMAND surveymissionbuilder_tests)

    add_executable(surveygriddialog_tests
        tests/test_surveygriddialog.cpp
        src/ui/flightplanner/SurveyGridDialog.cpp
        src/ui/flightplanner/SurveyGridDialog.h
        src/ui/flightplanner/SurveyGridGenerator.cpp
        src/ui/flightplanner/SurveyGridGenerator.h
        src/ui/flightplanner/SurveyMissionBuilder.cpp
        src/ui/flightplanner/SurveyMissionBuilder.h
    )
    target_include_directories(surveygriddialog_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(surveygriddialog_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME surveygriddialog_tests COMMAND surveygriddialog_tests)
    set_tests_properties(surveygriddialog_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(fencerallyfilecodec_tests
        tests/test_fencerallyfilecodec.cpp
        src/ui/flightplanner/FenceRallyFileCodec.cpp
        src/ui/flightplanner/FenceRallyFileCodec.h
        src/ui/flightplanner/FenceRallyModel.cpp
        src/ui/flightplanner/FenceRallyModel.h
    )
    target_include_directories(fencerallyfilecodec_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(fencerallyfilecodec_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME fencerallyfilecodec_tests
             COMMAND fencerallyfilecodec_tests)

    add_executable(qgcplanfilecodec_tests
        tests/test_qgcplanfilecodec.cpp
        src/ui/flightplanner/FenceRallyModel.cpp
        src/ui/flightplanner/FenceRallyModel.h
        src/ui/flightplanner/QgcPlanFileCodec.cpp
        src/ui/flightplanner/QgcPlanFileCodec.h
    )
    target_include_directories(qgcplanfilecodec_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(qgcplanfilecodec_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME qgcplanfilecodec_tests COMMAND qgcplanfilecodec_tests)

    add_executable(flightplannerpolygonmodel_tests
        tests/test_flightplannerpolygonmodel.cpp
        src/ui/flightplanner/FlightPlannerPolygonModel.cpp
        src/ui/flightplanner/FlightPlannerPolygonModel.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(flightplannerpolygonmodel_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(flightplannerpolygonmodel_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightplannerpolygonmodel_tests
             COMMAND flightplannerpolygonmodel_tests)

    add_executable(fencerallycontroller_tests
        tests/test_fencerallycontroller.cpp
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferController.cc
        src/comm/MissionTransferController.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/MissionTransferTransport.h
        src/ui/flightplanner/FenceRallyController.cpp
        src/ui/flightplanner/FenceRallyController.h
        src/ui/flightplanner/FenceRallyFileCodec.cpp
        src/ui/flightplanner/FenceRallyFileCodec.h
        src/ui/flightplanner/FenceRallyModel.cpp
        src/ui/flightplanner/FenceRallyModel.h
        src/ui/flightplanner/FlightPlannerMissionCodec.cpp
        src/ui/flightplanner/FlightPlannerMissionCodec.h
        src/ui/flightplanner/FlightPlannerMissionModel.cpp
        src/ui/flightplanner/FlightPlannerMissionModel.h
        src/ui/flightplanner/FlightPlannerNavigation.cpp
        src/ui/flightplanner/FlightPlannerNavigation.h
        src/ui/flightplanner/FlightPlannerRouteMetrics.cpp
        src/ui/flightplanner/FlightPlannerRouteMetrics.h
        src/ui/flightplanner/MissionElevationProfile.cpp
        src/ui/flightplanner/MissionElevationProfile.h
        src/ui/flightplanner/FlightPlannerPolygonModel.cpp
        src/ui/flightplanner/FlightPlannerPolygonModel.h
        src/ui/flightplanner/QgcPlanFileCodec.cpp
        src/ui/flightplanner/QgcPlanFileCodec.h
        src/ui/flightplanner/FlightPlannerViewModel.cpp
        src/ui/flightplanner/FlightPlannerViewModel.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(fencerallycontroller_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(fencerallycontroller_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME fencerallycontroller_tests
             COMMAND fencerallycontroller_tests)

    add_executable(flightplannerviewmodel_tests
        tests/test_flightplannerviewmodel.cpp
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferController.cc
        src/comm/MissionTransferController.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/MissionTransferTransport.h
        src/ui/flightplanner/FlightPlannerMissionCodec.cpp
        src/ui/flightplanner/FlightPlannerMissionCodec.h
        src/ui/flightplanner/FlightPlannerMissionModel.cpp
        src/ui/flightplanner/FlightPlannerMissionModel.h
        src/ui/flightplanner/FenceRallyModel.cpp
        src/ui/flightplanner/FenceRallyModel.h
        src/ui/flightplanner/FlightPlannerNavigation.cpp
        src/ui/flightplanner/FlightPlannerNavigation.h
        src/ui/flightplanner/FlightPlannerRouteMetrics.cpp
        src/ui/flightplanner/FlightPlannerRouteMetrics.h
        src/ui/flightplanner/MissionElevationProfile.cpp
        src/ui/flightplanner/MissionElevationProfile.h
        src/ui/flightplanner/FlightPlannerPolygonModel.cpp
        src/ui/flightplanner/FlightPlannerPolygonModel.h
        src/ui/flightplanner/QgcPlanFileCodec.cpp
        src/ui/flightplanner/QgcPlanFileCodec.h
        src/ui/flightplanner/FlightPlannerViewModel.cpp
        src/ui/flightplanner/FlightPlannerViewModel.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(flightplannerviewmodel_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(flightplannerviewmodel_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightplannerviewmodel_tests
             COMMAND flightplannerviewmodel_tests)

    add_executable(flightplannerwaypointpanel_tests
        tests/test_flightplannerwaypointpanel.cpp
        src/comm/MissionProtocolCoordinator.cc
        src/comm/MissionProtocolCoordinator.h
        src/comm/MissionTransferController.cc
        src/comm/MissionTransferController.h
        src/comm/MissionTransferService.cc
        src/comm/MissionTransferService.h
        src/comm/MissionTransferTransport.h
        src/ui/flightplanner/FlightPlannerMissionCodec.cpp
        src/ui/flightplanner/FlightPlannerMissionCodec.h
        src/ui/flightplanner/FlightPlannerMissionModel.cpp
        src/ui/flightplanner/FlightPlannerMissionModel.h
        src/ui/flightplanner/FenceRallyModel.cpp
        src/ui/flightplanner/FenceRallyModel.h
        src/ui/flightplanner/FlightPlannerNavigation.cpp
        src/ui/flightplanner/FlightPlannerNavigation.h
        src/ui/flightplanner/FlightPlannerRouteMetrics.cpp
        src/ui/flightplanner/FlightPlannerRouteMetrics.h
        src/ui/flightplanner/MissionElevationProfile.cpp
        src/ui/flightplanner/MissionElevationProfile.h
        src/ui/flightplanner/FlightPlannerPolygonModel.cpp
        src/ui/flightplanner/FlightPlannerPolygonModel.h
        src/ui/flightplanner/QgcPlanFileCodec.cpp
        src/ui/flightplanner/QgcPlanFileCodec.h
        src/ui/flightplanner/FlightPlannerViewModel.cpp
        src/ui/flightplanner/FlightPlannerViewModel.h
        src/ui/flightplanner/FlightPlannerWaypointPanel.cpp
        src/ui/flightplanner/FlightPlannerWaypointPanel.h
        src/ui/flightplanner/WpRow.cpp
        src/ui/flightplanner/WpRow.h
    )
    target_include_directories(flightplannerwaypointpanel_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(flightplannerwaypointpanel_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME flightplannerwaypointpanel_tests
             COMMAND flightplannerwaypointpanel_tests)
    set_tests_properties(flightplannerwaypointpanel_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(dockableview_tests
        tests/test_dockableview.cpp
        src/ui/docking/DockableView.cpp
        src/ui/docking/DockableView.h
    )
    target_include_directories(dockableview_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(dockableview_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME dockableview_tests COMMAND dockableview_tests)
    set_tests_properties(dockableview_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(qmlpluginmanifest_tests
        tests/test_qmlpluginmanifest.cpp
        src/qml/QmlPluginManifest.cpp
        src/qml/QmlPluginManifest.h
    )
    target_include_directories(qmlpluginmanifest_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(qmlpluginmanifest_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME qmlpluginmanifest_tests COMMAND qmlpluginmanifest_tests)

    add_executable(qmlpluginmanager_tests
        tests/test_qmlpluginmanager.cpp
        src/AppPaths.cpp
        src/AppPaths.h
        src/qml/ApmQmlApi.cpp
        src/qml/ApmQmlApi.h
        src/qml/QmlPluginContext.cpp
        src/qml/QmlPluginContext.h
        src/qml/QmlPluginManager.cpp
        src/qml/QmlPluginManager.h
        src/qml/QmlPluginManifest.cpp
        src/qml/QmlPluginManifest.h
        src/ui/configuration/QmlPluginManagerView.cpp
        src/ui/configuration/QmlPluginManagerView.h
    )
    target_include_directories(qmlpluginmanager_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/uas
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    target_link_libraries(qmlpluginmanager_tests PRIVATE
        apm_mission_command_catalog
        Qt::Core
        Qt::Widgets
        Qt::Qml
        Qt::Quick
        Qt::QuickWidgets
        Qt::Test
    )
    add_test(NAME qmlpluginmanager_tests COMMAND qmlpluginmanager_tests)
    set_tests_properties(qmlpluginmanager_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    )

    add_executable(flightdatamissionprogress_tests
        tests/test_flightdatamissionprogress.cpp
        src/ui/flightdata/FlightDataMissionProgress.cpp
        src/ui/flightdata/FlightDataMissionProgress.h
    )
    target_include_directories(flightdatamissionprogress_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(flightdatamissionprogress_tests PRIVATE
        Qt::Core
        Qt::Test
    )
    add_test(NAME flightdatamissionprogress_tests
        COMMAND flightdatamissionprogress_tests)

    add_executable(flightdatamapoverlay_tests
        tests/test_flightdatamapoverlay.cpp
        src/ui/flightdata/FlightDataMapOverlay.cpp
        src/ui/flightdata/FlightDataMapOverlay.h
        src/ui/map/AbstractMapWidget.h
    )
    target_include_directories(flightdatamapoverlay_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/opmapcontrol/src/core
    )
    target_link_libraries(flightdatamapoverlay_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME flightdatamapoverlay_tests COMMAND flightdatamapoverlay_tests)
    set_tests_properties(flightdatamapoverlay_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(preflightchecklist_tests
        tests/test_preflightchecklist.cpp
        src/ui/flightdata/PreflightChecklistModel.cpp
        src/ui/flightdata/PreflightChecklistModel.h
        src/ui/flightdata/PreflightChecklistWidget.cpp
        src/ui/flightdata/PreflightChecklistWidget.h
        src/ui/flightdata/SimpleActionsWidget.cpp
        src/ui/flightdata/SimpleActionsWidget.h
    )
    target_include_directories(preflightchecklist_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(preflightchecklist_tests PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME preflightchecklist_tests COMMAND preflightchecklist_tests)
    set_tests_properties(preflightchecklist_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(flightviews_tests
        tests/test_flightviews.cpp
        src/ui/FlightDataView.cpp
        src/ui/FlightDataView.h
        src/ui/FlightPlannerView.cpp
        src/ui/FlightPlannerView.h
        src/ui/docking/DockableView.cpp
        src/ui/docking/DockableView.h
    )
    target_include_directories(flightviews_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(flightviews_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME flightviews_tests COMMAND flightviews_tests)
    set_tests_properties(flightviews_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(uasactionswidget_tests
        tests/test_uasactionswidget.cpp
        src/ui/uas/UASActionCatalog.h
        src/ui/uas/UASActionsLayout.h
        src/ui/uas/UASActionsWidget.ui
    )
    target_include_directories(uasactionswidget_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/comm
        ${MAVLINKPATH}
        ${MAVLINKPATH}/${MAVLINK_CONF}
    )
    set_property(TARGET uasactionswidget_tests PROPERTY AUTOUIC_SEARCH_PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/uas"
    )
    target_link_libraries(uasactionswidget_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME uasactionswidget_tests COMMAND uasactionswidget_tests)
    set_tests_properties(uasactionswidget_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    foreach(signing_test IN ITEMS mavlinksigningclock mavlinksigningsession mavlinksigningmanager mavauthkeystore mavauthkeyservice mavlinksigningprofiles)
        add_executable(${signing_test}_tests tests/test_${signing_test}.cpp)
        target_link_libraries(${signing_test}_tests PRIVATE apm_mavlink_signing Qt::Test)
        add_test(NAME ${signing_test}_tests COMMAND ${signing_test}_tests)
        set_tests_properties(${signing_test}_tests PROPERTIES TIMEOUT 120)
    endforeach()
    target_sources(mavlinksigningsession_tests PRIVATE src/comm/MAVLinkFrameParser.cpp)
    target_link_libraries(mavauthkeystore_tests PRIVATE OpenSSL::Crypto)
    target_include_directories(mavlinksigningsession_tests PRIVATE
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})
    target_include_directories(mavlinksigningmanager_tests PRIVATE
        ${MAVLINKPATH} ${MAVLINKPATH}/${MAVLINK_CONF})

    add_executable(missionplannerstyle_tests
        tests/test_missionplannerstyle.cpp
    )
    target_compile_definitions(missionplannerstyle_tests PRIVATE
        APM_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(missionplannerstyle_tests PRIVATE
        Qt::Widgets
        Qt::Test
    )
    add_test(NAME missionplannerstyle_tests COMMAND missionplannerstyle_tests)
    set_tests_properties(missionplannerstyle_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

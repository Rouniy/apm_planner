# QtWidgets-only static build of upstream KDDockWidgets v1.4.0.
# Upstream identity and local patch policy are recorded in PATCHES.md.

set(KDDOCKWIDGETS_SOURCES
    src/Config.cpp
    src/DockWidgetBase.cpp
    src/FocusScope.cpp
    src/FrameworkWidgetFactory.cpp
    src/LayoutSaver.cpp
    src/MainWindow.cpp
    src/MainWindowBase.cpp
    src/MainWindowMDI.cpp
    src/kddockwidgets_resources.qrc
    src/private/DebugWindow.cpp
    src/private/DockRegistry.cpp
    src/private/DragController.cpp
    src/private/Draggable.cpp
    src/private/DropArea.cpp
    src/private/DropAreaWithCentralFrame.cpp
    src/private/DropIndicatorOverlayInterface.cpp
    src/private/FloatingWindow.cpp
    src/private/Frame.cpp
    src/private/LayoutWidget.cpp
    src/private/Logging.cpp
    src/private/MDILayoutWidget.cpp
    src/private/MultiSplitter.cpp
    src/private/ObjectViewer.cpp
    src/private/Position.cpp
    src/private/SideBar.cpp
    src/private/TabWidget.cpp
    src/private/TitleBar.cpp
    src/private/WidgetResizeHandler.cpp
    src/private/WindowBeingDragged.cpp
    src/private/indicators/ClassicIndicators.cpp
    src/private/indicators/ClassicIndicatorsWindow.cpp
    src/private/indicators/NullIndicators.cpp
    src/private/indicators/SegmentedIndicators.cpp
    src/private/multisplitter/Item.cpp
    src/private/multisplitter/ItemFreeContainer.cpp
    src/private/multisplitter/Logging.cpp
    src/private/multisplitter/MultiSplitterConfig.cpp
    src/private/multisplitter/Separator.cpp
    src/private/multisplitter/Separator_qwidget.cpp
    src/private/multisplitter/Widget.cpp
    src/private/multisplitter/Widget_qwidget.cpp
    src/private/widgets/DockWidget.cpp
    src/private/widgets/FloatingWindowWidget.cpp
    src/private/widgets/FrameWidget.cpp
    src/private/widgets/QWidgetAdapter_widgets.cpp
    src/private/widgets/SideBarWidget.cpp
    src/private/widgets/TabBarWidget.cpp
    src/private/widgets/TabWidgetWidget.cpp
    src/private/widgets/TitleBarWidget.cpp
)

# AUTOMOC must see every upstream header that owns a Q_OBJECT/Q_NAMESPACE. Keep
# this scoped to the imported QtWidgets directories; no QtQuick tree is vendored.
file(GLOB KDDOCKWIDGETS_HEADERS CONFIGURE_DEPENDS
    src/*.h
    src/private/*.h
    src/private/indicators/*.h
    src/private/multisplitter/*.h
    src/private/widgets/*.h
)
list(FILTER KDDOCKWIDGETS_HEADERS EXCLUDE REGEX "DockWidgetQuick\\.h$")

add_library(apm_kddockwidgets STATIC
    ${KDDOCKWIDGETS_SOURCES}
    ${KDDOCKWIDGETS_HEADERS}
)
add_library(KDAB::kddockwidgets ALIAS apm_kddockwidgets)

set_target_properties(apm_kddockwidgets PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(apm_kddockwidgets PUBLIC cxx_std_17)
target_include_directories(apm_kddockwidgets
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/fwd_headers>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(apm_kddockwidgets
    PUBLIC
        KDDOCKWIDGETS_STATICLIB
        KDDOCKWIDGETS_QTWIDGETS
    PRIVATE
        QT_NO_SIGNALS_SLOTS_KEYWORDS
        QT_USE_QSTRINGBUILDER
        QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
        QT_STRICT_ITERATORS
        QT_NO_KEYWORDS
        QT_NO_FOREACH
        QT_NO_CAST_TO_ASCII
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_FROM_BYTEARRAY
        QT_NO_URL_CAST_FROM_STRING
)
target_link_libraries(apm_kddockwidgets PUBLIC Qt::Widgets)

if(WIN32)
    if(NOT TARGET ${APM_QT_PACKAGE}::GuiPrivate)
        message(FATAL_ERROR
            "KDDockWidgets v1.4.0 on Windows requires ${APM_QT_PACKAGE}::GuiPrivate")
    endif()
    target_link_libraries(apm_kddockwidgets PRIVATE
        ${APM_QT_PACKAGE}::GuiPrivate
        dwmapi
    )
elseif(UNIX AND NOT APPLE AND QT_VERSION_MAJOR EQUAL 5)
    find_package(Qt5 REQUIRED COMPONENTS X11Extras)
    target_link_libraries(apm_kddockwidgets PUBLIC Qt5::X11Extras)
endif()

install(FILES
    LICENSE.txt
    LICENSE.GPL.txt
    CONTRIBUTORS.txt
    README.md
    DESTINATION "${APM_INSTALL_RESOURCE_SUBDIR}/licenses/KDDockWidgets"
)
install(DIRECTORY LICENSES
    DESTINATION "${APM_INSTALL_RESOURCE_SUBDIR}/licenses/KDDockWidgets"
)

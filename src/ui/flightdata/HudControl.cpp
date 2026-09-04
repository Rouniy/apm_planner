#include "HudControl.h"

#include "services/HudDisplaySettings.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kEase = 0.4;
constexpr double kPi = 3.14159265358979323846;

double normalizedHeading(double value)
{
    value = std::fmod(value, 360.0);
    return value < 0.0 ? value + 360.0 : value;
}

bool approach(double &value, double target)
{
    const double delta = target - value;
    if (std::abs(delta) < 0.001) {
        value = target;
        return false;
    }
    value += delta * kEase;
    return true;
}

bool approachAngle(double &value, double target)
{
    double delta = std::fmod(target - value + 540.0, 360.0) - 180.0;
    if (std::abs(delta) < 0.001) {
        value = normalizedHeading(target);
        return false;
    }
    value = normalizedHeading(value + delta * kEase);
    return true;
}

QColor hudGreen()
{
    return QColor(76, 255, 76);
}
}

HudControl::HudControl(QWidget *parent, HudDisplaySettings *displaySettings,
                       bool loadUserSettings)
    : QWidget(parent),
      m_easeTimer(new QTimer(this)),
      m_displaySettings(loadUserSettings
                            ? (displaySettings ? displaySettings
                                               : HudDisplaySettings::instance())
                            : nullptr)
{
    setObjectName(QStringLiteral("Hud"));
    setMinimumSize(240, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    if (loadUserSettings) {
        loadDisplaySettings();
    }
    if (m_displaySettings) {
        m_overlayEnabled = m_displaySettings->overlayEnabled();
        connect(m_displaySettings, &HudDisplaySettings::overlayEnabledChanged,
                this, &HudControl::applyOverlayEnabled);
    }
    m_easeTimer->setInterval(16);
    connect(m_easeTimer, &QTimer::timeout, this, &HudControl::stepEase);
    m_easeTimer->start();
}

QSize HudControl::sizeHint() const
{
    return m_sixteenByNine ? QSize(480, 270) : QSize(440, 330);
}

QRect HudControl::contentViewport() const
{
    // A decoded camera frame owns the complete render surface. Applying the
    // synthetic HUD aspect ratio here would crop the source and differs from
    // Mission Planner's OSD-video renderer.
    if (!m_videoBackground.isNull()) {
        return rect();
    }
    const double ratio = m_sixteenByNine ? (16.0 / 9.0) : (4.0 / 3.0);
    int contentWidth = width();
    int contentHeight = qRound(contentWidth / ratio);
    if (contentHeight > height()) {
        contentHeight = height();
        contentWidth = qRound(contentHeight * ratio);
    }
    return QRect((width() - contentWidth) / 2,
                 (height() - contentHeight) / 2,
                 contentWidth, contentHeight);
}

void HudControl::changed()
{
    emit telemetryChanged();
    update();
}

#define DEFINE_DOUBLE_SETTER(Method, Member) \
    void HudControl::Method(double value) \
    { \
        if (!std::isfinite(value) || Member == value) return; \
        Member = value; \
        changed(); \
    }

#define DEFINE_INT_SETTER(Method, Member) \
    void HudControl::Method(int value) \
    { \
        if (Member == value) return; \
        Member = value; \
        changed(); \
    }

#define DEFINE_BOOL_SETTER(Method, Member) \
    void HudControl::Method(bool value) \
    { \
        if (Member == value) return; \
        Member = value; \
        changed(); \
    }

DEFINE_DOUBLE_SETTER(setRoll, m_roll)
DEFINE_DOUBLE_SETTER(setPitch, m_pitch)
DEFINE_DOUBLE_SETTER(setYaw, m_yaw)
DEFINE_DOUBLE_SETTER(setAlt, m_alt)
DEFINE_DOUBLE_SETTER(setAirSpeed, m_airSpeed)
DEFINE_DOUBLE_SETTER(setGroundSpeed, m_groundSpeed)
DEFINE_DOUBLE_SETTER(setVerticalSpeed, m_verticalSpeed)
DEFINE_DOUBLE_SETTER(setSatCount, m_satCount)
DEFINE_INT_SETTER(setGpsFixType, m_gpsFixType)
DEFINE_BOOL_SETTER(setArmed, m_armed)
DEFINE_BOOL_SETTER(setPrearmOk, m_prearmOk)
DEFINE_DOUBLE_SETTER(setBatteryVoltage, m_batteryVoltage)
DEFINE_INT_SETTER(setBatteryRemaining, m_batteryRemaining)
DEFINE_BOOL_SETTER(setShowIcons, m_showIcons)
DEFINE_BOOL_SETTER(setRussian, m_russian)
DEFINE_INT_SETTER(setBatteryCells, m_batteryCells)
DEFINE_BOOL_SETTER(setGroundBrown, m_groundBrown)
DEFINE_BOOL_SETTER(setSixteenByNine, m_sixteenByNine)
DEFINE_BOOL_SETTER(setDisplayHeading, m_displayHeading)
DEFINE_BOOL_SETTER(setDisplaySpeed, m_displaySpeed)
DEFINE_BOOL_SETTER(setDisplayAlt, m_displayAlt)
DEFINE_BOOL_SETTER(setDisplayRollPitch, m_displayRollPitch)
DEFINE_BOOL_SETTER(setDisplayGps, m_displayGps)
DEFINE_BOOL_SETTER(setDisplayBattery, m_displayBattery)
DEFINE_BOOL_SETTER(setDisplayBattery2, m_displayBattery2)
DEFINE_BOOL_SETTER(setDisplayEkf, m_displayEkf)
DEFINE_BOOL_SETTER(setDisplayVibe, m_displayVibe)
DEFINE_BOOL_SETTER(setDisplayPrearm, m_displayPrearm)
DEFINE_BOOL_SETTER(setDisplayAoa, m_displayAoa)
DEFINE_BOOL_SETTER(setDisplayXTrack, m_displayXTrack)
DEFINE_BOOL_SETTER(setDisplayConnection, m_displayConnection)
DEFINE_DOUBLE_SETTER(setNavBearing, m_navBearing)
DEFINE_DOUBLE_SETTER(setCurrentAmps, m_currentAmps)
DEFINE_DOUBLE_SETTER(setTargetAlt, m_targetAlt)
DEFINE_DOUBLE_SETTER(setTargetSpeed, m_targetSpeed)
DEFINE_DOUBLE_SETTER(setWindDir, m_windDir)
DEFINE_DOUBLE_SETTER(setWindVel, m_windVel)
DEFINE_DOUBLE_SETTER(setAoa, m_aoa)
DEFINE_DOUBLE_SETTER(setSsa, m_ssa)
DEFINE_DOUBLE_SETTER(setXTrackError, m_xTrackError)
DEFINE_DOUBLE_SETTER(setTurnRate, m_turnRate)
DEFINE_DOUBLE_SETTER(setWpDist, m_wpDist)
DEFINE_INT_SETTER(setWpNo, m_wpNo)
DEFINE_DOUBLE_SETTER(setBatteryVoltage2, m_batteryVoltage2)
DEFINE_INT_SETTER(setBatteryRemaining2, m_batteryRemaining2)
DEFINE_DOUBLE_SETTER(setCurrentAmps2, m_currentAmps2)
DEFINE_DOUBLE_SETTER(setThrottlePercent, m_throttlePercent)
DEFINE_BOOL_SETTER(setFailsafe, m_failsafe)
DEFINE_BOOL_SETTER(setSafetyActive, m_safetyActive)
DEFINE_DOUBLE_SETTER(setLinkQuality, m_linkQuality)
DEFINE_INT_SETTER(setStatusMessageSeverity, m_statusMessageSeverity)

#undef DEFINE_DOUBLE_SETTER
#undef DEFINE_INT_SETTER
#undef DEFINE_BOOL_SETTER

void HudControl::setOverlayEnabled(bool value)
{
    if (m_displaySettings) {
        m_displaySettings->setOverlayEnabled(value);
    } else {
        applyOverlayEnabled(value);
    }
}

void HudControl::applyOverlayEnabled(bool enabled)
{
    if (m_overlayEnabled == enabled) {
        return;
    }
    m_overlayEnabled = enabled;
    changed();
}

void HudControl::setMode(const QString &value)
{
    if (m_mode == value) {
        return;
    }
    m_mode = value;
    changed();
}

void HudControl::setCustomItemsText(const QString &value)
{
    if (m_customItemsText == value) {
        return;
    }
    m_customItemsText = value;
    changed();
}

void HudControl::setStatusMessage(const QString &value)
{
    if (m_statusMessage == value) {
        return;
    }
    m_statusMessage = value;
    changed();
}

void HudControl::setVideoBackground(const QImage &image)
{
    m_videoBackground = image;
    update();
}

void HudControl::snapToValues()
{
    m_easedRoll = m_roll;
    m_easedPitch = m_pitch;
    m_easedYaw = normalizedHeading(m_yaw);
    m_easedAlt = m_alt;
    m_easedAirSpeed = m_airSpeed;
    m_easedGroundSpeed = m_groundSpeed;
    m_easedVerticalSpeed = m_verticalSpeed;
    m_easeInitialized = true;
    update();
}

void HudControl::stepEase()
{
    if (!m_easeInitialized) {
        snapToValues();
        return;
    }
    bool moving = false;
    moving |= approach(m_easedRoll, m_roll);
    moving |= approach(m_easedPitch, m_pitch);
    moving |= approachAngle(m_easedYaw, m_yaw);
    moving |= approach(m_easedAlt, m_alt);
    moving |= approach(m_easedAirSpeed, m_airSpeed);
    moving |= approach(m_easedGroundSpeed, m_groundSpeed);
    moving |= approach(m_easedVerticalSpeed, m_verticalSpeed);
    if (moving) {
        update();
    }
}

void HudControl::loadDisplaySettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("FlightData/Hud"));
    m_showIcons = settings.value(QStringLiteral("ShowIcons"), true).toBool();
    m_russian = settings.value(QStringLiteral("Russian"), false).toBool();
    m_groundBrown = settings.value(QStringLiteral("GroundBrown"), false).toBool();
    m_sixteenByNine = settings.value(QStringLiteral("SixteenByNine"), false).toBool();
    m_displayHeading = settings.value(QStringLiteral("DisplayHeading"), true).toBool();
    m_displaySpeed = settings.value(QStringLiteral("DisplaySpeed"), true).toBool();
    m_displayAlt = settings.value(QStringLiteral("DisplayAlt"), true).toBool();
    m_displayRollPitch = settings.value(QStringLiteral("DisplayRollPitch"), true).toBool();
    m_displayGps = settings.value(QStringLiteral("DisplayGps"), true).toBool();
    m_displayBattery = settings.value(QStringLiteral("DisplayBattery"), true).toBool();
    m_displayBattery2 = settings.value(QStringLiteral("DisplayBattery2"), true).toBool();
    m_displayEkf = settings.value(QStringLiteral("DisplayEkf"), true).toBool();
    m_displayVibe = settings.value(QStringLiteral("DisplayVibe"), true).toBool();
    m_displayPrearm = settings.value(QStringLiteral("DisplayPrearm"), true).toBool();
    m_displayAoa = settings.value(QStringLiteral("DisplayAoa"), false).toBool();
    m_displayXTrack = settings.value(QStringLiteral("DisplayXTrack"), true).toBool();
    m_displayConnection = settings.value(QStringLiteral("DisplayConnection"), true).toBool();
    settings.endGroup();
}

void HudControl::saveDisplaySetting(const QString &name, bool value)
{
    QSettings settings;
    settings.setValue(QStringLiteral("FlightData/Hud/%1").arg(name), value);
}

void HudControl::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    auto addToggle = [this](QMenu *parent, const QString &text, const QString &setting,
                            bool checked, void (HudControl::*setter)(bool)) {
        QAction *action = parent->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this,
                [this, setting, setter](bool value) {
                    (this->*setter)(value);
                    saveDisplaySetting(setting, value);
                });
    };

    QAction *overlayAction = menu.addAction(tr("HUD overlay"));
    overlayAction->setCheckable(true);
    overlayAction->setChecked(m_overlayEnabled);
    connect(overlayAction, &QAction::toggled,
            this, &HudControl::setOverlayEnabled);
    addToggle(&menu, tr("16:9 aspect ratio"), QStringLiteral("SixteenByNine"),
              m_sixteenByNine, &HudControl::setSixteenByNine);
    addToggle(&menu, tr("Russian HUD"), QStringLiteral("Russian"),
              m_russian, &HudControl::setRussian);
    addToggle(&menu, tr("Brown ground"), QStringLiteral("GroundBrown"),
              m_groundBrown, &HudControl::setGroundBrown);
    addToggle(&menu, tr("Show icons"), QStringLiteral("ShowIcons"),
              m_showIcons, &HudControl::setShowIcons);
    menu.addSeparator();
    QMenu *items = menu.addMenu(tr("HUD Items"));
    addToggle(items, tr("Heading"), QStringLiteral("DisplayHeading"), m_displayHeading,
              &HudControl::setDisplayHeading);
    addToggle(items, tr("Speed"), QStringLiteral("DisplaySpeed"), m_displaySpeed,
              &HudControl::setDisplaySpeed);
    addToggle(items, tr("Alt"), QStringLiteral("DisplayAlt"), m_displayAlt,
              &HudControl::setDisplayAlt);
    addToggle(items, tr("Connection"), QStringLiteral("DisplayConnection"),
              m_displayConnection, &HudControl::setDisplayConnection);
    addToggle(items, tr("X-Track"), QStringLiteral("DisplayXTrack"), m_displayXTrack,
              &HudControl::setDisplayXTrack);
    addToggle(items, tr("Roll/Pitch"), QStringLiteral("DisplayRollPitch"),
              m_displayRollPitch, &HudControl::setDisplayRollPitch);
    addToggle(items, tr("GPS"), QStringLiteral("DisplayGps"), m_displayGps,
              &HudControl::setDisplayGps);
    addToggle(items, tr("Battery"), QStringLiteral("DisplayBattery"), m_displayBattery,
              &HudControl::setDisplayBattery);
    addToggle(items, tr("Battery2"), QStringLiteral("DisplayBattery2"), m_displayBattery2,
              &HudControl::setDisplayBattery2);
    addToggle(items, tr("EKF"), QStringLiteral("DisplayEkf"), m_displayEkf,
              &HudControl::setDisplayEkf);
    addToggle(items, tr("Vibe"), QStringLiteral("DisplayVibe"), m_displayVibe,
              &HudControl::setDisplayVibe);
    addToggle(items, tr("Prearm Status"), QStringLiteral("DisplayPrearm"),
              m_displayPrearm, &HudControl::setDisplayPrearm);
    addToggle(items, tr("AOA"), QStringLiteral("DisplayAoa"), m_displayAoa,
              &HudControl::setDisplayAoa);
    menu.exec(event->globalPos());
}

void HudControl::mousePressEvent(QMouseEvent *event)
{
    const QPointF position = event->pos() - contentViewport().topLeft();
    if (m_ekfRect.contains(position)) {
        emit indicatorClicked(QStringLiteral("EKF"));
    } else if (m_vibeRect.contains(position)) {
        emit indicatorClicked(QStringLiteral("Vibe"));
    } else if (m_prearmRect.contains(position)) {
        emit indicatorClicked(QStringLiteral("Prearm"));
    }
    QWidget::mousePressEvent(event);
}

void HudControl::drawHaloText(QPainter &painter, const QPointF &at,
                              const QString &text, const QColor &color,
                              double fontSize, Qt::Alignment alignment) const
{
    QFont font = painter.font();
    font.setPixelSize(qMax(8, qRound(fontSize)));
    font.setBold(true);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    QRectF bounds = metrics.boundingRect(text);
    QPointF origin = at;
    if (alignment & Qt::AlignHCenter) {
        origin.rx() -= bounds.width() / 2.0;
    } else if (alignment & Qt::AlignRight) {
        origin.rx() -= bounds.width();
    }
    origin.ry() += metrics.ascent();
    painter.setPen(Qt::black);
    for (const QPointF &offset : {QPointF(-1, 0), QPointF(1, 0),
                                  QPointF(0, -1), QPointF(0, 1)}) {
        painter.drawText(origin + offset, text);
    }
    painter.setPen(color);
    painter.drawText(origin, text);
}

QString HudControl::gpsFixText() const
{
    const int satellites = qRound(m_satCount);
    if (m_gpsFixType <= 1) return tr("GPS: No Fix");
    if (m_gpsFixType == 2) return tr("GPS: 2D Fix (%1)").arg(satellites);
    if (m_gpsFixType == 3) return tr("GPS: 3D Fix (%1)").arg(satellites);
    if (m_gpsFixType == 4) return tr("GPS: 3D DGPS (%1)").arg(satellites);
    if (m_gpsFixType == 5) return tr("GPS: RTK Float (%1)").arg(satellites);
    return tr("GPS: RTK Fixed (%1)").arg(satellites);
}

void HudControl::drawHeadingTape(QPainter &painter, const QRectF &rect,
                                 double fontSize) const
{
    const double headHeight = qBound(16.0, rect.height() / 14.0, 48.0);
    painter.fillRect(QRectF(0, 0, rect.width(), headHeight), QColor(0, 0, 0, 140));
    painter.setPen(QPen(Qt::white, 1.5));
    const double spacing = (rect.width() - 10.0) / 120.0;
    const int current = qRound(normalizedHeading(m_easedYaw));
    for (int delta = -60; delta <= 60; ++delta) {
        const int heading = (current + delta + 720) % 360;
        const double x = rect.width() / 2.0 + delta * spacing;
        if (heading % 5 == 0) {
            painter.drawLine(QPointF(x, headHeight - 5), QPointF(x, headHeight - 10));
        }
        if (heading % 15 == 0) {
            QString label;
            switch (heading) {
            case 0: label = QStringLiteral("N"); break;
            case 45: label = QStringLiteral("NE"); break;
            case 90: label = QStringLiteral("E"); break;
            case 135: label = QStringLiteral("SE"); break;
            case 180: label = QStringLiteral("S"); break;
            case 225: label = QStringLiteral("SW"); break;
            case 270: label = QStringLiteral("W"); break;
            case 315: label = QStringLiteral("NW"); break;
            default: label = QStringLiteral("%1").arg(heading, 3, 10, QLatin1Char('0')); break;
            }
            drawHaloText(painter, QPointF(x, 1), label, Qt::white, fontSize,
                         Qt::AlignHCenter);
        }
    }
    const double navDelta = std::fmod(m_navBearing - m_easedYaw + 540.0, 360.0) - 180.0;
    if (std::abs(navDelta) >= 4.0 && std::abs(navDelta) <= 60.0) {
        const double x = rect.width() / 2.0 + navDelta * spacing;
        painter.setPen(QPen(Qt::green, 5));
        painter.drawLine(QPointF(x, 0), QPointF(x, headHeight));
    }
    const double boxWidth = fontSize * 3.0;
    const QRectF box(rect.width() / 2.0 - boxWidth / 2.0, 0, boxWidth, headHeight);
    painter.fillRect(box, QColor(255, 255, 255, 220));
    painter.setPen(QPen(Qt::black, 1.5));
    painter.drawRect(box);
    QFont font = painter.font();
    font.setPixelSize(qMax(8, qRound(fontSize)));
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(box, Qt::AlignCenter,
                     QStringLiteral("%1").arg(current, 3, 10, QLatin1Char('0')));
}

void HudControl::drawRollArc(QPainter &painter, const QRectF &rect,
                             double headHeight) const
{
    const double unit = qMin(rect.width(), rect.height());
    const QPointF center(rect.width() / 2.0, rect.height() / 2.0);
    const double radius = qMax(0.0, qMin(unit * 0.46,
                                        center.y() - headHeight - unit * 0.05));
    painter.save();
    painter.translate(center);
    painter.rotate(-m_easedRoll);
    painter.translate(-center);
    painter.setPen(QPen(Qt::white, 1.5));
    const int marks[] = {-60, -45, -30, -20, -10, 0, 10, 20, 30, 45, 60};
    for (int angle : marks) {
        const double radians = (angle - 90) * kPi / 180.0;
        const double length = angle == 0 ? unit * 0.028 : unit * 0.016;
        painter.drawLine(center + QPointF(radius * std::cos(radians), radius * std::sin(radians)),
                         center + QPointF((radius - length) * std::cos(radians),
                                          (radius - length) * std::sin(radians)));
    }
    QRectF arcRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
    painter.drawArc(arcRect, 30 * 16, 120 * 16);
    painter.restore();

    const double pointer = unit * 0.026;
    painter.setPen(QPen(Qt::red, qMax(2.0, unit * 0.005), Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(center.x(), center.y() - radius),
                     QPointF(center.x() - pointer * 0.7, center.y() - radius + pointer));
    painter.drawLine(QPointF(center.x(), center.y() - radius),
                     QPointF(center.x() + pointer * 0.7, center.y() - radius + pointer));
}

void HudControl::drawScrollTape(QPainter &painter, const QRectF &rect, double value,
                                double target, bool labelsOnLeft, double fontSize) const
{
    painter.fillRect(rect, QColor(0, 0, 0, 140));
    painter.setPen(QPen(Qt::white, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect);
    constexpr double range = 26.0;
    const double spacing = rect.height() / range;
    const double middle = rect.center().y();
    painter.save();
    painter.setClipRect(rect);
    for (int item = static_cast<int>(std::floor(value - range / 2.0));
         item <= static_cast<int>(std::ceil(value + range / 2.0)); ++item) {
        if (item % 5 != 0) continue;
        const double y = middle - (item - value) * spacing;
        if (labelsOnLeft) {
            painter.drawLine(QPointF(rect.left(), y), QPointF(rect.left() + 8, y));
            drawHaloText(painter, QPointF(rect.left() + 10, y - fontSize / 2.0),
                         QString::number(item), Qt::white, fontSize - 1);
        } else {
            painter.drawLine(QPointF(rect.right() - 8, y), QPointF(rect.right(), y));
            drawHaloText(painter, QPointF(rect.left() + 2, y - fontSize / 2.0),
                         QString::number(item), Qt::white, fontSize - 1);
        }
    }
    if (target != 0.0 && std::abs(target - value) < range / 2.0) {
        const double y = middle - (target - value) * spacing;
        painter.setPen(QPen(Qt::green, 4));
        painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
    painter.restore();
    const QRectF valueBox(rect.left(), middle - (fontSize + 6) / 2.0,
                          rect.width(), fontSize + 6);
    painter.fillRect(valueBox, QColor(0, 0, 0, 210));
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawRect(valueBox);
    drawHaloText(painter, QPointF(valueBox.center().x(), valueBox.top()),
                 QString::number(qRound(value)), QColor(240, 248, 255), fontSize,
                 Qt::AlignHCenter);
}

void HudControl::drawVsi(QPainter &painter, const QRectF &altRect) const
{
    painter.save();
    const double width = altRect.width() / 4.0;
    const double left = altRect.left() - width;
    QPainterPath outline;
    outline.moveTo(altRect.left(), altRect.top());
    outline.lineTo(left, altRect.top() + width);
    outline.lineTo(left, altRect.bottom() - width);
    outline.lineTo(altRect.left(), altRect.bottom());
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawPath(outline);
    const double scaled = qBound(-1.0, m_easedVerticalSpeed / 12.0, 1.0)
        * (altRect.height() / 2.0 - 4.0);
    QPolygonF needle;
    needle << QPointF(altRect.left(), altRect.center().y())
           << QPointF(left + 2, altRect.center().y() - scaled)
           << QPointF(altRect.left(), altRect.center().y() - scaled);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::blue);
    painter.drawPolygon(needle);
    painter.restore();
}

void HudControl::drawXTrack(QPainter &painter, const QRectF &rect,
                            double headHeight) const
{
    const double spacing = rect.width() / 30.0;
    const double xtrack = qBound(-40.0, m_xTrackError, 40.0);
    const double location = xtrack / 20.0 * spacing;
    const double center = rect.width() / 10.0;
    const double top = headHeight + 5;
    const double bottom = headHeight + rect.height() / 10.0;
    painter.setPen(QPen(std::abs(xtrack) == 40.0 ? QColor(0, 128, 0, 128) : Qt::green, 2));
    painter.drawLine(QPointF(center + location, top), QPointF(center + location, bottom));
    painter.setPen(QPen(Qt::white, 2));
    for (int index = -2; index <= 2; ++index) {
        const double x = center + spacing * index;
        const double padding = index == 0 ? 0 : 10;
        painter.drawLine(QPointF(x, top + padding), QPointF(x, bottom - padding));
    }
    const double turnY = bottom + 10;
    painter.setPen(QPen(Qt::white, 4));
    for (int index : {-2, 0, 2}) {
        const double x = center + spacing * index - spacing / 2.0;
        painter.drawLine(QPointF(x, turnY), QPointF(x + spacing, turnY));
    }
    const double turn = qBound(-6.0, m_turnRate, 6.0);
    const double turnLocation = turn / 12.0 * spacing * 4.0;
    painter.setPen(QPen(std::abs(turn) == 6.0 ? QColor(0, 128, 0, 128) : Qt::green, 4));
    painter.drawLine(QPointF(center + turnLocation - spacing / 2.0, turnY + 3),
                     QPointF(center + turnLocation + spacing / 2.0, turnY + 3));
    painter.drawLine(QPointF(center + turnLocation, turnY + 3),
                     QPointF(center + turnLocation, turnY + 13));
}

void HudControl::drawAoaSsa(QPainter &painter, const QRectF &rect) const
{
    painter.save();
    const double left = rect.width() - rect.width() / 6.0;
    const double top = rect.height() * 0.55;
    const double barWidth = rect.width() / 25.0;
    const double barHeight = rect.height() / 5.0;
    painter.fillRect(QRectF(left, top, barWidth, barHeight * 0.1), Qt::red);
    painter.fillRect(QRectF(left, top + barHeight * 0.1, barWidth, barHeight * 0.3), Qt::yellow);
    painter.fillRect(QRectF(left, top + barHeight * 0.4, barWidth, barHeight * 0.5), Qt::green);
    painter.fillRect(QRectF(left, top + barHeight * 0.9, barWidth, barHeight * 0.1), Qt::blue);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(left, top, barWidth, barHeight));
    const double indicator = qBound(0.0,
        barHeight * 0.9 - (m_aoa / 25.0) * (barHeight * 0.8), barHeight);
    QPolygonF arrow;
    arrow << QPointF(left + barWidth / 5.0, top + indicator)
          << QPointF(left - barWidth * 0.3, top + indicator + barWidth / 2.0)
          << QPointF(left - barWidth * 0.3, top + indicator - barWidth / 2.0);
    painter.setBrush(Qt::black);
    painter.drawPolygon(arrow);
    drawHaloText(painter, QPointF(left - 4, top + barHeight + 2),
                 tr("AOA %1  SSA %2").arg(m_aoa, 0, 'f', 1).arg(m_ssa, 0, 'f', 1),
                 Qt::white, qBound(8.0, rect.height() / 40.0, 16.0), Qt::AlignRight);
    painter.restore();
}

void HudControl::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Qt::black);
    const QRect viewport = contentViewport();
    if (viewport.isEmpty() || !m_overlayEnabled) {
        m_ekfRect = m_vibeRect = m_prearmRect = QRectF();
        return;
    }

    painter.save();
    painter.setClipRect(viewport);
    painter.translate(viewport.topLeft());
    const QRectF content(0, 0, viewport.width(), viewport.height());
    const double width = content.width();
    const double height = content.height();
    const double unit = qMin(width, height);
    const double fontSize = qBound(9.0, unit / 28.0, 30.0);
    const double centerX = width / 2.0;
    const double centerY = height / 2.0;
    const double degreesToPixels = height / 65.0;
    const double headHeight = qBound(16.0, height / 14.0, 48.0);

    if (!m_videoBackground.isNull()) {
        painter.drawImage(content, m_videoBackground);
    }

    painter.save();
    painter.setClipRect(QRectF(0, headHeight, width, height - headHeight));
    painter.translate(centerX, centerY);
    painter.rotate(m_russian ? 0.0 : -m_easedRoll);
    const double big = qMax(width, height) * 2.0;
    const double pitchOffset = m_easedPitch * degreesToPixels;
    if (m_videoBackground.isNull()) {
        QLinearGradient sky(0, -big, 0, pitchOffset);
        sky.setColorAt(0, QColor(58, 111, 176));
        sky.setColorAt(1, QColor(127, 179, 224));
        painter.fillRect(QRectF(-big, -big, big * 2, big + pitchOffset), sky);
        QLinearGradient ground(0, pitchOffset, 0, big);
        ground.setColorAt(0, m_groundBrown ? QColor(147, 78, 1) : QColor(155, 184, 36));
        ground.setColorAt(1, m_groundBrown ? QColor(60, 33, 4) : QColor(65, 79, 7));
        painter.fillRect(QRectF(-big, pitchOffset, big * 2, big * 2 - pitchOffset), ground);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawLine(QPointF(-big, pitchOffset), QPointF(big, pitchOffset));
    }
    if (m_displayRollPitch) {
        painter.setPen(QPen(Qt::white, 1));
        for (int angle = -90; angle <= 90; angle += 5) {
            if (angle == 0) continue;
            const double y = (m_easedPitch - angle) * degreesToPixels;
            if (std::abs(y) > height * 0.38) continue;
            const bool major = angle % 10 == 0;
            const double length = major ? unit * 0.11 : unit * 0.075;
            painter.drawLine(QPointF(-length / 2.0, y), QPointF(length / 2.0, y));
            if (major) {
                drawHaloText(painter, QPointF(length / 2.0 + 3, y - fontSize),
                             QString::number(std::abs(angle)), Qt::white, fontSize);
                drawHaloText(painter, QPointF(-length / 2.0 - 3, y - fontSize),
                             QString::number(std::abs(angle)), Qt::white, fontSize,
                             Qt::AlignRight);
            }
        }
    }
    painter.restore();

    if (m_displayRollPitch) {
        drawRollArc(painter, content, headHeight);
    }
    painter.save();
    if (m_russian) {
        painter.translate(centerX, centerY);
        painter.rotate(-m_easedRoll);
        painter.translate(-centerX, -centerY);
    }
    painter.setPen(QPen(Qt::red, qMax(2.5, unit * 0.009), Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    const double wing = unit * 0.11;
    const double gap = unit * 0.035;
    const double drop = unit * 0.05;
    const double caret = unit * 0.04;
    painter.drawLine(QPointF(centerX - gap - wing, centerY), QPointF(centerX - gap, centerY));
    painter.drawLine(QPointF(centerX + gap, centerY), QPointF(centerX + gap + wing, centerY));
    painter.drawLine(QPointF(centerX - caret, centerY), QPointF(centerX, centerY + drop));
    painter.drawLine(QPointF(centerX, centerY + drop), QPointF(centerX + caret, centerY));
    painter.restore();

    if (m_displayHeading) drawHeadingTape(painter, content, fontSize);
    if (m_displayConnection) {
        const QColor color = m_linkQuality <= 0 ? Qt::red
            : m_linkQuality < 50 ? QColor(255, 165, 0) : Qt::white;
        drawHaloText(painter, QPointF(width - 8, headHeight + 6),
                     QStringLiteral("%1%").arg(qRound(m_linkQuality)), color,
                     fontSize + 6, Qt::AlignRight);
    }
    if (m_displayXTrack) drawXTrack(painter, content, headHeight);
    if (m_displayAoa) drawAoaSsa(painter, content);

    const double tapeWidth = qMax(20.0, qMin(qMax(unit * 0.12, 42.0), width * 0.16));
    const QRectF speedRect(0, centerY - height / 4.0, tapeWidth, height / 2.0);
    const QRectF altRect(width - tapeWidth, centerY - height / 4.0,
                         tapeWidth, height / 2.0);
    if (m_displaySpeed) {
        const double speed = m_airSpeed > 0 ? m_easedAirSpeed : m_easedGroundSpeed;
        drawScrollTape(painter, speedRect, speed, m_targetSpeed, false, fontSize);
        drawHaloText(painter, QPointF(2, speedRect.bottom() + 4),
                     tr("AS %1").arg(m_airSpeed, 0, 'f', 1), Qt::white, fontSize);
        drawHaloText(painter, QPointF(2, speedRect.bottom() + fontSize + 8),
                     tr("GS %1").arg(m_groundSpeed, 0, 'f', 1), Qt::white, fontSize);
        drawHaloText(painter, QPointF(2, speedRect.bottom() + fontSize * 2 + 12),
                     tr("Thr %1%").arg(qRound(m_throttlePercent)), Qt::white, fontSize);
    }
    if (m_displayAlt) {
        drawScrollTape(painter, altRect, m_easedAlt, m_targetAlt, true, fontSize);
        drawVsi(painter, altRect);
        drawHaloText(painter, QPointF(altRect.left() - 30, altRect.bottom() + 5),
                     m_mode, Qt::white, fontSize);
        const QString distance = m_wpDist >= 1000.0
            ? QStringLiteral("%1k").arg(m_wpDist / 1000.0, 0, 'f', 1)
            : QStringLiteral("%1m").arg(qRound(m_wpDist));
        drawHaloText(painter,
                     QPointF(altRect.left() - 30, altRect.bottom() + fontSize + 12),
                     QStringLiteral("%1>%2").arg(distance).arg(m_wpNo), Qt::white, fontSize);
    }

    const double baseline = height - fontSize - 4;
    if (m_displayBattery) {
        const QColor batteryColor = m_batteryRemaining > 0 && m_batteryRemaining < 20
            ? Qt::red : m_batteryRemaining > 0 && m_batteryRemaining < 30
                ? QColor(255, 165, 0) : Qt::white;
        if (m_displayBattery2 && m_batteryVoltage2 > 0) {
            drawHaloText(painter, QPointF(2, baseline - fontSize - 4),
                         tr("Bat2 %1v %2 A %3%").arg(m_batteryVoltage2, 0, 'f', 2)
                             .arg(m_currentAmps2, 0, 'f', 1).arg(m_batteryRemaining2),
                         Qt::white, fontSize);
        } else if (m_batteryCells > 0) {
            drawHaloText(painter, QPointF(2, baseline - fontSize - 4),
                         tr("Cell %1v").arg(m_batteryVoltage / m_batteryCells, 0, 'f', 2),
                         batteryColor, fontSize);
        }
        drawHaloText(painter, QPointF(2, baseline),
                     tr("Bat1 %1v %2 A %3%").arg(m_batteryVoltage, 0, 'f', 2)
                         .arg(m_currentAmps, 0, 'f', 1).arg(m_batteryRemaining),
                     batteryColor, fontSize);
    }
    if (m_displayGps) {
        const QColor gpsColor = m_gpsFixType >= 3 ? hudGreen()
            : m_gpsFixType >= 2 ? QColor(255, 165, 0) : Qt::red;
        drawHaloText(painter, QPointF(width - 6, baseline), gpsFixText(),
                     gpsColor, fontSize, Qt::AlignRight);
    }

    m_ekfRect = m_vibeRect = m_prearmRect = QRectF();
    QFont measureFont = painter.font();
    measureFont.setPixelSize(qMax(8, qRound(fontSize)));
    measureFont.setBold(true);
    QFontMetricsF metrics(measureFont);
    const double ekfWidth = m_displayEkf ? metrics.horizontalAdvance(QStringLiteral("EKF")) : 0;
    const double vibeWidth = m_displayVibe ? metrics.horizontalAdvance(QStringLiteral("Vibe")) : 0;
    const double indicatorGap = m_displayEkf && m_displayVibe ? 16.0 : 0.0;
    double indicatorX = centerX - (ekfWidth + indicatorGap + vibeWidth) / 2.0;
    if (m_displayEkf) {
        drawHaloText(painter, QPointF(indicatorX, baseline), QStringLiteral("EKF"),
                     Qt::white, fontSize);
        m_ekfRect = QRectF(indicatorX, baseline, ekfWidth, fontSize + 4);
        indicatorX += ekfWidth + indicatorGap;
    }
    if (m_displayVibe) {
        drawHaloText(painter, QPointF(indicatorX, baseline), QStringLiteral("Vibe"),
                     Qt::white, fontSize);
        m_vibeRect = QRectF(indicatorX, baseline, vibeWidth, fontSize + 4);
    }
    if (m_displayPrearm && !m_armed) {
        const QString status = m_prearmOk ? tr("Ready to Arm") : tr("Not Ready to Arm");
        const QColor color = m_prearmOk ? QColor(50, 205, 50) : Qt::red;
        const double y = baseline - fontSize - 6;
        drawHaloText(painter, QPointF(centerX, y), status, color, fontSize, Qt::AlignHCenter);
        m_prearmRect = QRectF(centerX - metrics.horizontalAdvance(status) / 2.0, y,
                              metrics.horizontalAdvance(status), fontSize + 4);
    }

    double customY = headHeight + 4;
    for (const QString &line : m_customItemsText.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        drawHaloText(painter, QPointF(4, customY), line, Qt::white, fontSize);
        customY += fontSize + 3;
    }
    if (m_windVel > 0.0) {
        drawHaloText(painter, QPointF(centerX, headHeight + 4),
                     tr("Wind %1° %2").arg(qRound(normalizedHeading(m_windDir)))
                         .arg(m_windVel, 0, 'f', 1), Qt::white, fontSize,
                     Qt::AlignHCenter);
    }

    // Draw the MP high-message banner at its original location. Safety and
    // failsafe warnings are painted afterwards so they stay legible when a
    // very small HUD forces the two regions to overlap.
    if (!m_statusMessage.isEmpty()) {
        const QColor messageColor = m_statusMessageSeverity <= 3
            ? QColor(Qt::red)
            : m_statusMessageSeverity <= 4
                ? QColor(Qt::yellow) : QColor(Qt::white);
        const double availableWidth = qMax(40.0, width - 100.0);
        double messageFontSize = fontSize + 10.0;
        QFont messageFont = painter.font();
        messageFont.setBold(true);
        messageFont.setPixelSize(qMax(8, qRound(messageFontSize)));
        double messageWidth = QFontMetricsF(messageFont)
            .horizontalAdvance(m_statusMessage);
        if (messageWidth > availableWidth && messageWidth > 0.0) {
            messageFontSize = qMax(
                8.0, messageFontSize * availableWidth / messageWidth);
            messageFont.setPixelSize(qMax(8, qRound(messageFontSize)));
        }
        QString displayed = m_statusMessage;
        const QFontMetricsF messageMetrics(messageFont);
        if (messageMetrics.horizontalAdvance(displayed) > availableWidth) {
            displayed = messageMetrics.elidedText(
                displayed, Qt::ElideRight, qRound(availableWidth));
        }
        drawHaloText(painter, QPointF(centerX, height * 2.0 / 3.0),
                     displayed, messageColor, messageFontSize,
                     Qt::AlignHCenter);
    }

    drawHaloText(painter, QPointF(centerX, height / 3.0),
                 m_armed ? tr("ARMED") : tr("DISARMED"), Qt::red,
                 fontSize + 10, Qt::AlignHCenter);
    const bool blink = (QDateTime::currentMSecsSinceEpoch() % 1000) < 500;
    double warningY = height / 3.0 + fontSize + 14;
    if (m_safetyActive) {
        drawHaloText(painter, QPointF(centerX, warningY), tr("SAFE"), Qt::red,
                     fontSize + 10, Qt::AlignHCenter);
        warningY += fontSize + 16;
    }
    if (m_failsafe && blink) {
        drawHaloText(painter, QPointF(centerX, warningY), tr("FAILSAFE"), Qt::red,
                     fontSize + 16, Qt::AlignHCenter);
        warningY += fontSize + 22;
    }
    if (m_batteryRemaining > 0 && m_batteryRemaining < 20 && blink) {
        drawHaloText(painter, QPointF(centerX, warningY), tr("LOW VOLTAGE"), Qt::red,
                     fontSize + 8, Qt::AlignHCenter);
    }
    painter.restore();
}

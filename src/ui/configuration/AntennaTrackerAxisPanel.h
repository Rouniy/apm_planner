#ifndef ANTENNATRACKERAXISPANEL_H
#define ANTENNATRACKERAXISPANEL_H

#include "AntennaTrackerUIViewModel.h"

#include <QFrame>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSlider;

/*
 * One "Pan" / "Tilt" box of the Mission Planner 10 antenna tracker pages:
 * Range / Angle, PWM Range, Center PWM, Speed, Acceleration inputs, the
 * "Trim: N" slider (tick every 5) and the reverse checkbox. The Serial page
 * adds a centred "0" under the slider and labels the checkbox "Rev"; the Live
 * page omits the "0" and says "Reverse". The panel is a dumb widget: the page
 * wires it to AntennaTrackerUIViewModel.
 */
class AntennaTrackerAxisPanel final : public QFrame
{
    Q_OBJECT

public:
    using Axis = AntennaTrackerUIViewModel::Axis;
    using Field = AntennaTrackerUIViewModel::Field;

    struct Options
    {
        QString title;          // "Pan" / "Tilt"
        QString reverseText;    // "Rev" / "Reverse"
        QString objectPrefix;   // e.g. "trkSerialPan"
        bool showCenterLabel = false;
        double trimMin = -180.0;
        double trimMax = 180.0;
    };

    AntennaTrackerAxisPanel(Axis axis, const Options &options, QWidget *parent = nullptr);

    Axis axis() const { return m_axis; }
    QLineEdit *fieldEdit(Field field) const { return m_fields[int(field)]; }
    QSlider *trimSlider() const { return m_trimSlider; }
    QLabel *trimLabel() const { return m_trimLabel; }
    QLabel *centerLabel() const { return m_centerLabel; }
    QCheckBox *reverseCheck() const { return m_reverse; }

    QString fieldText(Field field) const;
    double trim() const;
    bool reverse() const;

public slots:
    void setFieldText(Field field, const QString &text);
    void setTrim(double value);
    void setTrimRange(double minimum, double maximum);
    void setReverse(bool value);
    // MP10 ControlsEnabled: the five inputs except Speed/Acceleration.
    void setControlsEnabled(bool enabled);
    // MP10 SpeedAccelEnabled: Speed and Acceleration only.
    void setSpeedAccelEnabled(bool enabled);

signals:
    void fieldEdited(AntennaTrackerUIViewModel::Axis axis,
                     AntennaTrackerUIViewModel::Field field, const QString &text);
    void trimEdited(AntennaTrackerUIViewModel::Axis axis, double value);
    void reverseToggled(AntennaTrackerUIViewModel::Axis axis, bool value);

private:
    void updateTrimLabel();

    Axis m_axis;
    QLineEdit *m_fields[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    QLabel *m_trimLabel = nullptr;
    QSlider *m_trimSlider = nullptr;
    QLabel *m_centerLabel = nullptr;
    QCheckBox *m_reverse = nullptr;
};

#endif // ANTENNATRACKERAXISPANEL_H

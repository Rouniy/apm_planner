#ifndef GUIDEDALTITUDEDIALOG_H
#define GUIDEDALTITUDEDIALOG_H

#include <QDialog>
#include <QString>

#include <mavlink.h>

class QComboBox;
class QLabel;
class QLineEdit;

class GuidedAltitudeDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit GuidedAltitudeDialog(
        double initialAltitudeMetres,
        MAV_FRAME initialFrame,
        double displayMultiplier,
        const QString &displayUnit,
        const QString &frozenTargetDescription,
        QWidget *parent = nullptr);

    double altitudeMetres() const;
    MAV_FRAME frame() const;
    bool hasAcceptedValue() const;

    static bool isSupportedFrame(MAV_FRAME frame);

public slots:
    void accept() override;
    void reject() override;

private:
    QLineEdit *m_altitude = nullptr;
    QComboBox *m_frame = nullptr;
    QLabel *m_validation = nullptr;
    double m_displayMultiplier = 1.0;
    double m_acceptedAltitudeMetres = 0.0;
    MAV_FRAME m_acceptedFrame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    bool m_hasAcceptedValue = false;
};

#endif // GUIDEDALTITUDEDIALOG_H

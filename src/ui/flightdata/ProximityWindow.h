#ifndef PROXIMITYWINDOW_H
#define PROXIMITYWINDOW_H

#include <QPointer>
#include <QWidget>

class ProximityRadarControl;
class Proximity;
class UASInterface;

class ProximityWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit ProximityWindow(QWidget *parent = nullptr);
    ~ProximityWindow() override;

    static ProximityWindow *OpenWindow(QWidget *owner = nullptr,
                                       UASInterface *uas = nullptr);

    ProximityRadarControl *Radar() const;
    Proximity *ProximityState() const;
    UASInterface *activeUAS() const;

public slots:
    void setActiveUAS(UASInterface *uas);

private:
    QPointer<UASInterface> m_uas;
    ProximityRadarControl *m_radar = nullptr;
};

#endif

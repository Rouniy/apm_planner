#ifndef HELICOLLECTIVEPLOT_H
#define HELICOLLECTIVEPLOT_H

#include "HeliVisualization.h"

#include <QVector>
#include <QWidget>

/**
 * Native, presentation-only collective-curve plot used by Heli Setup.
 *
 * The widget owns no vehicle or parameter state. Callers provide the already
 * calculated Stabilize and Acro curves plus the live collective cursor.
 */
class HeliCollectivePlot final : public QWidget
{
public:
    explicit HeliCollectivePlot(QWidget *parent = nullptr);

    QVector<HeliVisualization::CurvePoint> stabilizeCurve() const
    {
        return m_stabilizeCurve;
    }
    QVector<HeliVisualization::CurvePoint> acroCurve() const
    {
        return m_acroCurve;
    }
    double cursorPercent() const { return m_cursorPercent; }

    void setStabilizeCurve(
        const QVector<HeliVisualization::CurvePoint> &curve);
    void setAcroCurve(const QVector<HeliVisualization::CurvePoint> &curve);
    void setCursorPercent(double percent);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<HeliVisualization::CurvePoint> m_stabilizeCurve;
    QVector<HeliVisualization::CurvePoint> m_acroCurve;
    double m_cursorPercent = 0.0;
};

#endif // HELICOLLECTIVEPLOT_H

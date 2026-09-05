#ifndef CONFIGPX4FLOWVIEW_H
#define CONFIGPX4FLOWVIEW_H

#include <QImage>
#include <QVariantList>
#include <QWidget>

class QLabel;
class QComboBox;
class QPushButton;
class QShowEvent;
class QHideEvent;

/** Transport-free Mission Planner 10 PX4Flow image and focus surface. */
class ConfigPX4FlowView final : public QWidget
{
    Q_OBJECT

public:
    static constexpr int MaximumSources = 64;

    explicit ConfigPX4FlowView(QWidget *parent = nullptr);

    QString selectedSourceId() const;
    int sourceCount() const;
    QImage frame() const;
    QSize displayedFrameSize() const;
    bool videoOnly() const { return m_videoOnly; }
    bool active() const { return m_active; }

public slots:
    void setSources(const QVariantList &sources);
    void setSelectedSourceId(const QString &sourceId);
    void setStatus(const QString &status);
    void setFrame(const QImage &frame);
    void setModeState(bool videoOnly, bool canToggle, bool busy);

signals:
    void sourceSelected(const QString &sourceId);
    void focusRequested();
    void activated();
    void deactivated();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void applyRequestedSource();
    void syncModeButton();

    QComboBox *m_sources = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_image = nullptr;
    QPushButton *m_focus = nullptr;
    QString m_requestedSourceId;
    bool m_videoOnly = false;
    bool m_canToggle = false;
    bool m_busy = false;
    bool m_active = false;
};

#endif // CONFIGPX4FLOWVIEW_H

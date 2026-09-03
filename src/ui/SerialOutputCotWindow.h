#ifndef SERIALOUTPUTCOTWINDOW_H
#define SERIALOUTPUTCOTWINDOW_H

#include "SerialOutputCotViewModel.h"
#include "comm/CotOutputService.h"

#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableView;

/** Mission Planner 10 TOOLS > Cursor-on-Target / TAK Output window. */
class SerialOutputCotWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        CotOutputService::TransportFactory transportFactory;
        CotOutputService::Sender sender;
        CotOutputService::Clock clock;
        SerialOutputCotViewModel::Dependencies viewModel;
    };

    static constexpr int WindowWidth = 720;
    static constexpr int WindowHeight = 820;
    static constexpr int MinimumWindowWidth = 560;
    static constexpr int MinimumWindowHeight = 650;

    explicit SerialOutputCotWindow(QWidget *owner = nullptr);
    SerialOutputCotWindow(Dependencies dependencies,
                          QWidget *owner = nullptr);
    ~SerialOutputCotWindow() override;

    static SerialOutputCotWindow *OpenWindow(QWidget *owner = nullptr);

    CotOutputService *service() const { return m_service; }
    SerialOutputCotViewModel *viewModel() const { return m_viewModel; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectApplicationSignals();
    void syncUi();
    void commitGridEdit();

    CotOutputService *m_service = nullptr;
    SerialOutputCotViewModel *m_viewModel = nullptr;
    QComboBox *m_endpoint = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QDoubleSpinBox *m_update = nullptr;
    QLineEdit *m_eventType = nullptr;
    QLineEdit *m_uidPrefix = nullptr;
    QLineEdit *m_callsign = nullptr;
    QCheckBox *m_advanced = nullptr;
    QTableView *m_identities = nullptr;
    QPushButton *m_refreshSystems = nullptr;
    QPushButton *m_addIdentity = nullptr;
    QPushButton *m_removeIdentity = nullptr;
    QPushButton *m_toggle = nullptr;
    QCheckBox *m_indent = nullptr;
    QLabel *m_status = nullptr;
    QPlainTextEdit *m_lastEvent = nullptr;
};

#endif // SERIALOUTPUTCOTWINDOW_H

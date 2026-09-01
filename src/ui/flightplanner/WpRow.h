#ifndef WPROW_H
#define WPROW_H

#include <QObject>
#include <QStringList>
#include <QVariant>

struct WpRowData
{
    int Seq = 0;
    quint16 Command = 0;
    double P1 = 0.0;
    double P2 = 0.0;
    double P3 = 0.0;
    double P4 = 0.0;
    double Lat = 0.0;
    double Lng = 0.0;
    double Alt = 0.0;
    QString Grad;
    QString Angle;
    QString Dist;
    QString Az;
    quint8 Frame = 3;
    QVariant Tag;
    QString Zone;
    QString Easting;
    QString Northing;
    QString Mgrs;
};

Q_DECLARE_METATYPE(WpRowData)

class WpRow final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int Seq READ Seq WRITE setSeq NOTIFY seqChanged)
    Q_PROPERTY(int DisplayNumber READ DisplayNumber NOTIFY displayNumberChanged)
    Q_PROPERTY(quint16 Command READ Command WRITE setCommand NOTIFY commandChanged)
    Q_PROPERTY(QString CommandName READ CommandName WRITE setCommandName NOTIFY commandNameChanged)
    Q_PROPERTY(double P1 READ P1 WRITE setP1 NOTIFY p1Changed)
    Q_PROPERTY(double P2 READ P2 WRITE setP2 NOTIFY p2Changed)
    Q_PROPERTY(double P3 READ P3 WRITE setP3 NOTIFY p3Changed)
    Q_PROPERTY(double P4 READ P4 WRITE setP4 NOTIFY p4Changed)
    Q_PROPERTY(double Lat READ Lat WRITE setLat NOTIFY latChanged)
    Q_PROPERTY(double Lng READ Lng WRITE setLng NOTIFY lngChanged)
    Q_PROPERTY(double Alt READ Alt WRITE setAlt NOTIFY altChanged)
    Q_PROPERTY(double AltDisplay READ AltDisplay WRITE setAltDisplay NOTIFY altDisplayChanged)
    Q_PROPERTY(QString Grad READ Grad WRITE setGrad NOTIFY gradChanged)
    Q_PROPERTY(QString Angle READ Angle WRITE setAngle NOTIFY angleChanged)
    Q_PROPERTY(QString Dist READ Dist WRITE setDist NOTIFY distChanged)
    Q_PROPERTY(QString Az READ Az WRITE setAz NOTIFY azChanged)
    Q_PROPERTY(quint8 Frame READ Frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(QString FrameName READ FrameName WRITE setFrameName NOTIFY frameNameChanged)
    Q_PROPERTY(QVariant Tag READ Tag WRITE setTag NOTIFY tagChanged)
    Q_PROPERTY(QString Zone READ Zone WRITE setZone NOTIFY zoneChanged)
    Q_PROPERTY(QString Easting READ Easting WRITE setEasting NOTIFY eastingChanged)
    Q_PROPERTY(QString Northing READ Northing WRITE setNorthing NOTIFY northingChanged)
    Q_PROPERTY(QString Mgrs READ Mgrs WRITE setMgrs NOTIFY mgrsChanged)

public:
    explicit WpRow(QObject *parent = nullptr);
    explicit WpRow(const WpRowData &data, QObject *parent = nullptr);

    int Seq() const;
    int DisplayNumber() const;
    quint16 Command() const;
    QString CommandName() const;
    double P1() const;
    double P2() const;
    double P3() const;
    double P4() const;
    double Lat() const;
    double Lng() const;
    double Alt() const;
    double AltDisplay() const;
    QString Grad() const;
    QString Angle() const;
    QString Dist() const;
    QString Az() const;
    quint8 Frame() const;
    QString FrameName() const;
    QVariant Tag() const;
    QString Zone() const;
    QString Easting() const;
    QString Northing() const;
    QString Mgrs() const;

    WpRowData toData() const;

    static QStringList CommandList();
    static QString CommandNameFor(quint16 command);
    static bool commandForName(const QString &name, quint16 *command);
    static QStringList FrameList();

public slots:
    void setSeq(int value);
    void setCommand(quint16 value);
    void setCommandName(const QString &value);
    void setP1(double value);
    void setP2(double value);
    void setP3(double value);
    void setP4(double value);
    void setLat(double value);
    void setLng(double value);
    void setAlt(double value);
    void setAltDisplay(double value);
    void setGrad(const QString &value);
    void setAngle(const QString &value);
    void setDist(const QString &value);
    void setAz(const QString &value);
    void setFrame(quint8 value);
    void setFrameName(const QString &value);
    void setTag(const QVariant &value);
    void setZone(const QString &value);
    void setEasting(const QString &value);
    void setNorthing(const QString &value);
    void setMgrs(const QString &value);

signals:
    void seqChanged(int value);
    void displayNumberChanged(int value);
    void commandChanged(quint16 value);
    void commandNameChanged(const QString &value);
    void p1Changed(double value);
    void p2Changed(double value);
    void p3Changed(double value);
    void p4Changed(double value);
    void latChanged(double value);
    void lngChanged(double value);
    void altChanged(double value);
    void altDisplayChanged(double value);
    void gradChanged(const QString &value);
    void angleChanged(const QString &value);
    void distChanged(const QString &value);
    void azChanged(const QString &value);
    void frameChanged(quint8 value);
    void frameNameChanged(const QString &value);
    void tagChanged(const QVariant &value);
    void zoneChanged(const QString &value);
    void eastingChanged(const QString &value);
    void northingChanged(const QString &value);
    void mgrsChanged(const QString &value);
    void changed();

private:
    void applyData(const WpRowData &data);
    void recomputeCoordinates();
    void reverseFromUtm();

    WpRowData m_data;
    bool m_coordinateGuard = false;
};

#endif

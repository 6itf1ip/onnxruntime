#ifndef RADARVIDEOITEM_H
#define RADARVIDEOITEM_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QMutex>
#include <QPointF>
#include <QColor>
#include <QMap>
#include <QVector>
#include <QByteArray>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>

class RadarVideoItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(double latitude READ latitude WRITE setLatitude NOTIFY latitudeChanged)
    Q_PROPERTY(double longitude READ longitude WRITE setLongitude NOTIFY longitudeChanged)
    Q_PROPERTY(QPointF centerOffset READ centerOffset WRITE setCenterOffset NOTIFY centerOffsetChanged)

public:
    explicit RadarVideoItem();

    // Property getters
    double zoomLevel() const { return m_zoomLevel; }
    double latitude() const { return m_latitude; }
    double longitude() const { return m_longitude; }
    QPointF centerOffset() const { return m_centerOffset; }

    // Property setters
    void setZoomLevel(double zoom);
    void setLatitude(double lat);
    void setLongitude(double lon);
    void setCenterOffset(const QPointF &offset);

    // Painting
    void paint(QPainter *painter) override;

    // Invokable methods
    Q_INVOKABLE void updateAngle(int angleIndex,
                                  const QByteArray &video0,
                                  const QByteArray &video1,
                                  const QByteArray &video2,
                                  const QByteArray &video3,
                                  int angleThreshold);
    Q_INVOKABLE void drawStrobeLine(int angleIndex);
    Q_INVOKABLE void addPlot(int angleIndex, int rangeIndex,
                              unsigned char m_ucPlotType,
                              unsigned short m_usMode1,
                              unsigned short m_usMode2,
                              unsigned short m_usMode3,
                              float height);
    Q_INVOKABLE void clearAllPlots();
    Q_INVOKABLE void enablePlot(bool value);
    Q_INVOKABLE void enablePlotTime(bool value);
    Q_INVOKABLE void showPlotTextStatus(int status);
    Q_INVOKABLE void showPlotTextValues(int mode1, int mode2, int mode3, int height);
    Q_INVOKABLE void setPlotSize(float size);
    Q_INVOKABLE void setPlotIntensity(float intensity);
    Q_INVOKABLE void setVideoVisibility(int index, bool visible);
    Q_INVOKABLE void updateVideoColor(QString videoKey, float R, float G, float B);
    Q_INVOKABLE void updateVideoIntensity(float intensity, const QString &number);
    Q_INVOKABLE void loadGeoLinesFromFile(const QString &filePath);
    Q_INVOKABLE void clearVideoLayer(int layerIndex);

signals:
    void zoomLevelChanged();
    void latitudeChanged();
    void longitudeChanged();
    void centerOffsetChanged();
    void plotDifferenceAndAverageTime(int plotsCount, double averageTime, 
                                      double minAngle, double maxAngle, 
                                      int minTime, int maxTime);
    void iFFPlotDifferenceAndAverageTime(int iFFPlotsCount, double iFFAverageTime,
                                         double iffminAngle, double iffmaxAngle,
                                         int iffminTime, int iffmaxTime);

private:
    // Constants
    static constexpr int ANGLE_COUNT = 4032;
    static constexpr int RANGE_SIZE = 1024;
    static constexpr float TOTAL_SWEEP_ANGLE = 360.0f;
    static constexpr double MAX_RANGE_KM = 100.0;

    // Structures
    struct VideoData {
        int angleIndex;
        QByteArray video0;
        QByteArray video1;
        QByteArray video2;
        QByteArray video3;
    };

    struct ScreenLine {
        QPointF start;
        QPointF end;
        QColor color;
    };

    struct PlotPoint {
        int angle;
        int range;
        QColor color;
        unsigned char plotType;
        unsigned short mode1;
        unsigned short mode2;
        unsigned short mode3;
        float height;
    };

    // Helper methods
    void regenerateRadarImage();
    void drawAngleToImage(const VideoData &vdata, const QRgb *rgbColors,
                          const QPointF &viewCenter, double mpp, int w, int h);
    void clearAngleFromImage(int angleIndex);
    double metersPerPixel(double latitude, double zoomLevel);
    QPointF geoToPixel(double lat, double lon) const;
    QPointF geoToPixelLine(double lat, double lon) const;

    // Member variables - Position and view
    double m_latitude = 0.0;
    double m_longitude = 0.0;
    double m_zoomLevel = 10.0;
    QPointF m_centerOffset;

    // Member variables - State tracking
    double lastZoomLevel = 10.0;
    QPointF lastCenterOffset;
    bool lastVideoVisible[4] = {true, true, true, true};
    bool needsImageRegeneration = false;

    // Member variables - Radar data
    QImage radarImage;
    QMap<int, VideoData> videoDataMap;
    QVector<ScreenLine> m_lines;
    QVector<PlotPoint> plots;
    QVector<int> strobeAngles;

    // Member variables - Video settings
    bool videoVisible[4] = {true, true, true, true};
    bool isAllVideoVisible = true;
    QMap<QString, QColor> videoColorMap = {
        {"0", QColor(255, 0, 0)},
        {"1", QColor(0, 255, 0)},
        {"2", QColor(0, 0, 255)},
        {"3", QColor(255, 255, 0)}
    };
    QMap<QString, QColor> intensityVideoColorMap;
    QMap<QString, float> videoIntensityMap = {
        {"0", 1.0f},
        {"1", 1.0f},
        {"2", 1.0f},
        {"3", 1.0f}
    };

    // Member variables - Plot settings
    bool isPlotsEnabled = true;
    bool isPlotTimeEnabled = false;
    int isPlotTextEnabled = 0;
    float plotSize = 10.0f;
    float plotIntesity = 1.0f;
    bool modeHeightVisible[4] = {true, true, true, true};

    // Member variables - Plot timing
    int currentAngle = 0;
    int lastAngleIndex = -1;
    int scanValue = 0;
    int plotsCount = 0;
    int sumPlots = 0;
    double averageTime = 0.0;
    QVector<int> primaryPlotTime;
    QVector<double> primaryPlotAngle;
    int iFFPlotsCount = 0;
    int iFFSumPlots = 0;
    double iFFAverageTime = 0.0;
    QVector<int> iFFPlotTime;
    QVector<double> iFFPlotAngle;

    // Thread safety
    QMutex mutex;
};

#endif // RADARVIDEOITEM_H

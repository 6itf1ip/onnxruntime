#include "RadarVideoItem.h"
#include <QPainter>
#include <QtMath>
#include <QDebug>
#include <QPainterPath>
#include <QGeoCoordinate>

RadarVideoItem::RadarVideoItem() {
    setFlag(ItemHasContents, true);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    for (auto it = videoColorMap.begin(); it != videoColorMap.end(); ++it) {
        intensityVideoColorMap.insert(it.key(), QColor(it.value()));
    }
}

QPointF RadarVideoItem::geoToPixelLine(double lat, double lon) const {
    const double earthRadius = 6371000.0;

    const double centerLat = m_latitude;
    const double centerLon = m_longitude;

    double dLat = qDegreesToRadians(lat - centerLat);
    double dLon = qDegreesToRadians(lon - centerLon);
    double meanLat = qDegreesToRadians((lat + centerLat) / 2.0);
    double meanLon = qDegreesToRadians((lon + centerLon) / 2.0);
    double dx = earthRadius * dLon * qCos(meanLat);
    double dy = earthRadius * dLat * qSin(meanLon);

    double mpp = metersPerPixel(m_latitude, m_zoomLevel);
    double metersToPixels = 1.0 / mpp;

    QPointF pixelOffset(dx * metersToPixels, -dy * metersToPixels);
    QPointF viewCenter(width() / 2.0 + m_centerOffset.x(), height() / 2.0 + m_centerOffset.y());

    return viewCenter + pixelOffset;
}

void RadarVideoItem::loadGeoLinesFromFile(const QString &filePath) {
    QMutexLocker locker(&mutex);
    m_lines.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Could not open file:" << filePath;
        return;
    }

    QTextStream in(&file);
    QVector<QGeoCoordinate> coords;
    QVector<QColor> colors;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList parts = line.split(QRegularExpression("\\s+"));
        if (parts.size() < 5) continue;

        bool ok1, ok2, ok3, ok4, ok5;
        double lat = parts[0].toDouble(&ok1);
        double lon = parts[1].toDouble(&ok2);
        double r = parts[2].toDouble(&ok3);
        double g = parts[3].toDouble(&ok4);
        double b = parts[4].toDouble(&ok5);

        if (!(ok1 && ok2 && ok3 && ok4 && ok5)) continue;

        coords.append(QGeoCoordinate(lat, lon));
        colors.append(QColor::fromRgbF(r, g, b));
    }

    file.close();

    for (int i = 0; i + 1 < coords.size(); i += 2) {
        ScreenLine line;
        line.color = colors[i];
        m_lines.append(line);

        // Store coordinates, will be converted to pixels during paint
        m_lines.last().start = QPointF(coords[i].latitude(), coords[i].longitude());
        m_lines.last().end = QPointF(coords[i + 1].latitude(), coords[i + 1].longitude());
    }

    update();
}

double RadarVideoItem::metersPerPixel(double latitude, double zoomLevel) {
    constexpr double earthCircumference = 40075017.0;
    return (earthCircumference * qCos(qDegreesToRadians(latitude))) / (256.0 * qPow(2.0, zoomLevel));
}

void RadarVideoItem::setZoomLevel(double zoom) {
    if (!qFuzzyCompare(m_zoomLevel, zoom)) {
        m_zoomLevel = zoom;
        needsImageRegeneration = true;
        update();
        emit zoomLevelChanged();
    }
}

void RadarVideoItem::setCenterOffset(const QPointF &offset) {
    if (m_centerOffset != offset) {
        m_centerOffset = offset;
        needsImageRegeneration = true;
        update();
        emit centerOffsetChanged();
    }
}

void RadarVideoItem::setLongitude(double lon) {
    if (!qFuzzyCompare(m_longitude, lon)) {
        m_longitude = lon;
        emit longitudeChanged();
        update();
    }
}

void RadarVideoItem::setLatitude(double lat) {
    if (!qFuzzyCompare(m_latitude, lat)) {
        m_latitude = lat;
        update();
        emit latitudeChanged();
    }
}

void RadarVideoItem::drawStrobeLine(int angleIndex) {
    if (!strobeAngles.contains(angleIndex)) {
        strobeAngles.append(angleIndex);
    }
    update();
}

void RadarVideoItem::drawAngleToImage(const VideoData &vdata, const QRgb *rgbColors,
                                      const QPointF &viewCenter, double mpp, int w, int h) {
    const float angleStep = TOTAL_SWEEP_ANGLE / static_cast<float>(ANGLE_COUNT);
    const float angleDeg = vdata.angleIndex * angleStep - 90.0f;
    const float angleRad = qDegreesToRadians(angleDeg);
    const float cosAngle = qCos(angleRad);
    const float sinAngle = qSin(angleRad);

    auto drawLayerFast = [&](const QByteArray &data, QRgb color, bool isVisible) {
        if (!isVisible) return;

        const unsigned char* dataPtr = reinterpret_cast<const unsigned char*>(data.constData());

        for (int r = 0; r < RANGE_SIZE; ++r) {
            if (dataPtr[r] == 0) continue;

            double rangeMeters = (static_cast<double>(r) / RANGE_SIZE) * (MAX_RANGE_KM * 1000.0);
            double pixelRadius = rangeMeters / mpp;

            int x = static_cast<int>(viewCenter.x() + pixelRadius * cosAngle);
            int y = static_cast<int>(viewCenter.y() + pixelRadius * sinAngle);

            if (x >= 0 && x < w && y >= 0 && y < h) {
                radarImage.setPixel(x, y, color);
            }
        }
    };

    drawLayerFast(vdata.video0, rgbColors[0], videoVisible[0]);
    drawLayerFast(vdata.video1, rgbColors[1], videoVisible[1]);
    drawLayerFast(vdata.video2, rgbColors[2], videoVisible[2]);
    drawLayerFast(vdata.video3, rgbColors[3], videoVisible[3]);
}

void RadarVideoItem::regenerateRadarImage() {
    int w = qMax(100, static_cast<int>(width()));
    int h = qMax(100, static_cast<int>(height()));

    // Check if size changed
    bool sizeChanged = radarImage.size() != QSize(w, h);

    // Check if zoom/offset changed
    bool zoomChanged = !qFuzzyCompare(m_zoomLevel, lastZoomLevel);
    bool offsetChanged = m_centerOffset != lastCenterOffset;

    // Check if any individual video layer visibility has changed
    bool visibilityChanged = false;
    for (int i = 0; i < 4; ++i) {
        if (videoVisible[i] != lastVideoVisible[i]) {
            visibilityChanged = true;
            lastVideoVisible[i] = videoVisible[i];
        }
    }

    // Only recreate image if size changed
    if (sizeChanged) {
        radarImage = QImage(w, h, QImage::Format_ARGB32);
        radarImage.fill(Qt::transparent);
        needsImageRegeneration = true;
    }

    // Check if any video layer is currently visible
    bool anyVideoVisible = isAllVideoVisible && (videoVisible[0] || videoVisible[1] || videoVisible[2] || videoVisible[3]);

    // If zoom, offset, or visibility changed, redraw existing angles incrementally
    if ((zoomChanged || offsetChanged || visibilityChanged) && !radarImage.isNull() && anyVideoVisible && !videoDataMap.isEmpty()) {
        // Clear the image first
        radarImage.fill(Qt::transparent);

        double mpp = metersPerPixel(m_latitude, m_zoomLevel);
        QPointF viewCenter(w / 2.0 + m_centerOffset.x(), h / 2.0 + m_centerOffset.y());

        QRgb rgbColors[4];
        for (int i = 0; i < 4; ++i) {
            QColor c = intensityVideoColorMap[QString::number(i)];
            rgbColors[i] = qRgb(c.red(), c.green(), c.blue());
        }

        // Redraw each angle incrementally
        for (auto it = videoDataMap.constBegin(); it != videoDataMap.constEnd(); ++it) {
            const VideoData &vdata = it.value();
            drawAngleToImage(vdata, rgbColors, viewCenter, mpp, w, h);
        }
    }

    lastZoomLevel = m_zoomLevel;
    lastCenterOffset = m_centerOffset;
    needsImageRegeneration = false;
}

void RadarVideoItem::updateAngle(int angleIndex,
                                 const QByteArray &video0,
                                 const QByteArray &video1,
                                 const QByteArray &video2,
                                 const QByteArray &video3,
                                 int angleThreshold) {

    currentAngle = angleIndex;
    if (isPlotTimeEnabled) {
        scanValue = scanValue + 1;
    }

    if (angleIndex < 0 || angleIndex >= ANGLE_COUNT ||
        video0.size() != RANGE_SIZE || video1.size() != RANGE_SIZE ||
        video2.size() != RANGE_SIZE || video3.size() != RANGE_SIZE) {
        return;
    }

    QMutexLocker locker(&mutex);

    auto normAngle = [&](int a) -> int {
        if (ANGLE_COUNT <= 0) return 0;
        a %= ANGLE_COUNT;
        if (a < 0) a += ANGLE_COUNT;
        return a;
    };

    auto angleInRange = [&](int start, int end, int x) -> bool {
        start = normAngle(start);
        end   = normAngle(end);
        x     = normAngle(x);
        if (start <= end) return (x >= start && x <= end);
        return (x >= start || x <= end);
    };

    const int clearCount = qMax(1, qMin(angleThreshold, ANGLE_COUNT));
    const int clearStart = normAngle(angleIndex);
    const int clearEnd = normAngle(angleIndex + clearCount - 1);

    // Check if any video layer is visible
    bool anyVideoVisible = videoVisible[0] || videoVisible[1] || videoVisible[2] || videoVisible[3];

    // Clear old angles from image and map
    if (isAllVideoVisible && anyVideoVisible) {
        for (int i = clearStart; i != (clearEnd + 1) % ANGLE_COUNT; i = (i + 1) % ANGLE_COUNT) {
            if (videoDataMap.contains(i)) {
                clearAngleFromImage(i);
                videoDataMap.remove(i);
            }
        }
    } else {
        // Just remove from map, don't clear from image
        for (int i = clearStart; i != (clearEnd + 1) % ANGLE_COUNT; i = (i + 1) % ANGLE_COUNT) {
            videoDataMap.remove(i);
        }
    }

    if (lastAngleIndex >= 0) {
        int diff = lastAngleIndex - angleIndex;
        if (diff > (ANGLE_COUNT / 2)) {
            int wrapStart = normAngle(lastAngleIndex + 1);
            int wrapEnd = normAngle(ANGLE_COUNT);

            for (int i = wrapStart; i != (wrapEnd + 1) % ANGLE_COUNT; i = (i + 1) % ANGLE_COUNT) {
                if (isAllVideoVisible && anyVideoVisible && videoDataMap.contains(i)) {
                    clearAngleFromImage(i);
                }
                videoDataMap.remove(i);
            }

            for (int i = strobeAngles.size() - 1; i >= 0; --i) {
                if (angleInRange(wrapStart, wrapEnd, strobeAngles[i])) strobeAngles.removeAt(i);
            }
            for (int i = plots.size() - 1; i >= 0; --i) {
                if (angleInRange(wrapStart, wrapEnd, plots[i].angle)) plots.removeAt(i);
            }
        }
    }

    const int prevStrobeWindow = 5;
    const int prevPlotWindow   = 10;

    int strobeRangeStart = normAngle(angleIndex - prevStrobeWindow);
    int strobeRangeEnd   = normAngle(angleIndex - 1);
    for (int i = strobeAngles.size() - 1; i >= 0; --i) {
        if (angleInRange(strobeRangeStart, strobeRangeEnd, strobeAngles[i])) strobeAngles.removeAt(i);
    }

    int plotRangeStart = normAngle(angleIndex - prevPlotWindow);
    int plotRangeEnd   = normAngle(angleIndex - 1);
    for (int i = plots.size() - 1; i >= 0; --i) {
        if (angleInRange(plotRangeStart, plotRangeEnd, plots[i].angle)) plots.removeAt(i);
    }

    // Store new video data
    VideoData vdata;
    vdata.angleIndex = angleIndex;
    vdata.video0 = video0;
    vdata.video1 = video1;
    vdata.video2 = video2;
    vdata.video3 = video3;
    videoDataMap[angleIndex] = vdata;

    // Draw new angle to image incrementally
    if (isAllVideoVisible && anyVideoVisible && !radarImage.isNull()) {
        int w = radarImage.width();
        int h = radarImage.height();
        double mpp = metersPerPixel(m_latitude, m_zoomLevel);
        QPointF viewCenter(w / 2.0 + m_centerOffset.x(), h / 2.0 + m_centerOffset.y());

        QRgb rgbColors[4];
        for (int i = 0; i < 4; ++i) {
            QColor c = intensityVideoColorMap[QString::number(i)];
            rgbColors[i] = qRgb(c.red(), c.green(), c.blue());
        }

        drawAngleToImage(vdata, rgbColors, viewCenter, mpp, w, h);
    }

    lastAngleIndex = angleIndex;

    update();
}

void RadarVideoItem::clearAngleFromImage(int angleIndex) {
    if (radarImage.isNull()) return;

    int w = radarImage.width();
    int h = radarImage.height();
    double mpp = metersPerPixel(m_latitude, m_zoomLevel);
    QPointF viewCenter(w / 2.0 + m_centerOffset.x(), h / 2.0 + m_centerOffset.y());

    const float angleStep = TOTAL_SWEEP_ANGLE / static_cast<float>(ANGLE_COUNT);
    const float angleDeg = angleIndex * angleStep - 90.0f;
    const float angleRad = qDegreesToRadians(angleDeg);
    const float cosAngle = qCos(angleRad);
    const float sinAngle = qSin(angleRad);

    // Clear pixels at this angle for all ranges
    for (int r = 0; r < RANGE_SIZE; ++r) {
        double rangeMeters = (static_cast<double>(r) / RANGE_SIZE) * (MAX_RANGE_KM * 1000.0);
        double pixelRadius = rangeMeters / mpp;

        int x = static_cast<int>(viewCenter.x() + pixelRadius * cosAngle);
        int y = static_cast<int>(viewCenter.y() + pixelRadius * sinAngle);

        if (x >= 0 && x < w && y >= 0 && y < h) {
            radarImage.setPixel(x, y, qRgba(0, 0, 0, 0)); // Transparent
        }
    }
}

void RadarVideoItem::updateVideoColor(QString videoKey, float R, float G, float B) {
    if (videoColorMap.contains(videoKey)) {
        videoColorMap[videoKey] = QColor(R, G, B);
        intensityVideoColorMap[videoKey] = QColor(R * videoIntensityMap[videoKey], G * videoIntensityMap[videoKey], B * videoIntensityMap[videoKey]);
        needsImageRegeneration = true;
    } else {
        qDebug() << "Invalid video key:" << videoKey;
    }
}

void RadarVideoItem::updateVideoIntensity(float intensity, const QString &number){
    videoIntensityMap[number] = intensity;

    if (intensityVideoColorMap.contains(number)) {
        QColor baseColor = videoColorMap[number];
        float r = baseColor.red();
        float g = baseColor.green();
        float b = baseColor.blue();

        intensityVideoColorMap[number] = QColor(r*intensity, g*intensity, b*intensity);
        needsImageRegeneration = true;
    }
}

void RadarVideoItem::setVideoVisibility(int index, bool visible) {
    if (index >= 0 && index < 4) {
        QMutexLocker locker(&mutex);
        if (videoVisible[index] != visible) {
            videoVisible[index] = visible;
            
            // Mark that we need to regenerate the image on next zoom/offset change
            needsImageRegeneration = true;
        }
        update();
    }
    else if (index == 6){
        QMutexLocker locker(&mutex);
        isAllVideoVisible = visible;

        // Don't clear immediately - let the natural angle clearing process handle it
        update();
    }
}


void RadarVideoItem::clearVideoLayer(int layerIndex) {
    // Clear specific video layer from the cached image
    if (radarImage.isNull()) return;

    int w = radarImage.width();
    int h = radarImage.height();

    double mpp = metersPerPixel(m_latitude, m_zoomLevel);
    QPointF viewCenter(w / 2.0 + m_centerOffset.x(), h / 2.0 + m_centerOffset.y());

    const float angleStep = TOTAL_SWEEP_ANGLE / static_cast<float>(ANGLE_COUNT);

    // Get the color to clear
    QRgb clearColor = qRgba(
        intensityVideoColorMap[QString::number(layerIndex)].red(),
        intensityVideoColorMap[QString::number(layerIndex)].green(),
        intensityVideoColorMap[QString::number(layerIndex)].blue(),
        255
        );

    // Remove pixels matching this layer's color
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb*>(radarImage.scanLine(y));
        for (int x = 0; x < w; ++x) {
            if (line[x] == clearColor) {
                line[x] = qRgba(0, 0, 0, 0); // Set to transparent
            }
        }
    }
}

void RadarVideoItem::addPlot(int angleIndex, int rangeIndex,
                             unsigned char m_ucPlotType,
                             unsigned short m_usMode1,
                             unsigned short m_usMode2,
                             unsigned short m_usMode3,
                             float height) {

    if (angleIndex < 0 || angleIndex >= ANGLE_COUNT || rangeIndex < 0 || rangeIndex >= RANGE_SIZE)
        return;

    if (isPlotTimeEnabled){
        if(m_ucPlotType == 2){
            int differnceValue = currentAngle - angleIndex;
            if (differnceValue > 0){
                iFFPlotTime.append(differnceValue);
                iFFPlotAngle.append(differnceValue / 11.2);
                iFFPlotsCount = iFFPlotsCount + 1;
                iFFSumPlots = iFFSumPlots + differnceValue;
            }
        }
        if(m_ucPlotType == 0){
            int differnceValue = currentAngle - angleIndex;
            if (differnceValue > 0){
                primaryPlotTime.append(differnceValue);
                primaryPlotAngle.append(differnceValue / 11.2);
                plotsCount = plotsCount + 1;
                sumPlots = sumPlots + differnceValue;
            }
        }

        if (scanValue >= 4032){
            double iffminAngle = 0.0;
            double iffmaxAngle = 0.0;
            int iffminTime = 0;
            int iffmaxTime = 0;

            double minAngle = 0.0;
            double maxAngle = 0.0;
            int minTime = 0;
            int maxTime = 0;
            if (iFFSumPlots > 0 && iFFPlotsCount > 0){
                iFFAverageTime = (iFFSumPlots / iFFPlotsCount);
                if (!iFFPlotAngle.isEmpty()) {
                    auto[iffminIt, iffmaxIt] = std::minmax_element(iFFPlotAngle.begin(), iFFPlotAngle.end());
                    iffminAngle = *iffminIt;
                    iffmaxAngle = *iffmaxIt;
                }

                if (!iFFPlotTime.isEmpty()) {
                    auto[iffminIt, iffmaxIt] = std::minmax_element(iFFPlotTime.begin(), iFFPlotTime.end());
                    iffminTime = *iffminIt;
                    iffmaxTime = *iffmaxIt;
                }
            }
            if(sumPlots > 0 && plotsCount > 0) {
                averageTime = (sumPlots / plotsCount);
                if (!primaryPlotAngle.isEmpty()) {
                    auto[minIt, maxIt] = std::minmax_element(primaryPlotAngle.begin(), primaryPlotAngle.end());
                    minAngle = *minIt;
                    maxAngle = *maxIt;
                }

                if (!primaryPlotTime.isEmpty()) {
                    auto[minIt, maxIt] = std::minmax_element(primaryPlotTime.begin(), primaryPlotTime.end());
                    minTime = *minIt;
                    maxTime = *maxIt;
                }
            }
            emit plotDifferenceAndAverageTime(plotsCount, averageTime, minAngle, maxAngle, minTime, maxTime);
            emit iFFPlotDifferenceAndAverageTime(iFFPlotsCount, iFFAverageTime, iffminAngle, iffmaxAngle, iffminTime, iffmaxTime);
            scanValue = 0;
            iFFPlotsCount = 0;
            iFFSumPlots = 0;
            iFFAverageTime = 0.0;
            iFFPlotTime.clear();
            iFFPlotAngle.clear();

            plotsCount = 0;
            sumPlots = 0;
            averageTime = 0.0;
            primaryPlotTime.clear();
            primaryPlotAngle.clear();
        }
    }

    if(isPlotsEnabled){
        QColor plotColor = Qt::red;
        switch (m_ucPlotType) {
        case 1: plotColor = Qt::green; break;
        case 2: plotColor = Qt::yellow; break;
        case 3: plotColor = QColor(135, 206, 235); break;
        default: plotColor = Qt::red; break;
        }

        QMutexLocker locker(&mutex);
        plots.append({ angleIndex, rangeIndex, plotColor, m_ucPlotType,
                      m_usMode1, m_usMode2, m_usMode3, height });
        update();
    }
}

void RadarVideoItem::enablePlot(bool value) {
    isPlotsEnabled = value;
}

void RadarVideoItem::enablePlotTime(bool value) {
    isPlotTimeEnabled = value;
}

void RadarVideoItem::showPlotTextStatus(int status){
    isPlotTextEnabled = status;
}

void RadarVideoItem::showPlotTextValues(int mode1, int mode2, int mode3, int height){
    modeHeightVisible[0] = mode1;
    modeHeightVisible[1] = mode2;
    modeHeightVisible[2] = mode3;
    modeHeightVisible[3] = height;
}

void RadarVideoItem::setPlotSize(float size){
    plotSize = size;
}

void RadarVideoItem::setPlotIntensity(float intensity){
    plotIntesity = intensity;
}

void RadarVideoItem::clearAllPlots() {
    QMutexLocker locker(&mutex);
    plots.clear();
    update();
}

QPointF RadarVideoItem::geoToPixel(double lat, double lon) const {
    const double earthRadius = 6371000.0;
    const double centerLat = m_latitude;
    const double centerLon = 0.0;

    double dLat = qDegreesToRadians(lat - centerLat);
    double dLon = qDegreesToRadians(lon - centerLon);
    double meanLat = qDegreesToRadians((lat + centerLat) / 2.0);

    double dx = earthRadius * dLon * qCos(meanLat);
    double dy = earthRadius * dLat;

    double mpp = metersPerPixel(centerLat, m_zoomLevel);
    QPointF pixelOffset(dx / mpp, -dy / mpp);

    QPointF viewCenter(width() / 2.0 + m_centerOffset.x(), height() / 2.0 + m_centerOffset.y());

    return viewCenter + pixelOffset;
}

void RadarVideoItem::paint(QPainter *painter) {
    QMutexLocker locker(&mutex);

    // Regenerate radar image if needed
    regenerateRadarImage();

    painter->save();

    // Draw the cached radar image
    if (!radarImage.isNull()) {
        painter->drawImage(0, 0, radarImage);
    }

    // Calculate screen parameters for overlays
    double mpp = metersPerPixel(m_latitude, m_zoomLevel);
    double pixelsPer100km = 100000.0 / mpp;

    QPointF viewCenter(width() / 2.0 + m_centerOffset.x(),
                       height() / 2.0 + m_centerOffset.y());

    // Draw geo lines
    for (const ScreenLine &line : m_lines) {
        QPointF p1 = geoToPixelLine(line.start.x(), line.start.y());
        QPointF p2 = geoToPixelLine(line.end.x(), line.end.y());

        QPen pen(line.color, 1.5);
        painter->setPen(pen);
        painter->drawLine(p1, p2);
    }

    painter->restore();

    // Draw plots
    painter->save();

    for (const PlotPoint &plot : plots) {
        float angleDeg = plot.angle * (TOTAL_SWEEP_ANGLE / ANGLE_COUNT);
        float angleRad = qDegreesToRadians(angleDeg - 90);

        double rangeMeters = (static_cast<double>(plot.range) / RANGE_SIZE) * (MAX_RANGE_KM * 1000.0);
        double pixelRadius = rangeMeters / mpp;

        QPointF plotCenter = viewCenter + QPointF(pixelRadius * qCos(angleRad),
                                                  pixelRadius * qSin(angleRad));

        painter->setOpacity(plotIntesity);
        painter->setPen(QPen(plot.color, 1.2));

        if (plot.plotType == 1) {
            float sWidth = plotSize * 0.5;
            float sHeight = plotSize * 1.5;

            QPainterPath sPath;

            QPointF p0 = plotCenter + QPointF(sWidth * 0.5, -sHeight * 0.5);
            QPointF p2 = plotCenter;
            QPointF p4 = plotCenter + QPointF(-sWidth * 0.5, sHeight * 0.5);

            sPath.moveTo(p0);
            sPath.cubicTo(
                plotCenter + QPointF(sWidth, -sHeight * 0.7),
                plotCenter + QPointF(-sWidth, -sHeight * 0.3),
                p2
                );
            sPath.cubicTo(
                plotCenter + QPointF(sWidth, sHeight * 0.3),
                plotCenter + QPointF(-sWidth, sHeight * 0.7),
                p4
                );

            painter->setPen(QPen(plot.color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(sPath);

        } else {
            QPointF bottom = plotCenter + QPointF(0, plotSize);
            QPointF left = plotCenter + QPointF(-plotSize / 2, -plotSize);
            QPointF right = plotCenter + QPointF(plotSize / 2, -plotSize);

            QPolygonF vShape;
            vShape << left << bottom << right;
            painter->drawPolyline(vShape);
        }

        if (isPlotTextEnabled == 1) {
            QString heightText = QString("H %1").arg(int(plot.height));
            QString mode1Text = QString("M1 %1").arg(plot.mode1);
            QString mode2Text = QString("M2 %1").arg(plot.mode2);
            QString mode3Text = QString("M3 %1").arg(plot.mode3);

            const int vToTextGap = 10;

            int fontSize = int(plotSize + 3);
            int lineSpacing = int(fontSize * 1.8);

            QPointF basePos = plotCenter + QPointF(-10, plotSize + vToTextGap);
            painter->setFont(QFont("Digital Numbers", fontSize));
            painter->setPen(QPen(plot.color, 0.5));

            QPointF currentPos = basePos;

            if (modeHeightVisible[3]) {
                painter->drawText(currentPos, heightText);
                currentPos.setY(currentPos.y() + lineSpacing);
            }

            if (modeHeightVisible[0]) {
                painter->drawText(currentPos, mode1Text);
                currentPos.setY(currentPos.y() + lineSpacing);
            }

            if (modeHeightVisible[1]) {
                painter->drawText(currentPos, mode2Text);
                currentPos.setY(currentPos.y() + lineSpacing);
            }

            if (modeHeightVisible[2]) {
                painter->drawText(currentPos, mode3Text);
                currentPos.setY(currentPos.y() + lineSpacing);
            }
        }
    }

    // Draw strobe lines
    if (!strobeAngles.isEmpty()) {
        painter->setPen(QPen(Qt::red, 2.5, Qt::SolidLine));

        for (int strobeAngle : strobeAngles) {
            int angleStobe = strobeAngle * 360.0 / 4032.0;
            float angleDeg = float(angleStobe);
            float angleRad = qDegreesToRadians(angleDeg - 90.0f);

            QPointF endPoint = viewCenter + QPointF(pixelsPer100km * qCos(angleRad),
                                                    pixelsPer100km * qSin(angleRad));

            painter->drawLine(viewCenter, endPoint);
        }
    }

    painter->restore();
}


# RadarVideoItem Fix - Zoom/Pan Incremental Drawing Issue

## Problem
When video is enabled and user zooms in/out or pans, the entire radar screen redraws all angles at once instead of drawing angle-by-angle incrementally.

## Root Cause
In `regenerateRadarImage()`, there was code that detected zoom/offset changes and immediately redrew ALL stored angles from `videoDataMap`:

```cpp
// OLD CODE - BUGGY
if ((zoomChanged || offsetChanged) && !radarImage.isNull() && isAllVideoVisible && !videoDataMap.isEmpty()) {
    radarImage.fill(Qt::transparent);
    
    // This redraws EVERYTHING at once - breaks incremental behavior
    for (auto it = videoDataMap.constBegin(); it != videoDataMap.constEnd(); ++it) {
        const VideoData &vdata = it.value();
        drawAngleToImage(vdata, rgbColors, viewCenter, mpp, w, h);
    }
}
```

## Solution

### Change 1: `regenerateRadarImage()` (Lines ~168-193)
**BEFORE:**
```cpp
if ((zoomChanged || offsetChanged) && !radarImage.isNull() && isAllVideoVisible && !videoDataMap.isEmpty()) {
    radarImage.fill(Qt::transparent);
    double mpp = metersPerPixel(m_latitude, m_zoomLevel);
    QPointF viewCenter(w / 2.0 + m_centerOffset.x(), h / 2.0 + m_centerOffset.y());
    QRgb rgbColors[4];
    for (int i = 0; i < 4; ++i) {
        QColor c = intensityVideoColorMap[QString::number(i)];
        rgbColors[i] = qRgb(c.red(), c.green(), c.blue());
    }
    for (auto it = videoDataMap.constBegin(); it != videoDataMap.constEnd(); ++it) {
        const VideoData &vdata = it.value();
        drawAngleToImage(vdata, rgbColors, viewCenter, mpp, w, h);
    }
}
```

**AFTER:**
```cpp
// FIXED: If zoom or offset changed, just clear the image
// Don't redraw from videoDataMap - let updateAngle() redraw incrementally
if ((zoomChanged || offsetChanged) && !radarImage.isNull()) {
    radarImage.fill(Qt::transparent);
    // Clear the flag so we don't keep clearing
    needsImageRegeneration = false;
}
```

### Change 2: `setVideoVisibility()` (Lines ~420-430)
Added logic to clear image immediately when disabling video:

**BEFORE:**
```cpp
else if (index == 6){
    QMutexLocker locker(&mutex);
    isAllVideoVisible = visible;
    update();
}
```

**AFTER:**
```cpp
else if (index == 6){
    QMutexLocker locker(&mutex);
    isAllVideoVisible = visible;
    
    // FIXED: When disabling all video, clear the image immediately
    // When enabling, let it rebuild angle by angle
    if (!visible && !radarImage.isNull()) {
        radarImage.fill(Qt::transparent);
    }
    
    update();
}
```

### Change 3: Comments added to `updateAngle()` and `clearAngleFromImage()`
Added clarifying comments to emphasize that these functions always use current zoom/offset values (not cached), which is correct behavior.

## How It Works Now

1. **Normal Operation:**
   - `updateAngle()` is called with new radar sweep data
   - Draws angle incrementally at current zoom/offset
   - Old angles are cleared before new ones added

2. **When User Zooms:**
   - `regenerateRadarImage()` detects zoom change
   - Clears the radar image completely
   - Does NOT redraw from cache
   - As new `updateAngle()` calls come in from radar sweeps, screen rebuilds angle-by-angle at new zoom level

3. **When User Enables/Disables Video:**
   - Disable: Image cleared immediately
   - Enable: Screen stays clear, rebuilds angle-by-angle as radar scans
   - Behavior is now consistent with/without zoom

## Testing
Test these scenarios:
1. ✓ Enable video → should draw angle by angle
2. ✓ Zoom while video enabled → should clear and redraw angle by angle
3. ✓ Disable video → should clear immediately
4. ✓ Disable → Zoom → Enable → should draw angle by angle at new zoom
5. ✓ Pan while video enabled → should clear and redraw angle by angle

All scenarios now maintain incremental angle-by-angle drawing behavior.

#include "core.h"
#include "settings.h"
#include "watchface.h"

// void Watchface::updateCache() {
//   auto& cache = mSettings.mWatchface.mCache;
//   const auto& displ = mSettings.mConst.mDisplay;
//   const auto& watch = mSettings.mWatchface;
//   if (cache.mDone && displ.mRotation == cache.mRotation && watch.mType == cache.mType)
//     return; // Cache is valid!
  
//   auto fillCache = [&](Rect& r, uint8_t* data, bool units, size_t len){
//     // Render all the cache elements, the spacing of elements is given by the Rect
//     r = units ? rectU() : rectD();
//     // Clear the area!
//     // mDisplay.fillRect(r.x, r.y, r.w, r.h, 0);
//     mDisplay.alignRect(r);
//     auto size = r.size();
//     for (auto d=0; d<len; d++) {
//       units ? drawU(d) : drawD(d);
//       // Copy the area to the cache
//       for(auto y=0; y<r.h; y++) {
//         memcpy(
//           data + size * d + y * r.w / 8,
//           mDisplay.buffer + r.x/8 + (y + r.y) * mDisplay.WB_BITMAP,
//           r.w / 8);
//       }
//       // Clear the area! // TODO optimize
//       // mDisplay.fillRect(r.x, r.y, r.w, r.h, 0);
//       mDisplay.fillScreen(0);
//       // mDisplay.setTextColor(0);
//     }
//   };
//   fillCache(cache.mUnits.coord, cache.mUnits.data, true, 10);
//   fillCache(cache.mDecimal.coord, cache.mDecimal.data, false, 6);

//   cache.mDone = true;
//   cache.mRotation = displ.mRotation;
//   cache.mType = watch.mType;
// }

void Watchface::draw() {
  // Check & update the cache if needed
  updateCache();

  auto& mNow = mCore.mNow;

  auto& last = mSettings.mWatchface.mLastDraw;
  const auto needUpdateD = !last.mValid || last.mMinuteD != mNow.Minute / 10;
  const auto needUpdateU = !last.mValid || last.mMinuteU[0] != mNow.Minute % 10;

  auto copyCache2Display = [&](auto&& ptr, const Rect& r) {
    mDisplay.writeAlignedRectPacked(ptr, r);
  };
  auto copyCache2Buffer = [&](auto&& ptr, const Rect& r) {
    for (auto y=0; y<r.h; y++)
      memcpy(
        mDisplay.buffer + r.x / 8 + (r.y + y) * mDisplay.WB_BITMAP,
        ptr + y * r.w / 8,
        r.w / 8 
      );
  };

  auto copyMinutesD = [&]() {
    if (needUpdateD) {
      const auto& dec = mSettings.mWatchface.mCache.mDecimal;
      copyCache2Display(dec.data + dec.coord.size() * (mNow.Minute / 10), dec.coord);
    }
  };
  auto copyMinutesU = [&]() {
    if (needUpdateU) {
      const auto& units = mSettings.mWatchface.mCache.mUnits;
      copyCache2Display(units.data + units.coord.size() * (mNow.Minute % 10), units.coord);
    }
  };
  auto copyMinutes2Buffer = [&]() {
    const auto& dec = mSettings.mWatchface.mCache.mDecimal;
    copyCache2Buffer(dec.data + dec.coord.size() * (mNow.Minute / 10), dec.coord);
    const auto& units = mSettings.mWatchface.mCache.mUnits;
    copyCache2Buffer(units.data + units.coord.size() * (mNow.Minute % 10), units.coord);
  };

  // Common components
  auto composables = render();
  // ESP_LOGE("comp", "%d", composables.size());

  // DEBUG
  if (mSettings.mWatchface.mDebug) {
    for (auto& c : composables)
      mDisplay.drawRect(c.x, c.y, c.w, c.h, 1);
    auto ru = rectU();
    auto rd = rectD();
    mDisplay.drawRect(ru.x, ru.y, ru.w, ru.h, 1);
    mDisplay.drawRect(rd.x, rd.y, rd.w, rd.h, 1);
  }

  // Convert to aligned rotated coords, makes easier the copy
  for (auto& c : composables) {
    mDisplay.alignRect(c);
  }

  // Update display / refresh
  if (!last.mValid){
    copyMinutes2Buffer();
    mDisplay.writeAllAndRefresh();
    mDisplay.writeAll(); // We need to write the other buffer for partial updates
    last.mMinuteU[0] = last.mMinuteU[1] = mNow.Minute % 10; // Both buffers are set
  } else {
    // Pass 1: Copy all to display
    for (const auto& c : composables)      
      mDisplay.writeAlignedRect(c);
    copyMinutesD();
    copyMinutesU();

    // Manual refresh + swap buffers
    mDisplay.refresh();

    // Pass 3, copy again the updated parts to the front framebuffer
    for (const auto& c : composables)      
      mDisplay.writeAlignedRect(c);
    copyMinutesD();
    // Do not copy the minutes to frontbuffer, since we will likely
    // change it next update, saving a few 300 * 8 * 0.05us = >106us = 200us;
  }

  // Store the values for next round
  last.mValid = true;
  last.mMinuteD = mNow.Minute / 10;
  last.mMinuteU[0] = mNow.Minute % 10; // Update the front
  std::swap(last.mMinuteU[0], last.mMinuteU[1]);
}
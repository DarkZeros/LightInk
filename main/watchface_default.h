#pragma once

#include "watchface.h"

class DefaultWatchface : public Watchface {
public:
    using Watchface::Watchface; // Forward the constructor

    void draw(Display& gfx,
              const WatchfaceState* prev,
              const WatchfaceState& next) override;
};

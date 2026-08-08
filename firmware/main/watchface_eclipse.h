#pragma once

#include "watchface.h"

class EclipseWatchface : public Watchface {
  using Watchface::Watchface; // Forward the constructor

  // Needs to implement minute uni/dec draw & return Rect coordinates
  // void drawU(uint8_t d) override;
  // void drawD(uint8_t d) override;
  // Rect rectU() override {return {150,20,38,53}; }
  // Rect rectD() override {return {106,20,38,53}; }

  // Can optionally implement Other element drawing based & return vect of rect
  std::vector<Rect> render() override;
};
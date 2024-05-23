#pragma once

#include <GxEPD2_BW.h>
#include <LittleFS.h>
#include <Fonts/FreeMonoBold9pt7b.h>

// Define the display pins as used in Watchy
#define DISPLAY_CS 5
#define DISPLAY_RES 9
#define DISPLAY_DC 10
#define DISPLAY_BUSY 19

class WatchyDisplay : public GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> {
  public:
    WatchyDisplay(int8_t cs, int8_t dc, int8_t rst, int8_t busy);
    void initWatchy();
    void renderBMP(const char *filename);
    void displayBMP(const char *filename);
    void displayErrorMessage(const char *message);
};

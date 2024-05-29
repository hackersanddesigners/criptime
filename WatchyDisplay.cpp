#include "WatchyDisplay.h"
#include <Arduino.h>
#include <LittleFS.h>

WatchyDisplay::WatchyDisplay(int8_t cs, int8_t dc, int8_t rst, int8_t busy)
    : GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>(GxEPD2_154_D67(cs, dc, rst, busy)) {}

void WatchyDisplay::initWatchy() {
    Serial.println("Initializing display...");
    init(115200); // Initialize with SPI speed

    Serial.println("Clearing screen...");
    setRotation(0); // Set rotation to 0 for default orientation
    setFullWindow(); // Use the full window for updates
    fillScreen(GxEPD_WHITE); // Clear the screen to white
    display(true); // Perform a full refresh
    delay(2000); // Wait for the refresh to complete
    Serial.println("Screen cleared.");
}

void WatchyDisplay::displayErrorMessage(const char *message) {
    fillScreen(GxEPD_WHITE);
    setTextColor(GxEPD_BLACK);
    setFont(&FreeMonoBold9pt7b);
    setCursor(10, 30);
    println(message);
    display(true);
}

void WatchyDisplay::displayBMP(const char *filename) {
  renderBMP(filename);
  display(true); // Update the display with the new image
}

void WatchyDisplay::renderBMP(const char *filename) {
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        displayErrorMessage("FS mount failed");
        return;
    }
    
    File file = LittleFS.open(filename, "r");
    if (!file) {
        Serial.println("Failed to open file for reading");
        displayErrorMessage("Failed to open file");
        return;
    }
    
    // Check BMP header
    uint8_t header[54];
    file.read(header, 54);

    if (header[0] != 'B' || header[1] != 'M') {
        Serial.println("Not a valid BMP file");
        displayErrorMessage("Invalid BMP file");
        file.close();
        return;
    }

    // Read BMP header
    uint32_t dataOffset = *(uint32_t*)&header[10];
    uint32_t width = *(uint32_t*)&header[18];
    uint32_t height = *(uint32_t*)&header[22];
    uint16_t bitsPerPixel = *(uint16_t*)&header[28];

    if (width != GxEPD2_154_D67::WIDTH || height != GxEPD2_154_D67::HEIGHT) {
        Serial.println("Unsupported BMP format");
        displayErrorMessage("Unsupported BMP format");
        file.close();
        return;
    }

    if (bitsPerPixel != 8 && bitsPerPixel != 24) {
        Serial.println("Only 8-bit or 24-bit BMP files are supported");
        displayErrorMessage("Only 8-bit/24-bit BMP supported");
        file.close();
        return;
    }

    Serial.print("Rendering watchface "); 
    // print the filename char array as string
    Serial.println( filename );
    // Set the file pointer to the start of the bitmap data
    file.seek(dataOffset, SeekSet);

    // Read image data and draw
    uint8_t buffer[GxEPD2_154_D67::WIDTH * (bitsPerPixel / 8)]; // 3 bytes per pixel (RGB) for 24-bit or 1 byte per pixel for 8-bit
    for (int y = height - 1; y >= 0; y--) { // BMP files are stored bottom-to-top
        file.read(buffer, sizeof(buffer));
        for (int x = 0; x < width; x++) {
            uint8_t color;
            if (bitsPerPixel == 24) {
                uint8_t r = buffer[x * 3];
                uint8_t g = buffer[x * 3 + 1];
                uint8_t b = buffer[x * 3 + 2];
                uint8_t gray = (r + g + b) / 3;
                color = gray > 127 ? GxEPD_WHITE : GxEPD_BLACK;
            } else { // 8-bit
                uint8_t gray = buffer[x];
                color = gray > 127 ? GxEPD_WHITE : GxEPD_BLACK;
            }
            drawPixel(x, y, color);
        }
    }
    file.close();
}

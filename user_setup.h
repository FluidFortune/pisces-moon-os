#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define ST7789_DRIVER
// ... keep everything else you already have below ...
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON

#define TFT_MISO -1 // Not used
#define TFT_MOSI 41
#define TFT_SCLK 40
#define TFT_CS   10
#define TFT_DC   11
#define TFT_RST  9
#define TFT_BL   42

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SPI_FREQUENCY  40000000
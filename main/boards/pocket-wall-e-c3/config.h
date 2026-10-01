#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// I2S Audio Pinout for ESP32-C3 Pocket Wall-E
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_1
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_3
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_8

// Button Pinout
#define BOOT_BUTTON_GPIO    GPIO_NUM_9

// I2C OLED Display (SSD1306 128x64)
// Note: OLED PCB silk labels 0x78/0x7A are 8-bit addresses.
// ESP-IDF requires standard 7-bit address:
// 0x78 -> 0x3C (0x78 >> 1)
// 0x7A -> 0x3D (0x7A >> 1)
#define DISPLAY_I2C_ADDR    0x3C
#define DISPLAY_SDA_PIN     GPIO_NUM_21
#define DISPLAY_SCL_PIN     GPIO_NUM_20
#define DISPLAY_WIDTH       128
#define DISPLAY_HEIGHT      64

#define DISPLAY_MIRROR_X    false
#define DISPLAY_MIRROR_Y    false

#endif // _BOARD_CONFIG_H_

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TABYMOJI_WIDTH  48
#define TABYMOJI_HEIGHT 48
#define TABYMOJI_STRIDE 6
#define TABYMOJI_FRAME_BYTES 288

typedef struct {
    const char* id;
    bool loop;
    uint16_t frame_delay_ms;
    uint16_t frame_count;
    const uint8_t* const* frames; // Array of pointers to 1-bit frame bitmaps (MSB first)
} TabymojiAnimation;

const TabymojiAnimation* Tabymoji_GetById(const char* id);
const TabymojiAnimation* Tabymoji_GetByEmotion(const char* emotion);
const TabymojiAnimation* Tabymoji_GetByStatus(const char* status);
size_t Tabymoji_GetAnimationCount(void);
const TabymojiAnimation* Tabymoji_GetByIndex(size_t index);

#ifdef __cplusplus
}
#endif

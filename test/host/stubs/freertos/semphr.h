#pragma once
typedef void *SemaphoreHandle_t;
#define xSemaphoreCreateMutex() ((void *)1)
static inline int xSemaphoreTake(SemaphoreHandle_t a, unsigned b) { return 1; }
#define xSemaphoreGive(a) ((void)0)

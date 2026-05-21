#ifndef RINHA_FASTVECTOR_H
#define RINHA_FASTVECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FASTVECTOR_DIMENSIONS 14
#define FASTVECTOR_QUANT_SCALE 10000
#define FASTVECTOR_SENTINEL (-10000)

enum {
    FASTVECTOR_DIM_AMOUNT = 0,
    FASTVECTOR_DIM_INSTALLMENTS = 1,
    FASTVECTOR_DIM_AMOUNT_VS_AVG = 2,
    FASTVECTOR_DIM_HOUR_OF_DAY = 3,
    FASTVECTOR_DIM_DAY_OF_WEEK = 4,
    FASTVECTOR_DIM_MINUTES_SINCE_LAST_TX = 5,
    FASTVECTOR_DIM_KM_FROM_LAST_TX = 6,
    FASTVECTOR_DIM_KM_FROM_HOME = 7,
    FASTVECTOR_DIM_TX_COUNT_24H = 8,
    FASTVECTOR_DIM_IS_ONLINE = 9,
    FASTVECTOR_DIM_CARD_PRESENT = 10,
    FASTVECTOR_DIM_UNKNOWN_MERCHANT = 11,
    FASTVECTOR_DIM_MCC_RISK = 12,
    FASTVECTOR_DIM_MERCHANT_AVG_AMOUNT = 13
};

typedef struct {
    int64_t seconds;
    int hour;
    int weekday;
} fastvector_timestamp;

bool fastvector_vectorize(const char *body, size_t len, int16_t out[FASTVECTOR_DIMENSIONS]);
int16_t fastvector_quantize(float value);
bool fastvector_parse_timestamp(const char *raw, size_t len, fastvector_timestamp *out);
int16_t fastvector_mcc_risk_quantized(const char *code, size_t len);

#endif

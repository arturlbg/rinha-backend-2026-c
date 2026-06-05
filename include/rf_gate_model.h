#ifndef RINHA_RF_GATE_MODEL_H
#define RINHA_RF_GATE_MODEL_H

#include <stdint.h>

#include "fastvector.h"

#define RF_GATE_MODEL_ID "vector_poly_rf30_d12_leaf20_sqrt_2"
#define RF_GATE_THRESHOLD_NAME "asym_0.08_0.99"
#define RF_GATE_TREE_COUNT 30u
#define RF_GATE_FEATURE_COUNT 56u
#define RF_GATE_LOW_THRESHOLD 0.08000000
#define RF_GATE_HIGH_THRESHOLD 0.99000000
#define RF_GATE_NODE_COUNT 8490u

typedef enum {
    RF_GATE_DECISION_FALLBACK = 0,
    RF_GATE_DECISION_LEGIT = 1,
    RF_GATE_DECISION_FRAUD = 2
} RfGateDecision;

void rf_gate_vector_poly_features(const int16_t query[FASTVECTOR_DIMENSIONS],
                                  float features[RF_GATE_FEATURE_COUNT]);
double rf_gate_predict_fraud_probability_from_features(const float features[RF_GATE_FEATURE_COUNT]);
double rf_gate_predict_fraud_probability(const int16_t query[FASTVECTOR_DIMENSIONS]);
RfGateDecision rf_gate_decide_probability(double probability);
RfGateDecision rf_gate_decide(const int16_t query[FASTVECTOR_DIMENSIONS],
                              double *out_probability);

#endif

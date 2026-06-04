#include "rf_gate_fixture.h"
#include "rf_gate_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void test_feature_parity(void) {
    for (uint32_t row = 0; row < RF_GATE_FIXTURE_COUNT; row++) {
        float features[RF_GATE_FEATURE_COUNT];
        rf_gate_vector_poly_features(RF_GATE_FIXTURE_VECTORS[row], features);
        for (uint32_t feature = 0; feature < RF_GATE_FEATURE_COUNT; feature++) {
            float expected = RF_GATE_FIXTURE_FEATURES[row][feature];
            float got = features[feature];
            float diff = fabsf(got - expected);
            if (diff > 0.03125f) {
                fprintf(stderr,
                        "feature parity failed row=%u feature=%u got=%f expected=%f diff=%f\n",
                        row,
                        feature,
                        (double)got,
                        (double)expected,
                        (double)diff);
                _Exit(1);
            }
        }
    }
}

static void test_probability_and_decision_parity(void) {
    for (uint32_t row = 0; row < RF_GATE_FIXTURE_COUNT; row++) {
        double probability = rf_gate_predict_fraud_probability(RF_GATE_FIXTURE_VECTORS[row]);
        double expected = RF_GATE_FIXTURE_PROBABILITY[row];
        double diff = fabs(probability - expected);
        if (diff > 1e-7) {
            fprintf(stderr,
                    "probability parity failed row=%u got=%.17g expected=%.17g diff=%.17g\n",
                    row,
                    probability,
                    expected,
                    diff);
            _Exit(1);
        }
        RfGateDecision decision = rf_gate_decide_probability(probability);
        if ((uint8_t)decision != RF_GATE_FIXTURE_DECISION[row]) {
            fprintf(stderr,
                    "decision parity failed row=%u got=%u expected=%u probability=%.17g\n",
                    row,
                    (unsigned)decision,
                    (unsigned)RF_GATE_FIXTURE_DECISION[row],
                    probability);
            _Exit(1);
        }
    }
}

int main(void) {
    test_feature_parity();
    test_probability_and_decision_parity();
    return 0;
}

#include "fastvector.h"

#include <string.h>

typedef struct {
    const char *data;
    size_t len;
} slice;

static bool is_space(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static slice trim_spaces(slice value) {
    while (value.len > 0 && is_space(value.data[0])) {
        value.data++;
        value.len--;
    }
    while (value.len > 0 && is_space(value.data[value.len - 1])) {
        value.len--;
    }
    return value;
}

static bool has_prefix(slice value, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return value.len >= prefix_len && memcmp(value.data, prefix, prefix_len) == 0;
}

static bool key_at(const char *src, size_t len, size_t pos, const char *key) {
    size_t key_len = strlen(key);
    if (pos + key_len + 2 > len) {
        return false;
    }
    return src[pos] == '"' &&
           memcmp(src + pos + 1, key, key_len) == 0 &&
           src[pos + key_len + 1] == '"';
}

static bool raw_value(slice src, const char *key, slice *out) {
    size_t key_len = strlen(key);
    if (src.len < key_len + 2) {
        return false;
    }
    for (size_t pos = 0; pos + key_len + 2 <= src.len; pos++) {
        if (!key_at(src.data, src.len, pos, key)) {
            continue;
        }
        size_t i = pos + key_len + 2;
        while (i < src.len && is_space(src.data[i])) {
            i++;
        }
        if (i >= src.len || src.data[i] != ':') {
            continue;
        }
        i++;
        while (i < src.len && is_space(src.data[i])) {
            i++;
        }
        if (i >= src.len) {
            return false;
        }
        out->data = src.data + i;
        out->len = src.len - i;
        return true;
    }
    return false;
}

static int matching_end(slice src, char open, char close) {
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < src.len; i++) {
        char c = src.data[i];
        if (in_string) {
            if (c == '\\') {
                i++;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == open) {
            depth++;
        }
        if (c == close) {
            depth--;
            if (depth == 0) {
                return (int)i + 1;
            }
        }
    }
    return -1;
}

static bool object_field(slice src, const char *key, slice *out) {
    slice value;
    if (!raw_value(src, key, &value)) {
        return false;
    }
    value = trim_spaces(value);
    if (value.len == 0 || value.data[0] != '{') {
        return false;
    }
    int end = matching_end(value, '{', '}');
    if (end <= 0) {
        return false;
    }
    out->data = value.data;
    out->len = (size_t)end;
    return true;
}

static bool array_field(slice src, const char *key, slice *out) {
    slice value;
    if (!raw_value(src, key, &value)) {
        return false;
    }
    value = trim_spaces(value);
    if (value.len == 0 || value.data[0] != '[') {
        return false;
    }
    int end = matching_end(value, '[', ']');
    if (end <= 0) {
        return false;
    }
    out->data = value.data;
    out->len = (size_t)end;
    return true;
}

static bool string_field(slice src, const char *key, slice *out) {
    slice value;
    if (!raw_value(src, key, &value)) {
        return false;
    }
    value = trim_spaces(value);
    if (value.len == 0 || value.data[0] != '"') {
        return false;
    }
    for (size_t i = 1; i < value.len; i++) {
        if (value.data[i] == '"') {
            out->data = value.data + 1;
            out->len = i - 1;
            return true;
        }
        if (value.data[i] == '\\') {
            i++;
        }
    }
    return false;
}

static bool bool_field(slice src, const char *key, bool *out) {
    slice value;
    if (!raw_value(src, key, &value)) {
        return false;
    }
    value = trim_spaces(value);
    if (has_prefix(value, "true")) {
        *out = true;
        return true;
    }
    if (has_prefix(value, "false")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_number(slice src, float *out) {
    src = trim_spaces(src);
    if (src.len == 0) {
        return false;
    }
    size_t i = 0;
    float sign = 1.0f;
    if (src.data[i] == '-') {
        sign = -1.0f;
        i++;
    }
    if (i >= src.len || src.data[i] < '0' || src.data[i] > '9') {
        return false;
    }
    float value = 0.0f;
    while (i < src.len && src.data[i] >= '0' && src.data[i] <= '9') {
        value = value * 10.0f + (float)(src.data[i] - '0');
        i++;
    }
    if (i < src.len && src.data[i] == '.') {
        i++;
        float scale = 0.1f;
        while (i < src.len && src.data[i] >= '0' && src.data[i] <= '9') {
            value += (float)(src.data[i] - '0') * scale;
            scale *= 0.1f;
            i++;
        }
    }
    *out = value * sign;
    return true;
}

static bool number_field(slice src, const char *key, float *out) {
    slice value;
    if (!raw_value(src, key, &value)) {
        return false;
    }
    return parse_number(value, out);
}

static int parse_n_digits(const char *raw, int start, int end, bool *ok) {
    int value = 0;
    for (int i = start; i < end; i++) {
        if (raw[i] < '0' || raw[i] > '9') {
            *ok = false;
            return 0;
        }
        value = value * 10 + (raw[i] - '0');
    }
    *ok = true;
    return value;
}

static int div_floor_int(int a, int b) {
    if (a >= 0) {
        return a / b;
    }
    return -((-a + b - 1) / b);
}

static int64_t days_from_civil(int year, int month, int day) {
    int y = year;
    int m = month;
    if (m <= 2) {
        y--;
    }
    int era = div_floor_int(y, 400);
    int yoe = y - era * 400;
    int mp = m;
    if (mp > 2) {
        mp -= 3;
    } else {
        mp += 9;
    }
    int doy = (153 * mp + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static int days_in_month(int year, int month) {
    switch (month) {
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    case 2:
        if (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)) {
            return 29;
        }
        return 28;
    default:
        return 31;
    }
}

bool fastvector_parse_timestamp(const char *raw, size_t len, fastvector_timestamp *out) {
    if (out == NULL || len != 20 ||
        raw[4] != '-' || raw[7] != '-' || raw[10] != 'T' ||
        raw[13] != ':' || raw[16] != ':' || raw[19] != 'Z') {
        return false;
    }

    bool ok = false;
    int year = parse_n_digits(raw, 0, 4, &ok);
    if (!ok) {
        return false;
    }
    int month = parse_n_digits(raw, 5, 7, &ok);
    if (!ok) {
        return false;
    }
    int day = parse_n_digits(raw, 8, 10, &ok);
    if (!ok) {
        return false;
    }
    int hour = parse_n_digits(raw, 11, 13, &ok);
    if (!ok) {
        return false;
    }
    int minute = parse_n_digits(raw, 14, 16, &ok);
    if (!ok) {
        return false;
    }
    int second = parse_n_digits(raw, 17, 19, &ok);
    if (!ok) {
        return false;
    }

    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    int64_t days = days_from_civil(year, month, day);
    int weekday = (int)((days + 3) % 7);
    if (weekday < 0) {
        weekday += 7;
    }

    out->seconds = days * 86400 + (int64_t)(hour * 3600 + minute * 60 + second);
    out->hour = hour;
    out->weekday = weekday;
    return true;
}

static float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

int16_t fastvector_quantize(float value) {
    if (value <= -1.0f) {
        return (int16_t)FASTVECTOR_SENTINEL;
    }
    if (value >= 1.0f) {
        return (int16_t)FASTVECTOR_QUANT_SCALE;
    }
    if (value >= 0.0f) {
        return (int16_t)(value * (float)FASTVECTOR_QUANT_SCALE + 0.5f);
    }
    return (int16_t)(value * (float)FASTVECTOR_QUANT_SCALE - 0.5f);
}

static float amount_vs_average(float amount, float avg_amount) {
    if (avg_amount <= 0.0f) {
        if (amount <= 0.0f) {
            return 0.0f;
        }
        return 1.0f;
    }
    return clamp01((amount / avg_amount) / 10.0f);
}

static float bool_float(bool value) {
    return value ? 1.0f : 0.0f;
}

static float mcc_risk_float(slice code) {
    if (code.len == 4 && memcmp(code.data, "5411", 4) == 0) {
        return 0.15f;
    }
    if (code.len == 4 && memcmp(code.data, "5812", 4) == 0) {
        return 0.30f;
    }
    if (code.len == 4 && memcmp(code.data, "5912", 4) == 0) {
        return 0.20f;
    }
    if (code.len == 4 && memcmp(code.data, "5944", 4) == 0) {
        return 0.45f;
    }
    if (code.len == 4 && memcmp(code.data, "7801", 4) == 0) {
        return 0.80f;
    }
    if (code.len == 4 && memcmp(code.data, "7802", 4) == 0) {
        return 0.75f;
    }
    if (code.len == 4 && memcmp(code.data, "7995", 4) == 0) {
        return 0.85f;
    }
    if (code.len == 4 && memcmp(code.data, "4511", 4) == 0) {
        return 0.35f;
    }
    if (code.len == 4 && memcmp(code.data, "5311", 4) == 0) {
        return 0.25f;
    }
    if (code.len == 4 && memcmp(code.data, "5999", 4) == 0) {
        return 0.50f;
    }
    return 0.50f;
}

int16_t fastvector_mcc_risk_quantized(const char *code, size_t len) {
    return fastvector_quantize(mcc_risk_float((slice){code, len}));
}

static bool known_merchant(slice array, slice merchant_id) {
    size_t i = 0;
    while (i < array.len) {
        if (array.data[i] != '"') {
            i++;
            continue;
        }
        size_t start = i + 1;
        i = start;
        while (i < array.len && array.data[i] != '"') {
            if (array.data[i] == '\\') {
                i++;
            }
            i++;
        }
        if (i <= array.len && i - start == merchant_id.len &&
            memcmp(array.data + start, merchant_id.data, merchant_id.len) == 0) {
            return true;
        }
        i++;
    }
    return false;
}

bool fastvector_vectorize(const char *body, size_t len, int16_t out[FASTVECTOR_DIMENSIONS]) {
    if (body == NULL || out == NULL) {
        return false;
    }
    slice root = {body, len};
    slice transaction;
    slice customer;
    slice merchant;
    slice terminal;
    if (!object_field(root, "transaction", &transaction) ||
        !object_field(root, "customer", &customer) ||
        !object_field(root, "merchant", &merchant) ||
        !object_field(root, "terminal", &terminal)) {
        return false;
    }

    float amount;
    float installments;
    float customer_avg;
    float tx_count_24h;
    float merchant_avg;
    float km_from_home;
    slice requested_raw;
    slice known_raw;
    slice merchant_id;
    slice mcc;
    bool is_online;
    bool card_present;

    if (!number_field(transaction, "amount", &amount) ||
        !number_field(transaction, "installments", &installments) ||
        !string_field(transaction, "requested_at", &requested_raw) ||
        !number_field(customer, "avg_amount", &customer_avg) ||
        !number_field(customer, "tx_count_24h", &tx_count_24h) ||
        !array_field(customer, "known_merchants", &known_raw) ||
        !string_field(merchant, "id", &merchant_id) ||
        !string_field(merchant, "mcc", &mcc) ||
        !number_field(merchant, "avg_amount", &merchant_avg) ||
        !bool_field(terminal, "is_online", &is_online) ||
        !bool_field(terminal, "card_present", &card_present) ||
        !number_field(terminal, "km_from_home", &km_from_home)) {
        return false;
    }

    fastvector_timestamp requested;
    if (!fastvector_parse_timestamp(requested_raw.data, requested_raw.len, &requested)) {
        return false;
    }

    for (int i = 0; i < FASTVECTOR_DIMENSIONS; i++) {
        out[i] = 0;
    }

    out[FASTVECTOR_DIM_AMOUNT] = fastvector_quantize(clamp01(amount / 10000.0f));
    out[FASTVECTOR_DIM_INSTALLMENTS] = fastvector_quantize(clamp01(installments / 12.0f));
    out[FASTVECTOR_DIM_AMOUNT_VS_AVG] = fastvector_quantize(amount_vs_average(amount, customer_avg));
    out[FASTVECTOR_DIM_HOUR_OF_DAY] = fastvector_quantize((float)requested.hour / 23.0f);
    out[FASTVECTOR_DIM_DAY_OF_WEEK] = fastvector_quantize((float)requested.weekday / 6.0f);
    out[FASTVECTOR_DIM_KM_FROM_HOME] = fastvector_quantize(clamp01(km_from_home / 1000.0f));
    out[FASTVECTOR_DIM_TX_COUNT_24H] = fastvector_quantize(clamp01(tx_count_24h / 20.0f));
    out[FASTVECTOR_DIM_IS_ONLINE] = fastvector_quantize(bool_float(is_online));
    out[FASTVECTOR_DIM_CARD_PRESENT] = fastvector_quantize(bool_float(card_present));
    out[FASTVECTOR_DIM_UNKNOWN_MERCHANT] = fastvector_quantize(bool_float(!known_merchant(known_raw, merchant_id)));
    out[FASTVECTOR_DIM_MCC_RISK] = fastvector_quantize(mcc_risk_float(mcc));
    out[FASTVECTOR_DIM_MERCHANT_AVG_AMOUNT] = fastvector_quantize(clamp01(merchant_avg / 10000.0f));

    slice last_value;
    if (!raw_value(root, "last_transaction", &last_value)) {
        return false;
    }
    last_value = trim_spaces(last_value);
    if (has_prefix(last_value, "null")) {
        out[FASTVECTOR_DIM_MINUTES_SINCE_LAST_TX] = (int16_t)FASTVECTOR_SENTINEL;
        out[FASTVECTOR_DIM_KM_FROM_LAST_TX] = (int16_t)FASTVECTOR_SENTINEL;
        return true;
    }

    slice last;
    slice last_timestamp_raw;
    float km_from_current;
    if (!object_field(root, "last_transaction", &last) ||
        !string_field(last, "timestamp", &last_timestamp_raw) ||
        !number_field(last, "km_from_current", &km_from_current)) {
        return false;
    }

    fastvector_timestamp last_timestamp;
    if (!fastvector_parse_timestamp(last_timestamp_raw.data, last_timestamp_raw.len, &last_timestamp)) {
        return false;
    }
    float minutes = (float)(requested.seconds - last_timestamp.seconds) / 60.0f;
    out[FASTVECTOR_DIM_MINUTES_SINCE_LAST_TX] = fastvector_quantize(clamp01(minutes / 1440.0f));
    out[FASTVECTOR_DIM_KM_FROM_LAST_TX] = fastvector_quantize(clamp01(km_from_current / 1000.0f));
    return true;
}

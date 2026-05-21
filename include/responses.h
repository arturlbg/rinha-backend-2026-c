#ifndef RINHA_RESPONSES_H
#define RINHA_RESPONSES_H

#include <stddef.h>

typedef struct {
    const char *data;
    size_t len;
} http_response;

extern const http_response RESPONSE_READY;
extern const http_response RESPONSE_FRAUD_APPROVED;
extern const http_response RESPONSE_FRAUD[6];
extern const http_response RESPONSE_NOT_FOUND;
extern const http_response RESPONSE_METHOD_NOT_ALLOWED;
extern const http_response RESPONSE_BAD_REQUEST;

const http_response *response_for_fraud_count(unsigned int fraud_count);

#endif

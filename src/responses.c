#include "responses.h"

static const char ready_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "ok";

static const char fraud_approved_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 33\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0}";

static const char fraud_1_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 35\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0.2}";

static const char fraud_2_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 35\r\n"
    "\r\n"
    "{\"approved\":true,\"fraud_score\":0.4}";

static const char fraud_3_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 36\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":0.6}";

static const char fraud_4_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 36\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":0.8}";

static const char fraud_5_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 34\r\n"
    "\r\n"
    "{\"approved\":false,\"fraud_score\":1}";

static const char not_found_response[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char method_not_allowed_response[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char bad_request_response[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

const http_response RESPONSE_READY = {ready_response, sizeof(ready_response) - 1};
const http_response RESPONSE_FRAUD_APPROVED = {fraud_approved_response, sizeof(fraud_approved_response) - 1};
const http_response RESPONSE_FRAUD[6] = {
    {fraud_approved_response, sizeof(fraud_approved_response) - 1},
    {fraud_1_response, sizeof(fraud_1_response) - 1},
    {fraud_2_response, sizeof(fraud_2_response) - 1},
    {fraud_3_response, sizeof(fraud_3_response) - 1},
    {fraud_4_response, sizeof(fraud_4_response) - 1},
    {fraud_5_response, sizeof(fraud_5_response) - 1},
};
const http_response RESPONSE_NOT_FOUND = {not_found_response, sizeof(not_found_response) - 1};
const http_response RESPONSE_METHOD_NOT_ALLOWED = {method_not_allowed_response, sizeof(method_not_allowed_response) - 1};
const http_response RESPONSE_BAD_REQUEST = {bad_request_response, sizeof(bad_request_response) - 1};

const http_response *response_for_fraud_count(unsigned int fraud_count) {
    if (fraud_count > 5) {
        fraud_count = 5;
    }
    return &RESPONSE_FRAUD[fraud_count];
}

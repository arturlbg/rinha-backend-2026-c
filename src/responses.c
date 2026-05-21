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
const http_response RESPONSE_NOT_FOUND = {not_found_response, sizeof(not_found_response) - 1};
const http_response RESPONSE_METHOD_NOT_ALLOWED = {method_not_allowed_response, sizeof(method_not_allowed_response) - 1};
const http_response RESPONSE_BAD_REQUEST = {bad_request_response, sizeof(bad_request_response) - 1};

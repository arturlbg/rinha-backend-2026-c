#ifndef RINHA_FDPASS_H
#define RINHA_FDPASS_H

#include "raw_http.h"

int fdpass_serve(const char *control_path, const raw_http_app *app);
int fdpass_recv_one_fd(int control_fd);
int fdpass_send_one_fd(int control_fd, int fd);

#endif

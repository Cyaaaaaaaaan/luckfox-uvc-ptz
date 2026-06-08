#pragma once
#include <stdint.h>
#include <sys/socket.h>

void visca_handle(int sock, struct sockaddr *src, socklen_t srclen,
                  const uint8_t *buf, int len);

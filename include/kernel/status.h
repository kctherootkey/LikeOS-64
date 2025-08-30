// LikeOS-64 - Common status and error codes
#ifndef LIKEOS_STATUS_H
#define LIKEOS_STATUS_H

typedef enum {
    ST_OK = 0,
    ST_ERR = -1,
    ST_UNSUPPORTED = -2,
    ST_NO_DEVICE = -3,
    ST_TIMEOUT = -4,
    ST_IO = -5,
    ST_NOT_FOUND = -6,
    ST_EOF = -7,
    ST_INVALID = -8,
    ST_NOMEM = -9,
    ST_AGAIN = -10, // temporary condition, try again later
    ST_BUSY = -11,
} status_t;

#endif // LIKEOS_STATUS_H

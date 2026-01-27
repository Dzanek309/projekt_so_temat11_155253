#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

void die_perror(const char* what) {
    perror(what);
    exit(1);
}

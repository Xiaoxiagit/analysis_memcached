#pragma once

#include <cstdint>
#include <string.h>

typedef uint32_t (*hash_func)(const void *key, size_t length);
hash_func hash;

enum hashfunc_type {
    JENKINS_HASH=0, MURMUR3_HASH
};

int hash_init(enum hashfunc_type type);

#pragma once
#include "esp_partition.h"

// Implemented in the standalone bootstrap.cpp and reused by the application.
namespace ota {
bool layoutCompatible();
bool imageChecksum(const esp_partition_t* partition, char output[65], uint32_t* imageSize);
bool fetchChecksum(char output[65]);
bool install(const char* expected);
}

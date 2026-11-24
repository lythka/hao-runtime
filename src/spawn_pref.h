#pragma once
#include <spawn.h>
#include <stdbool.h>

bool hao_spawn_attr_abi(const void *original);
bool hao_spawn_host_attr(const void *original, const posix_spawnattr_t *guest, posix_spawnattr_t *out);
bool hao_spawn_generation(const char *path);
void hao_spawn_process_start(void);

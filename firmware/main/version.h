#pragma once

/* 固件版本。GET_DEVICE_INFO 会回报这三个值。 */
#define ESPGPU_FW_VERSION_MAJOR 0
#define ESPGPU_FW_VERSION_MINOR 1
#define ESPGPU_FW_VERSION_PATCH 0

/* 构建时可由 CMake 覆盖；未定义则为 0。 */
#ifndef ESPGPU_FW_GIT
#define ESPGPU_FW_GIT 0
#endif

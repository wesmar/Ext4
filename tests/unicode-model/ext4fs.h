/* Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 * Host stand-in for the driver's umbrella header: src/unicode needs only
 * the Windows base types.
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <string.h>

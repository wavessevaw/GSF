/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "config.h"
#include <stdint.h>

/* Atomic, bounded, per-game control file. A malformed/partial revision is ignored. */
struct afmf_config gsf_live_config(void);

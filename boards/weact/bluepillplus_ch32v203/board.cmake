# Copyright (c) 2025 MASSDRIVER EI (massdriver.space)
# SPDX-License-Identifier: Apache-2.0

board_runner_args(minichlink)
include(${ZEPHYR_BASE}/boards/common/minichlink.board.cmake)

board_runner_args(openocd "--use-elf" "--cmd-reset-halt" "halt")
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)

# Copyright (c) 2025 scholzri
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_USB_DC_B91 usb_dc_b91.c)

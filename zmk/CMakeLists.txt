if(CONFIG_USB_DC_B91 OR CONFIG_BT_HCI_B91)
  zephyr_include_directories(include)
endif()

add_subdirectory_ifdef(CONFIG_USB_DC_B91 drivers/usb)
add_subdirectory_ifdef(CONFIG_BT_HCI_B91 drivers/bluetooth)
add_subdirectory_ifdef(CONFIG_LED_STRIP_B91_SPI drivers/led_strip)
add_subdirectory_ifdef(CONFIG_BATTERY_B91_ADC drivers/sensor)
add_subdirectory_ifdef(CONFIG_WDT_B91 drivers/watchdog)

if(CONFIG_BT_HCI_B91_CTLR_BLOB)
  zephyr_link_libraries(${CMAKE_CURRENT_SOURCE_DIR}/lib/liblt_9518_zephyr.a)
endif()

if(CONFIG_BOOT_DIAG)
  zephyr_library_named(boot_diag)
  zephyr_library_sources(src/boot_diag.c)
endif()

if(CONFIG_IMG_MANAGER AND CONFIG_WDT_B91)
  zephyr_library_named(mcuboot_confirm)
  zephyr_library_sources(src/mcuboot_confirm.c)
endif()

if(CONFIG_POWEROFF)
  zephyr_library_named(poweroff_b91)
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/drivers/bluetooth)
  zephyr_library_sources(src/poweroff.c)
endif()

if(CONFIG_FLASH_MGMT)
  zephyr_library_named(flash_mgmt)
  zephyr_library_sources(src/flash_mgmt.c)
endif()

if(CONFIG_USB_DC_B91 AND CONFIG_ZMK_USB)
  zephyr_library_named(usb_cdc_recover)
  zephyr_library_sources(src/usb_cdc_recover.c)
endif()

if(CONFIG_OPENLL_MGMT)
  zephyr_library_named(openll_mgmt)
  zephyr_library_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bluetooth
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bluetooth/openll)
  zephyr_library_sources(src/openll_mgmt.c)
endif()

if(CONFIG_RGB_MGMT)
  zephyr_library_named(rgb_mgmt)
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
  zephyr_library_sources(src/rgb_mgmt.c)
endif()

if(CONFIG_RAINY75_CONFIG)
  zephyr_library_named(rainy75_config)
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
  zephyr_library_sources(
    src/config/cfg_registry.c
    src/config/cfg_table.c
    src/config/cfg_store.c
    src/config/cfg_mgmt.c
  )
endif()

if(CONFIG_RAINY_RGB)
  zephyr_library_named(rainy_rgb)
  zephyr_library_include_directories(
    ${APPLICATION_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  zephyr_library_sources(
    src/behaviors/behavior_rainy_rgb.c
    src/rainy_rgb/color.c
    src/rainy_rgb/effects.c
    src/rainy_rgb/led_map.c
    src/rainy_rgb/reactive.c
    src/rainy_rgb/engine.c
    src/rainy_rgb/state.c
    src/rainy_rgb/overlay.c
    src/rainy_rgb/ble_status.c
    src/rainy_rgb/zmk_adapter.c
  )
endif()

# BLE policy on top of ZMK's public API: the &bt_sel_ble behavior and the
# open profile timeout (see docs/zmk-firmware.md, "BLE policy module").
if(CONFIG_ZMK_BLE)
  zephyr_library_named(rainy75_ble)
  zephyr_library_include_directories(
    ${APPLICATION_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  zephyr_library_sources(src/behaviors/behavior_bt_sel_ble.c)
  if(CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT GREATER 0)
    zephyr_library_sources(
      src/ble_open_profile/open_profile.c
      src/ble_open_profile/open_profile_timeout.c
    )
  endif()
endif()

# Embed git version at compile time
execute_process(
  COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${ZEPHYR_BASE}/..
  OUTPUT_VARIABLE BUILD_GIT_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT BUILD_GIT_VERSION)
  set(BUILD_GIT_VERSION "unknown")
endif()
zephyr_compile_definitions(BUILD_GIT_VERSION="${BUILD_GIT_VERSION}")

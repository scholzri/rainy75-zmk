zephyr_library()
if(CONFIG_BATTERY_B91_ADC)
  zephyr_library_sources(battery_b91_adc.c)
endif()

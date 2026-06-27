zephyr_library()
zephyr_library_sources_ifdef(CONFIG_BT_HCI_B91 hci_b91.c b91_bt.c)

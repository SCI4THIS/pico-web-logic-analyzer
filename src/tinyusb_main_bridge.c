#define main tinyusb_example_main_unused
#define httpd_init tinyusb_httpd_init_unused

#include "main.c"

#undef httpd_init
#undef main

void tinyusb_httpd_init_unused(void) { }
void tinyusb_network_init(void)
{
  /* initialize TinyUSB */
  board_init();

  // init device stack on configured roothub port
  tusb_rhport_init_t dev_init = {
    .role = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_AUTO
  };
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  board_init_after_tusb();

  /* initialize lwip, dhcp-server, dns-server, and http */
  init_lwip();
  while (!netif_is_up(&netif_data));
  while (dhserv_init(&dhcp_config) != ERR_OK);
  while (dnserv_init(IP_ADDR_ANY, 53, dns_query_proc) != ERR_OK);
}
void tinyusb_network_poll(void)
{
  tud_task();
  sys_check_timeouts();
  handle_link_state_switch();
  led_blinking_task();
}

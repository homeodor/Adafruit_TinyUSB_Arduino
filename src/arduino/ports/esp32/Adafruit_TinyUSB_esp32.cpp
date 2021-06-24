/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019, hathach for Adafruit
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "tusb_option.h"

#if defined(ARDUINO_ARCH_ESP32) && TUSB_OPT_DEVICE_ENABLED

#include "sdkconfig.h"

#include "soc/soc.h"
#include "soc/efuse_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_struct.h"
#include "soc/usb_reg.h"
#include "soc/usb_wrap_reg.h"
#include "soc/usb_wrap_struct.h"
#include "soc/usb_periph.h"
#include "soc/periph_defs.h"
#include "soc/timer_group_struct.h"
#include "soc/system_reg.h"

#include "hal/usb_hal.h"

// compiler error with gpio_ll_get_drive_capability()
// invalid conversion from 'uint32_t' {aka 'unsigned int'} to 'gpio_drive_cap_t' [-fpermissive]
// #include "hal/gpio_ll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/periph_ctrl.h"

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_rom_gpio.h"

#include "esp32-hal.h"

#include "esp32s2/rom/usb/usb_persist.h"
#include "esp32s2/rom/usb/usb_dc.h"
#include "esp32s2/rom/usb/chip_usb_dw_wrapper.h"

#include "arduino/Adafruit_TinyUSB_API.h"
#include "arduino/Adafruit_USBD_Device.h"

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

// Note: persist mode is mostly copied from esp32-hal-tinyusb.c from arduino-esp32 core.

typedef enum {
    RESTART_NO_PERSIST,
    RESTART_PERSIST,
    RESTART_BOOTLOADER,
    RESTART_BOOTLOADER_DFU,
    RESTART_TYPE_MAX
} restart_type_t;

static bool usb_persist_enabled = false;
static restart_type_t usb_persist_mode = RESTART_NO_PERSIST;

static void IRAM_ATTR usb_persist_shutdown_handler(void)
{
    if(usb_persist_mode != RESTART_NO_PERSIST){
        if (usb_persist_enabled) {
            usb_dc_prepare_persist();
        }
        if (usb_persist_mode == RESTART_BOOTLOADER) {
            //USB CDC Download
            if (usb_persist_enabled) {
                chip_usb_set_persist_flags(USBDC_PERSIST_ENA);
            } else {
                periph_module_reset(PERIPH_USB_MODULE);
                periph_module_enable(PERIPH_USB_MODULE);
            }
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        } else if (usb_persist_mode == RESTART_BOOTLOADER_DFU) {
            //DFU Download
            chip_usb_set_persist_flags(USBDC_BOOT_DFU);
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        } else if (usb_persist_enabled) {
            //USB Persist reboot
            chip_usb_set_persist_flags(USBDC_PERSIST_ENA);
        }
    }
}

static void configure_pins(usb_hal_context_t *usb)
{
    for (const usb_iopin_dsc_t *iopin = usb_periph_iopins; iopin->pin != -1; ++iopin) {
        if ((usb->use_external_phy) || (iopin->ext_phy_only == 0)) {
            esp_rom_gpio_pad_select_gpio(iopin->pin);
            if (iopin->is_output) {
                esp_rom_gpio_connect_out_signal(iopin->pin, iopin->func, false, false);
            } else {
                esp_rom_gpio_connect_in_signal(iopin->pin, iopin->func, false);
                if ((iopin->pin != GPIO_FUNC_IN_LOW) && (iopin->pin != GPIO_FUNC_IN_HIGH)) {
                  // workaround for compiler error with including "hal/gpio_ll.h"
                  // gpio_ll_input_enable(&GPIO, (gpio_num_t) iopin->pin);
                  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[iopin->pin]);
                }
            }
            esp_rom_gpio_pad_unhold(iopin->pin);
        }
    }
    if (!usb->use_external_phy) {
        gpio_set_drive_capability((gpio_num_t) USBPHY_DM_NUM, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t) USBPHY_DP_NUM, GPIO_DRIVE_CAP_3);
    }
}

//--------------------------------------------------------------------+
// Porting API
//--------------------------------------------------------------------+

#define USBD_STACK_SZ (4096)

// USB Device Driver task
// This top level thread processes all usb events and invokes callbacks
static void usb_device_task(void *param) {
    (void)param;
    while(1) tud_task(); // RTOS forever loop
}

void TinyUSB_Port_InitDevice(uint8_t rhport) {
  (void)rhport;

  // from esp32-hal_tinyusb
  bool usb_did_persist = (USB_WRAP.date.val == USBDC_PERSIST_ENA);

  //if(usb_did_persist && usb_persist_enabled){
  // Enable USB/IO_MUX peripheral reset, if coming from persistent reboot
  REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_IO_MUX_RESET_DISABLE);
  REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_RESET_DISABLE);
  //} else
  if(!usb_did_persist || !usb_persist_enabled){
    // Reset USB module
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_enable(PERIPH_USB_MODULE);
  }

  esp_register_shutdown_handler(usb_persist_shutdown_handler);

  usb_hal_context_t hal = {
    .use_external_phy = false
  };
  usb_hal_init(&hal);
  configure_pins(&hal);

  tusb_init();

  // Create a task for tinyusb device stack
  xTaskCreate(usb_device_task, "usbd", USBD_STACK_SZ, NULL, configMAX_PRIORITIES - 1, NULL);
}

void TinyUSB_Port_EnterDFU(void) {
  // Reset to Bootloader
  usb_persist_mode = RESTART_BOOTLOADER;
  esp_restart();
}

uint8_t TinyUSB_Port_GetSerialNumber(uint8_t serial_id[16]) {
  uint32_t *serial_32 = (uint32_t *)serial_id;

  /* Get the MAC address */
  const uint32_t mac0 = __builtin_bswap32(REG_GET_FIELD(EFUSE_RD_MAC_SPI_SYS_0_REG, EFUSE_MAC_0));
  const uint16_t mac1 = __builtin_bswap16((uint16_t) REG_GET_FIELD(EFUSE_RD_MAC_SPI_SYS_1_REG, EFUSE_MAC_1));

  memcpy(serial_id, &mac1, 2);
  memcpy(serial_id+2, &mac0, 4);

  return 6;
}

extern "C" void yield(void) {
   TinyUSB_Device_FlushCDC();
   vPortYield();
}

#endif // USE_TINYUSB

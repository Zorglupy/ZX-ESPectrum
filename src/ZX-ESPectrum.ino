// -------------------------------------------------------------------
//  ESPectrum Emulator
//  Ramón Martínez & Jorge fuertes 2019
//  Though from ESP32 Spectrum Emulator (KOGEL esp32) Pete Todd 2017
//  You are not allowed to distribute this software commercially
//  lease, notify me, if you make any changes to this file
// -------------------------------------------------------------------

// Headers
#pragma GCC diagnostic ignored "-Wall"
#include <Arduino.h>
#include <ESP32Lib.h>
#include <FS.h>
#include <Ressources/Font6x8.h>
#include <SPIFFS.h>
#include <esp_bt.h>
#include <esp_task_wdt.h>
#pragma GCC diagnostic warning "-Wall"

#include "PS2Kbd.h"
#include "msg.h"
#include "osd.h"
#include "paledefs.h"

// EXTERN VARS
extern byte bank_latch;
extern int start_im1_irq;
extern int Z80_IPeriod;
extern boolean writeScreen;
extern boolean cfg_mode_sna;
extern boolean cfg_slog_on;
extern String cfg_ram_file;
extern String cfg_rom_file;

// EXTERN METHODS
extern void load_rom(String);
extern void load_ram(String);
void Z80_Reset(void);       /* Reset registers to the initial values */
unsigned int Z80_Execute(); /* Execute IPeriod T-States              */
unsigned int Z80();         /* Execute IPeriod T-States              */
void setup_cpuspeed();
byte Z80_RDMEM(uint16_t A);
void Z80_WRMEM(uint16_t A, byte V);
void config_read();
void do_OSD();
extern void mount_spiffs();
extern void listAllFiles();

// GLOBALS
byte *bank0;
byte z80ports_in[32];
byte borderTemp = 7;
byte soundTemp = 0;
byte flashing = 0;
byte lastAudio = 0;
boolean xULAStop = false;
boolean xULAStopped = false;

// SETUP *************************************
VGA3Bit vga;

void setup() {
    // Turn off peripherals to gain memory (?do they release properly)
    esp_bt_controller_deinit();
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    // esp_wifi_set_mode(WIFI_MODE_NULL);

    mount_spiffs();
    config_read();

    Serial.println(MSG_CHIP_SETUP);
    Serial.println(MSG_VGA_INIT);
    vga.setFrameBufferCount(2);
    vga.init(vga.MODE360x200, redPin, greenPin, bluePin, hsyncPin, vsyncPin);

    pinMode(SOUND_PIN, OUTPUT);
    digitalWrite(SOUND_PIN, LOW);

    start_im1_irq = 1;

    kb_begin();

    // ALLOCATE MEMORY
    //
    bank0 = (byte *)malloc(49152);
    if (bank0 == NULL)
        Serial.println(MSG_BANK_FAIL + "0");
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    Serial.println(MSG_FREE_HEAP_AFTER + "bank 0: " + system_get_free_heap_size() + "b");
#pragma GCC diagnostic warning "-Wall"

    setup_cpuspeed();

    // START Z80
    Serial.println(MSG_Z80_RESET);
    Z80_Reset();

    // make sure keyboard ports are FF
    for (int t = 0; t < 32; t++) {
        z80ports_in[t] = 0xff;
    }

    Serial.println(MSG_EXEC_ON_CORE + xPortGetCoreID());
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    Serial.println(MSG_FREE_HEAP_AFTER + "Z80 reset: " + system_get_free_heap_size());
#pragma GCC diagnostic warning "-Wall"

    xTaskCreatePinnedToCore(videoTask,   /* Function to implement the task */
                            "videoTask", /* Name of the task */
                            2048,        /* Stack size in words */
                            NULL,        /* Task input parameter */
                            20,          /* Priority of the task */
                            NULL,        /* Task handle. */
                            0);          /* Core where the task should run */

    load_rom(cfg_rom_file);
    if (cfg_mode_sna)
        load_ram(cfg_ram_file);
}

// VIDEO core 0 *************************************

void videoTask(void *parameter) {
    unsigned int ff, i, byte_offset;
    unsigned char color_attrib, pixel_map, zx_fore_color, zx_back_color, flash, bright;
    unsigned int zx_vidcalc;
    unsigned int tmpColour;

    while (1) {
        while (xULAStop) {
            xULAStopped = true;
            delay(5);
        }
        xULAStopped = false;
        if (flashing++ > 32)
            flashing = 0;

        vga.clear(zxcolor(borderTemp, 0));
        for (unsigned int lin = 0; lin < 192; lin++) {
            for (ff = 0; ff < 32; ff++) // foreach byte in line
            {
                byte_offset = lin * 32 + ff; //*2+1;
                // spectrum attributes
                // bit 7 6   5 4 3   2 1 0
                //    F B   P A P   I N K
                color_attrib = bank0[0x1800 + (calcY(byte_offset) / 8) * 32 + ff]; // get 1 of 768 attrib values
                pixel_map = bank0[byte_offset];
                zx_fore_color = color_attrib & 0x07;
                zx_back_color = (color_attrib & 0x38) >> 3;

                flash = bitRead(color_attrib, 7);
                bright = bitRead(color_attrib, 6);

                if (flash && (flashing > 16)) {
                    tmpColour = zx_fore_color;
                    zx_fore_color = zx_back_color;
                    zx_back_color = tmpColour;
                }

                for (i = 0; i < 8; i++) // foreach pixel within a byte
                {
                    zx_vidcalc = ff * 8 + i;
                    byte bitpos = (0x80 >> i);

                    if ((pixel_map & bitpos) != 0)
                        vga.dotFast(zx_vidcalc + 52, calcY(byte_offset) + 5, zxcolor(zx_fore_color, bright));
                    else
                        vga.dotFast(zx_vidcalc + 52, calcY(byte_offset) + 5, zxcolor(zx_back_color, bright));
                }
            }
        }

        vga.show();

        TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
        TIMERG0.wdt_feed = 1;
        TIMERG0.wdt_wprotect = 0;
        // vTaskDelay(0);
    }
}

// SPECTRUM SCREEN DISPLAY
//
/* Calculate Y coordinate (0-192) from Spectrum screen memory location */
int calcY(int offset) {
    return ((offset >> 11) << 6)                                            // sector start
           + ((offset % 2048) >> 8)                                         // pixel rows
           + ((((offset % 2048) >> 5) - ((offset % 2048) >> 8 << 3)) << 3); // character rows
}

/* Calculate X coordinate (0-255) from Spectrum screen memory location */
int calcX(int offset) { return (offset % 32) << 3; }

unsigned int zxcolor(byte c, byte f) {
    int rgbLevel = 192;
    if (f)
        rgbLevel = 255;
    else
        rgbLevel = 192;

    if (c == 0)
        return vga.RGB(0, 0, 0);
    else if (c == 1)
        return vga.RGB(0, 0, rgbLevel);
    else if (c == 2)
        return vga.RGB(rgbLevel, 0, 0);
    else if (c == 3)
        return vga.RGB(rgbLevel, 0, rgbLevel);
    else if (c == 4)
        return vga.RGB(0, rgbLevel, 0);
    else if (c == 5)
        return vga.RGB(0, rgbLevel, rgbLevel);
    else if (c == 6)
        return vga.RGB(rgbLevel, rgbLevel, 0);
    else if (c == 7)
        return vga.RGB(rgbLevel, rgbLevel, rgbLevel);
    return 0;
}

/* Load zx keyboard lines from PS/2 */
void do_keyboard() {
    if (keymap != oldKeymap) {
        bitWrite(z80ports_in[0], 0, keymap[0x12]);
        bitWrite(z80ports_in[0], 1, keymap[0x1a]);
        bitWrite(z80ports_in[0], 2, keymap[0x22]);
        bitWrite(z80ports_in[0], 3, keymap[0x21]);
        bitWrite(z80ports_in[0], 4, keymap[0x2a]);

        bitWrite(z80ports_in[1], 0, keymap[0x1c]);
        bitWrite(z80ports_in[1], 1, keymap[0x1b]);
        bitWrite(z80ports_in[1], 2, keymap[0x23]);
        bitWrite(z80ports_in[1], 3, keymap[0x2b]);
        bitWrite(z80ports_in[1], 4, keymap[0x34]);

        bitWrite(z80ports_in[2], 0, keymap[0x15]);
        bitWrite(z80ports_in[2], 1, keymap[0x1d]);
        bitWrite(z80ports_in[2], 2, keymap[0x24]);
        bitWrite(z80ports_in[2], 3, keymap[0x2d]);
        bitWrite(z80ports_in[2], 4, keymap[0x2c]);

        bitWrite(z80ports_in[3], 0, keymap[0x16]);
        bitWrite(z80ports_in[3], 1, keymap[0x1e]);
        bitWrite(z80ports_in[3], 2, keymap[0x26]);
        bitWrite(z80ports_in[3], 3, keymap[0x25]);
        bitWrite(z80ports_in[3], 4, keymap[0x2e]);

        bitWrite(z80ports_in[4], 0, keymap[0x45]);
        bitWrite(z80ports_in[4], 1, keymap[0x46]);
        bitWrite(z80ports_in[4], 2, keymap[0x3e]);
        bitWrite(z80ports_in[4], 3, keymap[0x3d]);
        bitWrite(z80ports_in[4], 4, keymap[0x36]);

        bitWrite(z80ports_in[5], 0, keymap[0x4d]);
        bitWrite(z80ports_in[5], 1, keymap[0x44]);
        bitWrite(z80ports_in[5], 2, keymap[0x43]);
        bitWrite(z80ports_in[5], 3, keymap[0x3c]);
        bitWrite(z80ports_in[5], 4, keymap[0x35]);

        bitWrite(z80ports_in[6], 0, keymap[0x5a]);
        bitWrite(z80ports_in[6], 1, keymap[0x4b]);
        bitWrite(z80ports_in[6], 2, keymap[0x42]);
        bitWrite(z80ports_in[6], 3, keymap[0x3b]);
        bitWrite(z80ports_in[6], 4, keymap[0x33]);

        bitWrite(z80ports_in[7], 0, keymap[0x29]);
        bitWrite(z80ports_in[7], 1, keymap[0x14]);
        bitWrite(z80ports_in[7], 2, keymap[0x3a]);
        bitWrite(z80ports_in[7], 3, keymap[0x31]);
        bitWrite(z80ports_in[7], 4, keymap[0x32]);
    }
    strcpy(oldKeymap, keymap);
}

/* +-------------+
   | LOOP core 1 |
   +-------------+
*/
void loop() {
    while (1) {
        start_im1_irq = 1;
        do_keyboard();
        do_OSD();
        Z80_Execute();
        TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
        TIMERG0.wdt_feed = 1;
        TIMERG0.wdt_wprotect = 0;
        vTaskDelay(0); // important to avoid task watchdog timeouts - change this to slow down emu
    }
}

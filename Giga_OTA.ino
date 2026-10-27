/*
  Giga_OTA - a minimal over-the-air firmware update on an Arduino GIGA R1 WiFi,
  with an LVGL screen so the result is visible rather than inferred.

  The whole demonstration is the version label. Flash this once over USB, then
  push a new build over the air and watch the number on the screen change. No
  counters, no log scraping, no "it probably worked".

  WHAT THIS SHOWS
    - the QSPI partition layout the bootloader expects, read back at boot
    - turning off the WHD WiFi powersave the core enables behind your back
    - Arduino_Portenta_OTA driven from a button rather than from setup()
    - the one thing that actually proves an update applied: a changed version
    - two serial dev commands that make the whole cycle scriptable from a PC:
      'D' dumps the framebuffer (tools/dump_screen.py turns it into a PNG) and
      'P' injects a tap (tools/sim_tap.py presses the button). Between them
      you can run an update and read the result back without touching the
      board or photographing the screen.

  BEFORE THE FIRST RUN
    1. Copy arduino_secrets.h.example to arduino_secrets.h and fill it in.
    2. If the partition report below says the QSPI is not partitioned, run
       File > Examples > STM32H747_System > QSPIFormat once. This ERASES the
       QSPI, including stored WiFi credentials and any user data on it.
    3. Serve an update image over HTTP - see tools/serve.py.

  See README.md for how the pieces fit and for why some of them are odd.
*/

#include "Arduino_H7_Video.h"
#include "Arduino_GigaDisplayTouch.h"
#include "SDRAM.h"
#include "lvgl.h"

#include <WiFi.h>
#include <Arduino_Portenta_OTA.h>
#include <BlockDevice.h>

// Not pulled in by WiFi.h, but part of the same SocketWrapper library that is
// already linked. Needed to drive the HTTP GET ourselves instead of handing
// it to the library's blocking download() - see downloadWithProgress().
#include <utility/http_request.h>

#include "arduino_secrets.h"

// Bump this and push the new build over the air. Watching it change on the
// screen is the entire acceptance test.
#define FW_VERSION "1.0.0"

static char const SSID[]     = SECRET_SSID;
static char const PASS[]     = SECRET_PASS;
static char const OTA_URL[]  = SECRET_OTA_URL;

// Plain HTTP. HTTPS works too, but it needs a valid cacert.pem in QSPI
// partition 1, which is one more thing that can be wrong on a first attempt -
// so prove the mechanism over HTTP and add TLS afterwards.
static bool const OTA_IS_HTTPS = false;

// Is the payload a plain .bin, or an LZSS-compressed .ota container?
//
// RAW IS THE DEFAULT, AND IT IS THE WHOLE POINT. The documented way to build
// an image needs Arduino's lzss.py, which is a ctypes shim over a .so that
// does not exist on Windows. You can skip all of it: serve the .bin that
// arduino-cli already produced and do not decompress.
//
// download() always writes to /fs/UPDATE.BIN.LZSS regardless of what the
// payload actually is; decompress() is what produces /fs/UPDATE.BIN. In raw
// mode we copy instead of decompressing, and the bootloader cannot tell the
// difference - it only cares that /fs/UPDATE.BIN is a valid image.
//
// Set false to use a real .ota built with lzss.py + bin2ota.py.
static bool const OTA_PAYLOAD_IS_RAW = true;

Arduino_H7_Video       Display(800, 480, GigaDisplayShield);
Arduino_GigaDisplayTouch TouchDetector;

static lv_obj_t *lbl_version;
static lv_obj_t *lbl_status;
static lv_obj_t *bar_progress;
static lv_obj_t *lbl_ip;
static lv_obj_t *lbl_qspi;
static lv_obj_t *btn_update;

// The QSPI report is produced before the display exists, and a USB board
// re-enumerates after a DFU upload, so the serial boot banner is easy to miss
// entirely. Keeping the summary here lets the screen show it once there is a
// screen - which is the point of putting a display on an OTA demo.
static char qspi_summary[96] = "QSPI: not checked";

// Set by the button callback, acted on by loop().
//
// NOT done inside the callback: the callback runs from inside
// lv_timer_handler(), and the OTA path blocks for tens of seconds and then
// reboots the board. Anything long or re-entrant started from an LVGL event
// is a hang waiting to happen.
static volatile bool ota_requested = false;

/* -------------------------------------------------------------------------
   WiFi power save

   whd_wifi_on() in the Giga core enables PM2 powersave unconditionally on
   every radio power-up, with a 200 ms return-to-sleep delay. From the core's
   own libmbed.a (whd_management.o):

       bl    whd_wifi_set_up
       movs  r1, #200                                 ; return_to_sleep_delay
       bl    whd_wifi_enable_powersave_with_throughput

   After that the radio wakes only on DTIM beacons. Outbound traffic is
   unaffected, so a board that publishes happily can still be unreachable, and
   a large inbound download can stall. mbed_giga's WiFi class exposes no
   setting for it - there is no WiFi.setSleep() the way there is on the ESP32
   core - but WiFi.h already drags in whd_wifi_api.h, so the WHD calls link
   straight from a sketch. Only the BSP driver accessor needs declaring.

   Call it AFTER the join: whd_wifi_on() re-arms PM2 every time the radio is
   powered up, so anything that cycles the radio undoes this.
   ------------------------------------------------------------------------- */
extern "C" {
  whd_driver_t cybsp_get_wifi_driver(void);
}

// 0 = NO_POWERSAVE_MODE, 1 = PM1, 2 = PM2; 99 = never read.
static uint32_t wifi_ps_mode = 99;

static void disableWifiPowerSave(void) {
  whd_driver_t drv = cybsp_get_wifi_driver();
  if (drv == NULL) { Serial.println("WiFi PS: no WHD driver, left alone"); return; }
  whd_interface_t ifp = whd_get_primary_interface(drv);
  if (ifp == NULL) { Serial.println("WiFi PS: no WHD interface, left alone"); return; }

  uint32_t before = 99, after = 99;
  whd_wifi_get_powersave_mode(ifp, &before);
  const uint32_t rc = whd_wifi_disable_powersave(ifp);
  whd_wifi_get_powersave_mode(ifp, &after);
  wifi_ps_mode = after;

  // Reading the mode back rather than assuming the call worked. A fix you
  // cannot observe is a fix you are only hoping for.
  Serial.print("WiFi PS: mode ");
  Serial.print(before);
  Serial.print(" -> ");
  Serial.print(after);
  Serial.print(" (rc ");
  Serial.print(rc);
  Serial.println(after == 0 ? ") - powersave OFF" : ") - STILL ON");
}

/* -------------------------------------------------------------------------
   QSPI partition report

   The bootloader looks for the update at /fs/UPDATE.BIN, which lives on a FAT
   filesystem on MBR partition 2 of the 16 MB QSPI chip. If the QSPI has never
   been partitioned, OTA cannot work and the failure is opaque, so the board
   says what it has at boot instead of leaving you to guess.

   Expected layout, created by the core's QSPIFormat example:
     1: WiFi firmware and certificates   1 MB
     2: OTA                              5 MB   <- mounted as "fs"
     3: Provisioning KVStore             1 MB
     4: User data                        7 MB
   ------------------------------------------------------------------------- */
struct __attribute__((packed)) mbrEntry {
  uint8_t  status;
  uint8_t  chsStart[3];
  uint8_t  type;
  uint8_t  chsStop[3];
  uint32_t lbaOffset;
  uint32_t lbaSize;
};

struct __attribute__((packed)) mbrTable {
  mbrEntry entries[4];
  uint8_t  signature[2];
};

static int reportQspiPartitions(void) {
  auto bd = mbed::BlockDevice::get_default_instance();
  if (bd->init() != 0) {
    Serial.println("QSPI: cannot init the block device");
    return -1;
  }

  auto buffer_size = std::max<uint32_t>(bd->get_program_size(), sizeof(mbrTable));
  if (buffer_size % bd->get_program_size() != 0) {
    buffer_size += bd->get_program_size() - (buffer_size % bd->get_program_size());
  }

  auto buffer = new uint8_t[buffer_size];
  if (bd->read(buffer, 512 - buffer_size, buffer_size) != 0) {
    Serial.println("QSPI: cannot read the Master Boot Record");
    delete[] buffer;
    return -1;
  }

  auto table = reinterpret_cast<mbrTable *>(&buffer[buffer_size - sizeof(mbrTable)]);
  int found = 0;

  // THE LBA FIELDS ARE NOT IN 512-BYTE SECTORS. MBRBlockDevice takes byte
  // addresses (bd_addr_t) and stores them divided by max(erase_size, 512),
  // which on this QSPI is 4096 - so assuming the usual 512 under-reports
  // every partition by 8x. Worth getting right rather than eyeballing,
  // because "is the OTA partition big enough for my image" is the one
  // question this report exists to answer.
  const uint32_t sector = (bd->get_erase_size() > 512)
                            ? (uint32_t)bd->get_erase_size() : 512u;

  Serial.print("QSPI: chip is ");
  Serial.print((unsigned long)(bd->size() / (1024 * 1024)));
  Serial.println(" MB");

  if (table->signature[0] != 0x55 || table->signature[1] != 0xAA) {
    Serial.println("QSPI: NO MBR SIGNATURE - the flash is not partitioned.");
    Serial.println("QSPI: run the QSPIFormat example before attempting an OTA.");
    snprintf(qspi_summary, sizeof(qspi_summary),
             "QSPI: NOT PARTITIONED - run QSPIFormat before any OTA");
    delete[] buffer;
    return 0;
  }

  for (int i = 0; i < 4; i++) {
    const mbrEntry &e = table->entries[i];
    if (e.lbaSize == 0) continue;
    found++;
    Serial.print("QSPI: partition ");
    Serial.print(i + 1);
    Serial.print("  type 0x");
    Serial.print(e.type, HEX);
    Serial.print("  offset ");
    Serial.print((unsigned long)e.lbaOffset * sector / 1024UL);
    Serial.print(" kB  size ");
    Serial.print((unsigned long)e.lbaSize * sector / 1024UL);
    Serial.println(" kB");
  }

  if (found < 2) {
    Serial.println("QSPI: partition 2 is where /fs/UPDATE.BIN has to live.");
    Serial.println("QSPI: run the QSPIFormat example before attempting an OTA.");
    snprintf(qspi_summary, sizeof(qspi_summary),
             "QSPI: %d partition(s) - need at least 2, run QSPIFormat", found);
  } else {
    // Size the OTA partition against this very sketch, since "big enough"
    // is the only thing the number is for.
    const unsigned long ota_kb =
        (unsigned long)table->entries[1].lbaSize * sector / 1024UL;
    snprintf(qspi_summary, sizeof(qspi_summary),
             "QSPI: %d partitions, OTA partition 2 is %lu kB", found, ota_kb);
    // The sector size only matters when the number looks wrong, so it goes
    // to the console rather than taking up screen width.
    Serial.print("QSPI: LBA unit is ");
    Serial.print((unsigned long)sector);
    Serial.println(" bytes");
  }

  delete[] buffer;
  return found;
}

/* -------------------------------------------------------------------------
   Dev tools: screen exporter and simulated touch

   These exist because "did the OTA work?" should be answerable from the PC.
   'D' dumps the framebuffer so tools/dump_screen.py can write a real PNG of
   the version label; 'P' injects a tap so tools/sim_tap.py can press Update.
   Together they make the whole update cycle scriptable with no hands on the
   board and no camera - which is the point of an over-the-air mechanism.

   Implemented in Arduino_H7_Video's own dsi.cpp and already linked in, but
   dsi.h is in the library's src/ folder rather than its public include path,
   so they are redeclared here with matching signatures.
   ------------------------------------------------------------------------- */
uint32_t dsi_getActiveFrameBuffer(void);
uint32_t dsi_getDisplayXSize(void);
uint32_t dsi_getDisplayYSize(void);

// The framebuffer is in the PHYSICAL panel's orientation. The Giga Display
// Shield's panel is portrait; declaring Display(800, 480) makes
// Arduino_H7_Video create the LVGL display at the panel's native 480x800 and
// apply ROTATION_270. dsi_getDisplayXSize()/YSize() report that native size,
// not the logical 800x480 - dump_screen.py un-rotates it on the PC side.
static void dumpFramebufferToSerial(void) {
  const uint32_t  w  = dsi_getDisplayXSize();
  const uint32_t  h  = dsi_getDisplayYSize();
  const uint16_t *fb = (const uint16_t *)dsi_getActiveFrameBuffer();

  // DMA2D writes this memory, so a stale D-cache line could otherwise be
  // served back instead of what is actually on screen right now.
  SCB_CleanInvalidateDCache();

  Serial.println();
  Serial.println("FBDUMP");
  Serial.print(w);
  Serial.print('x');
  Serial.println(h);
  Serial.println("RGB565");
  Serial.write((const uint8_t *)fb, w * h * 2);
  Serial.println();
  Serial.println("FBEND");
}

static volatile bool    g_simTouchDown = false;
static volatile int16_t g_simTouchX = 0;
static volatile int16_t g_simTouchY = 0;
static unsigned long    g_simTouchReleaseAt = 0;

// A SECOND pointer indev alongside the real touchscreen. Both feed the same
// screen's input processing, so a simulated tap goes through real LVGL
// hit-testing and click bubbling rather than calling the callback directly.
//
// The hand-written rotation here is the subtle part. LVGL expects an indev's
// raw point in the display's UN-rotated base resolution and applies
// lv_display_rotate_point() to every read. The real touch controller already
// reports panel-space coordinates, so it works for free; feeding already-
// logical coordinates makes LVGL rotate them a second time and the tap lands
// nowhere. Inverting ROTATION_270 by hand (raw_x = 479 - logical_y,
// raw_y = logical_x) lets the serial protocol and the PC script deal only in
// the logical 800x480 space that everything else in this sketch uses.
static void simTouchReadCb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  data->point.x = 479 - g_simTouchY;
  data->point.y = g_simTouchX;
  data->state   = g_simTouchDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// 'D' dumps the screen; 'P' + 6 ASCII digits (3-digit x, 3-digit y,
// zero-padded, e.g. "P400300") taps at that logical point.
//
// Called from loop() AND from the connect retry loop in setup(), because the
// slow paths are exactly the ones worth capturing.
static void serviceDevCommands(void) {
  if (Serial.available()) {
    switch (Serial.read()) {
      case 'D': dumpFramebufferToSerial(); break;
      case 'P': {
        char c[6];
        if (Serial.readBytes(c, 6) == 6) {
          char xb[4] = { c[0], c[1], c[2], '\0' };
          char yb[4] = { c[3], c[4], c[5], '\0' };
          g_simTouchX = atoi(xb);
          g_simTouchY = atoi(yb);
          g_simTouchDown = true;
          g_simTouchReleaseAt = millis() + 80;
        }
        break;
      }
      default: break;
    }
  }

  // Release ~80 ms after the press, checked every call so the cycle stays
  // short enough for LVGL to read it as a click rather than a long-press.
  if (g_simTouchDown && millis() >= g_simTouchReleaseAt) g_simTouchDown = false;
}

/* -------------------------------------------------------------------------
   UI
   ------------------------------------------------------------------------- */
static void setStatus(const char *s) {
  lv_label_set_text(lbl_status, s);
  // Some stages block, so without forcing a paint here the status line only
  // ever shows the LAST message - the user sees nothing until it is over.
  lv_refr_now(NULL);
}

// Same, without the forced repaint: for the inner loops that are already
// calling lv_timer_handler() every pass. Forcing a full refresh per chunk of
// a 700 KB download makes the transfer measurably slower than the network.
static void setStatusQuiet(const char *s) {
  lv_label_set_text(lbl_status, s);
}

static void setProgress(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  lv_bar_set_value(bar_progress, pct, LV_ANIM_OFF);
}

static void showProgress(bool on) {
  if (on) lv_obj_clear_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
  else    lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
  setProgress(0);
}

static void onUpdateClicked(lv_event_t *e) {
  (void)e;
  ota_requested = true;
  lv_obj_add_state(btn_update, LV_STATE_DISABLED);
  setStatus("Starting update...");
}

static void buildUi(void) {
  lv_obj_t *scr = lv_scr_act();

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "GIGA R1 over-the-air update");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lbl_version = lv_label_create(scr);
  lv_label_set_text(lbl_version, "FW " FW_VERSION);
  lv_obj_align(lbl_version, LV_ALIGN_CENTER, 0, -60);

  lbl_ip = lv_label_create(scr);
  lv_label_set_text(lbl_ip, "WiFi: connecting");
  lv_obj_align(lbl_ip, LV_ALIGN_CENTER, 0, -10);

  lbl_qspi = lv_label_create(scr);
  lv_obj_set_width(lbl_qspi, 760);
  lv_label_set_long_mode(lbl_qspi, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(lbl_qspi, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lbl_qspi, qspi_summary);
  lv_obj_align(lbl_qspi, LV_ALIGN_CENTER, 0, 20);

  btn_update = lv_btn_create(scr);
  lv_obj_set_size(btn_update, 240, 70);
  lv_obj_align(btn_update, LV_ALIGN_CENTER, 0, 90);
  lv_obj_add_event_cb(btn_update, onUpdateClicked, LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_lbl = lv_label_create(btn_update);
  lv_label_set_text(btn_lbl, "Update");
  lv_obj_center(btn_lbl);

  // Hidden until an update starts, so the idle screen stays uncluttered.
  bar_progress = lv_bar_create(scr);
  lv_obj_set_size(bar_progress, 560, 24);
  lv_obj_align(bar_progress, LV_ALIGN_BOTTOM_MID, 0, -70);
  lv_bar_set_range(bar_progress, 0, 100);
  lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
  lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);

  lbl_status = lv_label_create(scr);
  lv_label_set_text(lbl_status, "Idle");
  lv_obj_set_width(lbl_status, 760);
  lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* -------------------------------------------------------------------------
   The OTA itself

   Deliberately the library's documented sequence and nothing clever:
     isOtaCapable -> begin -> download -> decompress -> update -> reset

   There is no progress bar. download() is a blocking call with no progress
   callback, so a bar would either not move or would be a lie. The status line
   says what stage it is in and reports the byte count afterwards, which is
   what the API can actually support.
   ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
   Downloading, with the UI still alive

   ota.download() is a blocking call with no progress reporting: it issues the
   GET, then spins on `while (!rsp->is_message_complete()) delay(10);`. For a
   700 KB image over WiFi that is the better part of a minute during which the
   screen is frozen and the board looks dead. On an update mechanism that is
   not a cosmetic problem - a user who thinks it has hung will reach for the
   power, which is the one thing that must not happen mid-write.

   So we run the same request ourselves. send() returns once the headers are
   parsed and the body streams in through the callback, which means we own the
   wait loop: we can pump LVGL, count bytes, and read Content-Length for a
   real denominator.

   Two things that fall out of doing it this way, both of which matter more
   than the progress bar:

     - A STALL TIMEOUT. If the server goes away mid-transfer,
       is_message_complete() never becomes true and the library's loop spins
       for ever. Here, no bytes for STALL_MS and we give up.
     - A COMPLETENESS CHECK. Knowing the expected length is what lets us tell
       a finished download from a truncated one, which is what stops a
       half-image ever reaching /fs/UPDATE.BIN.
   ------------------------------------------------------------------------- */
#define DL_STALL_MS 15000UL

static FILE             *dl_file;
static volatile uint32_t dl_received;

static void dlBodyCb(const char *data, uint32_t len) {
  if (dl_file && len) fwrite(data, 1, len, dl_file);
  dl_received += len;
}

static uint32_t contentLengthOf(HttpResponse *rsp) {
  std::vector<std::string *> names = rsp->get_headers_fields();
  std::vector<std::string *> values = rsp->get_headers_values();
  for (size_t i = 0; i < names.size() && i < values.size(); i++) {
    if (strcasecmp(names[i]->c_str(), "Content-Length") == 0) {
      return (uint32_t)strtoul(values[i]->c_str(), NULL, 10);
    }
  }
  return 0;   // chunked, or a server that did not say
}

// Returns bytes written, or a negative value on failure.
// expectedOut receives Content-Length, or 0 if the server did not give one.
static int downloadWithProgress(const char *url, const char *target,
                                uint32_t *expectedOut) {
  *expectedOut = 0;
  dl_received = 0;

  dl_file = fopen(target, "wb");
  if (!dl_file) return -1;

  HttpRequest req(WiFi.getNetwork(), HTTP_GET, url, mbed::callback(dlBodyCb));
  HttpResponse *rsp = req.send(NULL, 0);
  if (rsp == NULL) {
    fclose(dl_file); dl_file = NULL;
    return -(req.get_error() ? req.get_error() : 1);
  }
  if (rsp->get_status_code() != 200) {
    const int code = rsp->get_status_code();
    fclose(dl_file); dl_file = NULL;
    return -code;
  }

  const uint32_t expected = contentLengthOf(rsp);
  *expectedOut = expected;

  uint32_t lastSeen = 0;
  unsigned long lastChange = millis();

  while (!rsp->is_message_complete()) {
    const uint32_t now = dl_received;
    if (now != lastSeen) { lastSeen = now; lastChange = millis(); }
    else if (millis() - lastChange > DL_STALL_MS) {
      fclose(dl_file); dl_file = NULL;
      return -9001;      // stalled; the caller reports it as such
    }

    if (expected) {
      setProgress((int)((uint64_t)now * 100 / expected));
      char msg[96];
      snprintf(msg, sizeof(msg), "Downloading %lu of %lu kB...",
               (unsigned long)(now / 1024), (unsigned long)(expected / 1024));
      setStatusQuiet(msg);
    } else {
      char msg[80];
      snprintf(msg, sizeof(msg), "Downloading %lu kB (size unknown)...",
               (unsigned long)(now / 1024));
      setStatusQuiet(msg);
    }

    lv_timer_handler();
    serviceDevCommands();
    delay(20);
  }

  fclose(dl_file);
  dl_file = NULL;
  return (int)dl_received;
}

// Raw mode's replacement for decompress(): copy what the download fetched
// into the filename the bootloader looks for. Returns bytes copied, or -1.
//
// 8 KB at a time because this runs after ota.begin() has mounted the OTA
// filesystem and there is plenty of heap; a byte-at-a-time copy of a 700 KB
// image through FAT is painfully slow.
//
// Reports coarse progress - start, halfway, done - because this is the other
// place the board sits silent for several seconds, and "staging" with a
// moving bar reads very differently from a frozen screen.
static int copyRawToUpdateBin(uint32_t expected) {
  FILE *in = fopen("/fs/UPDATE.BIN.LZSS", "rb");
  if (!in) return -1;
  FILE *out = fopen("/fs/UPDATE.BIN", "wb");
  if (!out) { fclose(in); return -1; }

  const size_t BUF = 8192;
  uint8_t *buf = (uint8_t *)malloc(BUF);
  if (!buf) { fclose(in); fclose(out); return -1; }

  int total = 0;
  size_t n;
  while ((n = fread(buf, 1, BUF, in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { total = -1; break; }
    total += (int)n;

    if (expected) setProgress((int)((uint64_t)total * 100 / expected));
    char msg[64];
    snprintf(msg, sizeof(msg), "Staging %lu kB...", (unsigned long)(total / 1024));
    setStatusQuiet(msg);
    lv_timer_handler();
  }

  free(buf);
  fclose(in);
  fclose(out);
  return total;
}

static void runOta(void) {
  char msg[160];

  Arduino_Portenta_OTA_QSPI ota(QSPI_FLASH_FATFS_MBR, 2);
  Arduino_Portenta_OTA::Error err = Arduino_Portenta_OTA::Error::None;

  if (!ota.isOtaCapable()) {
    // By far the most likely first-run failure, and the default message for
    // it is unhelpful, so name the actual remedy.
    setStatus("Bootloader too old for OTA. Update it: Examples > "
              "STM32H747_System > STM32H747_manageBootloader");
    Serial.println("OTA: isOtaCapable() == false");
    return;
  }

  setStatus("Initialising OTA storage...");
  if ((err = ota.begin()) != Arduino_Portenta_OTA::Error::None) {
    snprintf(msg, sizeof(msg), "ota.begin() failed, error %d. Is the QSPI "
                               "partitioned? See the boot log.", (int)err);
    setStatus(msg);
    return;
  }

  snprintf(msg, sizeof(msg), "Contacting %s ...", OTA_URL);
  setStatus(msg);
  Serial.print("OTA: downloading ");
  Serial.println(OTA_URL);
  showProgress(true);

  // HTTPS still goes through the library's own call: HttpsRequest needs a
  // cert store set up, and the streaming path below is the HTTP one.
  uint32_t expected = 0;
  int got;
  if (OTA_IS_HTTPS) {
    got = ota.download(OTA_URL, true);
  } else {
    got = downloadWithProgress(OTA_URL, "/fs/UPDATE.BIN.LZSS", &expected);
  }

  if (got == -9001) {
    showProgress(false);
    snprintf(msg, sizeof(msg), "Download stalled after %lu kB - the server "
                               "stopped sending. Nothing was changed.",
             (unsigned long)(dl_received / 1024));
    setStatus(msg);
    Serial.println("OTA: download stalled; UPDATE.BIN left untouched");
    return;
  }
  if (got <= 0) {
    showProgress(false);
    snprintf(msg, sizeof(msg), "Download failed, error %d. Is the server "
                               "running and reachable?", got);
    setStatus(msg);
    return;
  }

  // THE CHECK THAT MAKES A FAILED UPDATE SURVIVABLE. A truncated image that
  // reaches /fs/UPDATE.BIN is a brick on the next boot, and a download can be
  // cut short by the server, the network or the board losing its association
  // - none of which are rare. Refusing to stage anything short of
  // Content-Length means an interrupted update costs nothing: the running
  // firmware is untouched and /fs/UPDATE.BIN still holds whatever it held.
  if (expected && (uint32_t)got != expected) {
    showProgress(false);
    snprintf(msg, sizeof(msg), "Truncated: got %d bytes of %lu. Refusing to "
                               "stage it. Nothing was changed.",
             got, (unsigned long)expected);
    setStatus(msg);
    Serial.print("OTA: truncated (");
    Serial.print(got);
    Serial.print(" of ");
    Serial.print((unsigned long)expected);
    Serial.println("); UPDATE.BIN left untouched");
    return;
  }

  snprintf(msg, sizeof(msg), "Downloaded %d bytes. %s...", got,
           OTA_PAYLOAD_IS_RAW ? "Staging" : "Decompressing");
  setStatus(msg);
  setProgress(0);

  int out;
  if (OTA_PAYLOAD_IS_RAW) {
    out = copyRawToUpdateBin(expected ? expected : (uint32_t)got);
    if (out < 0) {
      showProgress(false);
      setStatus("Could not stage the image to /fs/UPDATE.BIN.");
      return;
    }
  } else {
    out = ota.decompress();
    if (out < 0) {
      showProgress(false);
      snprintf(msg, sizeof(msg), "decompress() failed, error %d. Was the image "
                                 "built with lzss + bin2ota?", out);
      setStatus(msg);
      return;
    }
  }

  setProgress(100);
  snprintf(msg, sizeof(msg), "%s %d bytes. Arming bootloader...",
           OTA_PAYLOAD_IS_RAW ? "Staged" : "Decompressed", out);
  setStatus(msg);

  if ((err = ota.update()) != Arduino_Portenta_OTA::Error::None) {
    showProgress(false);
    snprintf(msg, sizeof(msg), "ota.update() failed, error %d", (int)err);
    setStatus(msg);
    return;
  }

  // update() only RECORDS the update. The bootloader applies it on the next
  // boot, and the only statement that it really happened is the bootloader's
  // own "I-Sketch updated" followed by a changed version on screen.
  setStatus("Complete. Rebooting to apply the update...");
  Serial.println("OTA: armed, resetting");

  // Hold the finished state on screen for a moment. Without this the last
  // thing anyone sees is a half-drawn bar before the display goes, which
  // reads like a crash rather than a success.
  for (unsigned long t = millis(); millis() - t < 1500; ) {
    lv_timer_handler();
    delay(10);
  }
  ota.reset();
}

/* -------------------------------------------------------------------------
   Joining, and re-joining

   A FAILED JOIN MUST NOT BE TERMINAL. The obvious shape - try a few times in
   setup(), give up, carry on - leaves a board that is permanently offline
   after one bad moment at power-up, with no way back except a reset. On a
   board whose entire purpose is to be updated remotely, that is the one
   failure you cannot afford, because it is also the failure that takes away
   your means of fixing it.
   ------------------------------------------------------------------------- */
#define WIFI_ATTEMPTS      8
#define WIFI_ATTEMPT_MS    10000UL
#define WIFI_RETRY_MS      30000UL

static unsigned long wifi_lastTry = 0;

// Waits while keeping LVGL and the dev commands alive, so the screen stays
// readable and capturable through a slow join.
static void busyWait(unsigned long ms) {
  for (unsigned long t = millis(); millis() - t < ms; ) {
    lv_timer_handler();
    serviceDevCommands();
    delay(5);
  }
}

// Waits up to ms for the association, returning THE MOMENT it is up rather
// than sitting out the full timeout. The earlier version slept the whole
// 10 s whether or not the board had already joined, which made every attempt
// cost the maximum and made a fast join indistinguishable from a slow one.
static bool waitForConnect(unsigned long ms) {
  for (unsigned long t = millis(); millis() - t < ms; ) {
    if (WiFi.status() == WL_CONNECTED) return true;
    lv_timer_handler();
    serviceDevCommands();
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool tryJoinWiFi(void) {
  wifi_lastTry = millis();

  if (WiFi.status() == WL_CONNECTED) return true;

  int used = 0;
  for (int attempt = 1; attempt <= WIFI_ATTEMPTS; attempt++) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to WiFi (attempt %d/%d)...",
             attempt, WIFI_ATTEMPTS);
    setStatus(msg);

    Serial.print("[wifi] attempt ");
    Serial.print(attempt);
    Serial.print(": ");

    const int rc = WiFi.begin(SSID, PASS);

    // BREAK ON SUCCESS, AND DO NOT RE-READ. The previous version took
    // begin()'s result, then unconditionally overwrote it with
    // WiFi.status() before testing the loop condition - so a status that
    // had not caught up yet sent it round again and called begin() a second
    // time, tearing down an association that had just come up. A join that
    // works can be undone by the code meant to check it.
    if (rc == WL_CONNECTED || waitForConnect(WIFI_ATTEMPT_MS)) {
      used = attempt;
      Serial.println("connected");
      break;
    }

    Serial.print("no (status ");
    Serial.print(WiFi.status());
    Serial.println(")");
  }

  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(lbl_ip, "WiFi: not connected");
    setStatus("WiFi join failed. Retrying in the background - check the "
              "antenna and arduino_secrets.h.");
    return false;
  }

  // After the join, every time: whd_wifi_on() re-arms PM2 on every radio
  // power-up, so a reconnect silently undoes a one-shot disable.
  disableWifiPowerSave();

  char ipbuf[64];
  snprintf(ipbuf, sizeof(ipbuf), "WiFi: %s   PS mode %lu",
           WiFi.localIP().toString().c_str(), (unsigned long)wifi_ps_mode);
  lv_label_set_text(lbl_ip, ipbuf);

  // SAY WHICH ATTEMPT WORKED. The status line used to show the attempt
  // currently in progress and was then immediately replaced by "Ready", so
  // the number that succeeded was never displayed - the last one you could
  // catch was whichever was in flight when you happened to look, which makes
  // every join look like it took every attempt. Reporting it after the fact
  // turns that into a fact instead of an impression.
  char msg[96];
  snprintf(msg, sizeof(msg),
           "Ready (joined on attempt %d of %d). Tap Update to fetch a new image.",
           used, WIFI_ATTEMPTS);
  setStatus(msg);

  Serial.print("[wifi] joined on attempt ");
  Serial.print(used);
  Serial.print(" of ");
  Serial.println(WIFI_ATTEMPTS);
  return true;
}

/* -------------------------------------------------------------------------
   setup / loop
   ------------------------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  for (const auto timeout = millis() + 2500; !Serial && millis() < timeout; delay(250))
    ;

  Serial.println();
  Serial.println("=== Giga_OTA " FW_VERSION " ===");

  reportQspiPartitions();

  Display.begin();      // calls lv_init() internally
  TouchDetector.begin();

  // A second pointer indev alongside the real touchscreen, so tools/sim_tap.py
  // can press the button. Registered after the touch driver so both are live.
  lv_indev_t *simIndev = lv_indev_create();
  lv_indev_set_type(simIndev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(simIndev, simTouchReadCb);

  buildUi();
  lv_refr_now(NULL);

  if (WiFi.status() == WL_NO_SHIELD || WiFi.status() == WL_NO_MODULE) {
    setStatus("No WiFi module.");
    return;
  }

  tryJoinWiFi();
}

void loop() {
  lv_timer_handler();
  serviceDevCommands();

  // Keep trying to get back on the network. Cheap, and it is what stops one
  // bad moment at power-up from stranding the board for good.
  if (WiFi.status() != WL_CONNECTED && millis() - wifi_lastTry > WIFI_RETRY_MS) {
    tryJoinWiFi();
  }

  // Outside the LVGL event callback, as above. Cleared first so a failed
  // attempt cannot re-trigger itself.
  if (ota_requested) {
    ota_requested = false;
    if (WiFi.status() != WL_CONNECTED) {
      // Worth its own message: "download failed" sent me looking at the
      // server when the board simply had no network.
      setStatus("Not on WiFi yet - cannot update. Waiting for a connection.");
      lv_obj_clear_state(btn_update, LV_STATE_DISABLED);
      return;
    }
    runOta();
    // Only reached if the OTA did not happen; a success never returns.
    lv_obj_clear_state(btn_update, LV_STATE_DISABLED);
  }

  delay(5);
}

#include <Arduino.h>
#include <bluefruit.h>
#include "Adafruit_TinyUSB.h"

// =====================================================
// 三 buffer pipeline 設計：
//   cur     = 正在打字
//   next    = 已收完，等待打字
//   incoming = 正在接收新資料
// 打完 cur → 切換到 next → incoming 成為新的 next
// BEGIN 永遠寫入 incoming，不會覆蓋 next
// =====================================================

enum ImeCmd { IME_NONE=0, IME_WIN_SPACE, IME_CTRL_SPACE, IME_SHIFT_SPACE, IME_ALT_SHIFT };
void processBle();

BLEDfu  bledfu;
BLEDis  bledis;
BLEUart bleuart;

uint8_t const desc_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
Adafruit_USBD_HID usb_hid(desc_hid_report, sizeof(desc_hid_report),
                           HID_ITF_PROTOCOL_KEYBOARD, 2, false);

static const char*  DEVICE_NAME   = "WebTyper_003";
static const size_t LINE_BUF_SIZE = 256;
static const size_t BUF_SIZE      = 6000; // 三個 buffer 各 6K

// ── 三個 buffer ──
struct Segment {
  char   text[BUF_SIZE];
  size_t len;
  int    cps;
  bool   switchIme;
  ImeCmd ime;
  bool   ideIndent; // true=IDE 縮排狀態機；false=原文逐字原封不動
};

Segment segCur, segNext, segIncoming;
bool nextReady     = false; // segNext 有資料可打
bool receivingText = false;
bool textReady     = false; // segIncoming 收完，可移入 segNext
volatile bool stopReq = false;

char lineBuf[LINE_BUF_SIZE];
size_t lineLen = 0;

void pressKey(uint8_t mod, uint8_t key);
bool asciiToHid(char c, uint8_t &mod, uint8_t &key);

// =====================================================
// IDE 縮排狀態機（4 空格 = 1 Tab；Enter 繼承上一行；Backspace 刪 1 空格）
// =====================================================
static const int SPACES_PER_TAB = 4;

static int ide_current_spaces = 0;  // 模擬 IDE 游標前行首累積空格數
static int line_leading_spaces = 0; // 目前這一行開頭的縮排空格數

void resetIndentState() {
  ide_current_spaces = 0;
  line_leading_spaces = 0;
}

// 計算一行開頭「預期」有多少空格（Tab 算 4、空格算 1）
int countLeadingSpaces(const char* text, size_t start, size_t end) {
  int n = 0;
  for (size_t i = start; i < end; i++) {
    if (text[i] == '\t') n += SPACES_PER_TAB;
    else if (text[i] == ' ') n += 1;
    else break;
  }
  return n;
}

void pressEnter(uint32_t d) {
  pressKey(0, HID_KEY_ENTER);
  // IDE 純繼承：新行行首空格數 = 上一行行首縮排（保持不變）
  ide_current_spaces = line_leading_spaces;
  delay(d);
}

void pressTabOut(uint32_t d) {
  pressKey(0, HID_KEY_TAB);
  ide_current_spaces += SPACES_PER_TAB;
  delay(d);
}

void pressSpaceOut(uint32_t d) {
  pressKey(0, HID_KEY_SPACE);
  ide_current_spaces++;
  delay(d);
}

// 輸出 n 個空格-worth 的縮排（每 4 空格發 1 Tab）
void emitIndentSpaces(int spaces, uint32_t d) {
  while (spaces >= SPACES_PER_TAB) {
    pressTabOut(d);
    spaces -= SPACES_PER_TAB;
  }
  while (spaces > 0) {
    pressSpaceOut(d);
    spaces--;
  }
}

// 輸出字串中的行首空白（4 連空格 → Tab）
void emitLeadingFromSource(const char* text, size_t start, size_t end, uint32_t d) {
  size_t i = start;
  while (i < end && (text[i] == ' ' || text[i] == '\t')) {
    if (text[i] == '\t') {
      pressTabOut(d);
      i++;
    } else {
      int run = 0;
      while (i < end && text[i] == ' ') { run++; i++; }
      emitIndentSpaces(run, d);
    }
  }
}

// 輸出一般字元（含行內 4 空格轉 Tab 的累積器）
void emitCharWithSpaceRun(char c, int& space_run, uint32_t d) {
  if (c == ' ') {
    space_run++;
    if (space_run >= SPACES_PER_TAB) {
      pressTabOut(d);
      space_run = 0;
    }
    return;
  }
  if (space_run > 0) {
    emitIndentSpaces(space_run, d);
    space_run = 0;
  }
  uint8_t mod, key;
  if (asciiToHid(c, mod, key)) {
    pressKey(mod, key);
    delay(d);
  }
}

// =====================================================
// Base64
// =====================================================
int b64val(char c) {
  if (c>='A'&&c<='Z') return c-'A';
  if (c>='a'&&c<='z') return c-'a'+26;
  if (c>='0'&&c<='9') return c-'0'+52;
  if (c=='+') return 62; if (c=='/') return 63; return -1;
}

void appendBase64(Segment& seg, const char* src) {
  int len = strlen(src);
  for (int i = 0; i < len; i += 4) {
    int v0=b64val(src[i]), v1=(i+1<len)?b64val(src[i+1]):0;
    int v2=(i+2<len)?b64val(src[i+2]):0, v3=(i+3<len)?b64val(src[i+3]):0;
    if (v0<0||v1<0) break;
    if (seg.len<BUF_SIZE-1) seg.text[seg.len++]=(v0<<2)|(v1>>4);
    if (src[i+2]!='='&&seg.len<BUF_SIZE-1) seg.text[seg.len++]=(v1<<4)|(v2>>2);
    if (src[i+3]!='='&&seg.len<BUF_SIZE-1) seg.text[seg.len++]=(v2<<6)|v3;
  }
  seg.text[seg.len]='\0';
}

// =====================================================
// BLE 回應
// =====================================================
void sendOK()  { bleuart.print("OK\n");  }
void sendERR() { bleuart.print("ERR\n"); }

// =====================================================
// USB HID
// =====================================================
void waitUsbMount(uint32_t ms=5000) {
  uint32_t t=millis();
  while (!TinyUSBDevice.mounted()&&millis()-t<ms) delay(10);
}
void releaseKeys() {
  if (!usb_hid.ready()) return;
  uint8_t k[6]={0}; usb_hid.keyboardReport(0,0,k); delay(8);
}
void pressKey(uint8_t mod, uint8_t key) {
  if (TinyUSBDevice.suspended()) TinyUSBDevice.remoteWakeup();
  while (!usb_hid.ready()) delay(1);
  uint8_t k[6]={0}; k[0]=key;
  usb_hid.keyboardReport(0,mod,k); delay(8); releaseKeys();
}
void pressCombo(uint8_t mod, uint8_t key) {
  if (TinyUSBDevice.suspended()) TinyUSBDevice.remoteWakeup();
  while (!usb_hid.ready()) delay(1);
  uint8_t k[6]={0}; k[0]=key;
  usb_hid.keyboardReport(0,mod,k); delay(30); releaseKeys(); delay(30);
}

bool asciiToHid(char c, uint8_t &mod, uint8_t &key) {
  mod=0; key=0;
  if (c>='a'&&c<='z'){key=HID_KEY_A+(c-'a');return true;}
  if (c>='A'&&c<='Z'){mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_A+(c-'A');return true;}
  if (c>='1'&&c<='9'){key=HID_KEY_1+(c-'1');return true;}
  if (c=='0'){key=HID_KEY_0;return true;}
  switch(c){
    case ' ':  key=HID_KEY_SPACE;return true;
    case '\n': key=HID_KEY_ENTER;return true;
    case '\r': return false;
    case '\b': key=HID_KEY_BACKSPACE;return true;
    case '\t': key=HID_KEY_TAB;return true;
    case '-':  key=HID_KEY_MINUS;return true;
    case '_':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_MINUS;return true;
    case '=':  key=HID_KEY_EQUAL;return true;
    case '+':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_EQUAL;return true;
    case '[':  key=HID_KEY_BRACKET_LEFT;return true;
    case '{':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_BRACKET_LEFT;return true;
    case ']':  key=HID_KEY_BRACKET_RIGHT;return true;
    case '}':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_BRACKET_RIGHT;return true;
    case '\\': key=HID_KEY_BACKSLASH;return true;
    case '|':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_BACKSLASH;return true;
    case ';':  key=HID_KEY_SEMICOLON;return true;
    case ':':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_SEMICOLON;return true;
    case '\'': key=HID_KEY_APOSTROPHE;return true;
    case '"':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_APOSTROPHE;return true;
    case ',':  key=HID_KEY_COMMA;return true;
    case '<':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_COMMA;return true;
    case '.':  key=HID_KEY_PERIOD;return true;
    case '>':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_PERIOD;return true;
    case '/':  key=HID_KEY_SLASH;return true;
    case '?':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_SLASH;return true;
    case '`':  key=HID_KEY_GRAVE;return true;
    case '~':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_GRAVE;return true;
    case '!':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_1;return true;
    case '@':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_2;return true;
    case '#':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_3;return true;
    case '$':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_4;return true;
    case '%':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_5;return true;
    case '^':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_6;return true;
    case '&':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_7;return true;
    case '*':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_8;return true;
    case '(':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_9;return true;
    case ')':  mod=KEYBOARD_MODIFIER_LEFTSHIFT;key=HID_KEY_0;return true;
  }
  return false;
}

void doImeSwitch(ImeCmd cmd) {
  switch(cmd){
    case IME_WIN_SPACE:   pressCombo(KEYBOARD_MODIFIER_LEFTGUI,   HID_KEY_SPACE);      break;
    case IME_CTRL_SPACE:  pressCombo(KEYBOARD_MODIFIER_LEFTCTRL,  HID_KEY_SPACE);      break;
    case IME_SHIFT_SPACE: pressCombo(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_SPACE);      break;
    case IME_ALT_SHIFT:   pressCombo(KEYBOARD_MODIFIER_LEFTALT,   HID_KEY_SHIFT_LEFT); break;
    default: break;
  }
}

// =====================================================
// 打字：非 IDE = 原文逐字；IDE = 縮排狀態機
// =====================================================
void typeSegRaw(Segment& seg, uint32_t d) {
  for (size_t i = 0; i < seg.len; i++) {
    if (stopReq) break;
    processBle();
    uint8_t mod, key;
    if (asciiToHid(seg.text[i], mod, key)) {
      pressKey(mod, key);
      delay(d);
    }
  }
}

void typeSegIde(Segment& seg, uint32_t d) {
  resetIndentState();
  size_t pos = 0;
  bool firstLine = true;
  int space_run = 0;

  while (pos < seg.len && !stopReq) {
    processBle();

    size_t lineStart = pos;
    while (pos < seg.len && seg.text[pos] != '\n' && seg.text[pos] != '\r') pos++;
    size_t lineEnd = pos;

    int incoming = countLeadingSpaces(seg.text, lineStart, lineEnd);

    if (!firstLine) pressEnter(d);

    // 每行完整輸出（含行首 Tab/空格），不自動 Backspace
    for (size_t i = lineStart; i < lineEnd; i++) {
      processBle();
      if (stopReq) break;
      if (seg.text[i] == '\t') {
        if (space_run > 0) { emitIndentSpaces(space_run, d); space_run = 0; }
        pressTabOut(d);
      } else if (seg.text[i] == ' ') {
        space_run++;
        if (space_run >= SPACES_PER_TAB) { pressTabOut(d); space_run = 0; }
      } else {
        emitCharWithSpaceRun(seg.text[i], space_run, d);
      }
    }
    if (space_run > 0) { emitIndentSpaces(space_run, d); space_run = 0; }
    ide_current_spaces = incoming;

    line_leading_spaces = incoming;
    firstLine = false;

    while (pos < seg.len && (seg.text[pos] == '\n' || seg.text[pos] == '\r')) pos++;
  }

  if (space_run > 0) emitIndentSpaces(space_run, d);
}

void typeSeg(Segment& seg) {
  if (seg.len == 0) { bleuart.print("DONE\n"); return; }
  if (seg.switchIme && seg.ime != IME_NONE) { doImeSwitch(seg.ime); delay(300); }
  uint32_t d = (seg.cps > 0) ? max(2UL, 1000UL/(uint32_t)seg.cps) : 50;

  // 預設原文逐字；僅 BEGIN 第 4 參數為 1 時啟用 IDE 縮排（無自動 Backspace）
  if (seg.ideIndent) typeSegIde(seg, d);
  else typeSegRaw(seg, d);

  releaseKeys();
  seg.len = 0; seg.text[0] = '\0';
  bleuart.print("DONE\n");

  // 打完立刻檢查 next，有的話繼續打，無停頓
  if (nextReady) {
    nextReady = false;
    // 把 segNext 複製到 segCur 再打
    memcpy(&segCur, &segNext, sizeof(Segment));
    segNext.len = 0;
    // 如果 incoming 已收完，移入 next
    if (textReady) {
      textReady = false;
      memcpy(&segNext, &segIncoming, sizeof(Segment));
      segIncoming.len = 0;
      nextReady = true;
    }
    typeSeg(segCur);
  }
}

// =====================================================
// 指令解析
// =====================================================
ImeCmd parseIme(const char* s) {
  if (strcmp(s,"WIN_SPACE")==0)   return IME_WIN_SPACE;
  if (strcmp(s,"CTRL_SPACE")==0)  return IME_CTRL_SPACE;
  if (strcmp(s,"SHIFT_SPACE")==0) return IME_SHIFT_SPACE;
  if (strcmp(s,"ALT_SHIFT")==0)   return IME_ALT_SHIFT;
  return IME_NONE;
}

void processLine(const char* line) {
  if (strcmp(line,"PING")==0) { sendOK(); return; }
  if (strcmp(line,"STOP")==0) { stopReq=true; sendOK(); return; }

  if (strcmp(line,"END")==0) {
    receivingText = false;
    // incoming 收完：若 next 空著就直接移入，否則標記 textReady 等打完再移
    if (!nextReady) {
      memcpy(&segNext, &segIncoming, sizeof(Segment));
      segIncoming.len = 0;
      nextReady = true;
      textReady = false;
    } else {
      textReady = true;
    }
    sendOK(); return;
  }

  if (strncmp(line,"BEGIN ",6)==0) {
    // 永遠寫入 incoming，不影響 cur 和 next
    stopReq = false; // 新任務開始時解除停止狀態，允許再次打字
    resetIndentState();
    segIncoming.len = 0; segIncoming.text[0] = '\0';
    receivingText = true; textReady = false;
    char tmp[LINE_BUF_SIZE];
    strncpy(tmp, line+6, sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
    char* tok = strtok(tmp," ");
    segIncoming.cps = tok ? atoi(tok) : 20;
    tok = strtok(NULL," ");
    segIncoming.switchIme = tok ? (tok[0]=='1') : false;
    tok = strtok(NULL," ");
    segIncoming.ime = tok ? parseIme(tok) : IME_NONE;
    tok = strtok(NULL, " ");
    segIncoming.ideIndent = tok ? (tok[0] == '1') : false;
    sendOK(); return;
  }

  if (strncmp(line,"DATA ",5)==0) {
    if (!receivingText) { sendERR(); return; }
    appendBase64(segIncoming, line+5);
    sendOK(); return;
  }

  sendERR();
}

// =====================================================
// BLE 接收
// =====================================================
void processBle() {
  while (bleuart.available()) {
    char c=(char)bleuart.read();
    if (c=='\r') continue;
    if (c=='\n') {
      lineBuf[lineLen]='\0';
      if (lineLen>0) processLine(lineBuf);
      lineLen=0; return;
    }
    if (lineLen<LINE_BUF_SIZE-1) lineBuf[lineLen++]=c;
  }
}

// =====================================================
// BLE 廣播 & 回呼
// =====================================================
void startAdv() {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32,244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

void onConnect(uint16_t h) {
  (void)h; bleuart.print("READY\n"); Serial.println("[BLE] connected");
}
void onDisconnect(uint16_t h, uint8_t r) {
  (void)h;(void)r;
  receivingText=false; textReady=false; stopReq=true; nextReady=false;
  segCur.len=0; segNext.len=0; segIncoming.len=0;
  resetIndentState();
  Serial.println("[BLE] disconnected");
}

// =====================================================
// Setup / Loop
// =====================================================
void setup() {
  Serial.begin(115200); delay(100);
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  usb_hid.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  usb_hid.setPollInterval(2);
  usb_hid.setStringDescriptor("nRF52840 USB Keyboard");
  usb_hid.begin(); delay(100); waitUsbMount();

  Bluefruit.begin(); Bluefruit.setTxPower(4); Bluefruit.setName(DEVICE_NAME);
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);
  bledfu.begin();
  bledis.setManufacturer("DIY"); bledis.setModel("nRF52840 Web Typer v4-indent"); bledis.begin();
  bleuart.begin(); startAdv();
  segCur.len=0; segNext.len=0; segIncoming.len=0;
  resetIndentState();
  Serial.println("[READY] v4 IDE indent state-machine");
}

void loop() {
  processBle();
  if (nextReady && segCur.len == 0) {
    nextReady = false;
    memcpy(&segCur, &segNext, sizeof(Segment));
    segNext.len = 0;
    if (textReady) {
      textReady = false;
      memcpy(&segNext, &segIncoming, sizeof(Segment));
      segIncoming.len = 0;
      nextReady = true;
    }
    typeSeg(segCur);
  }
  delay(1);
}

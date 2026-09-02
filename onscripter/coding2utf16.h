/* -*- C++ -*-
 *  onscripter/coding2utf16.h
 *
 *  上游编码表头的适配副本（Onscripter/coding2utf16.h 的修改版）：
 *  静态字符串缓冲按 UTF-8 长度放大 —— 上游表文件已转码为 UTF-8 源，
 *  init() 里 strcpy 的字符串在 UTF-8 下更长，原尺寸会溢出（GPL-2.0，见 Onscripter/COPYING）。
 */

#ifndef __CODING2UTF16_H__
#define __CODING2UTF16_H__

#include <stdint.h>

class Coding2UTF16{
public:
  bool force_utf8=false;
  static char space[8];
  static char minus[8];
  static char bracket[16];
  static char num_str[32];
  static char DEFAULT_START_KINSOKU[64];
  static char DEFAULT_END_KINSOKU[24];
  static char DEFAULT_SAVE_MENU_NAME[24];
  static char DEFAULT_LOAD_MENU_NAME[24];
  static char DEFAULT_SAVE_ITEM_NAME[16];
  static char MESSAGE_SAVE_EXIST[32];
  static char MESSAGE_SAVE_EMPTY[48];
  static char MESSAGE_SAVE_CONFIRM[64];
  static char MESSAGE_LOAD_CONFIRM[64];
  static char MESSAGE_RESET_CONFIRM[64];
  static char MESSAGE_END_CONFIRM[48];
  static char MESSAGE_YES[8];
  static char MESSAGE_NO[16];
  static char MESSAGE_OK[8];
  static char MESSAGE_CANCEL[24];
  virtual void init() = 0;
  virtual uint16_t conv2UTF16(uint16_t) const = 0;
  virtual uint16_t convUTF162Coding(uint16_t) const = 0;
  int convUTF16ToUTF8(unsigned char*,uint16_t) const;
  unsigned short convUTF8ToUTF16(const char**);
  virtual ~Coding2UTF16() {};
};

#endif

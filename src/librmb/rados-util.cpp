// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Copyright (c) 2017 Tallence AG and the authors
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */
#include "rados-util.h"
#include <string>
#include <sstream>

namespace librmb {

RadosUtils::RadosUtils() {
}

RadosUtils::~RadosUtils() {
}

bool RadosUtils::convert_str_to_time_t(const std::string &date, time_t *val) {
  struct tm tm;
  memset(&tm, 0, sizeof(struct tm));
  if (strptime(date.c_str(), "%Y-%m-%d %H:%M:%S", &tm)) {
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    *val = t;
    return true;
  }

  val = 0;
  return false;
}

std::string RadosUtils::convert_string_to_date(std::string &date) {
  std::string ret;
  time_t t;
  return convert_str_to_time_t(date, &t) ? std::to_string(t) : "";
}

bool RadosUtils::is_numeric(const std::string &s) {
  std::string::const_iterator it = s.begin();
  while (it != s.end() && std::isdigit(*it)) {
    ++it;
  }
  return !s.empty() && it == s.end();
}

bool RadosUtils::is_date_attribute(rbox_metadata_key &key) {
  return (key == RBOX_METADATA_OLDV1_SAVE_TIME || key == RBOX_METADATA_RECEIVED_TIME);
}
int RadosUtils::convert_time_t_to_str(const time_t &t, std::string *ret_val) {
  char buffer[256];
  struct tm *timeinfo;
  timeinfo = localtime(&t);
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  *ret_val = std::string(buffer);
  return 0;
}
std::string RadosUtils::flags_to_string(const uint8_t &flags_) {
  std::string flags;
  std::stringstream sstream;
  sstream << std::hex << flags_;
  sstream >> flags;
  return flags;
}
uint8_t RadosUtils::string_to_flags(std::string &flags_) {
  std::istringstream in(flags_);
  uint8_t flags;
  if (in >> std::hex >> flags) {
    return flags;
  }
  return -1;
}
} /* namespace tallence */
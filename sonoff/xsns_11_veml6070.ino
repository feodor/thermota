/*
  xsns_11_veml6070.ino - VEML6070 ultra violet light sensor support for Sonoff-Tasmota

  Copyright (C) 2017  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_I2C
#ifdef USE_VEML6070
/*********************************************************************************************\
 * VEML6070 - Ultra Violet Light Intensity
\*********************************************************************************************/

#define VEML6070_ADDR_H      0x39
#define VEML6070_ADDR_L      0x38

#define VEML6070_INTEGRATION_TIME 3  // 500msec integration time

uint8_t veml6070_address;
uint8_t veml6070_type = 0;
char veml6070_types[9];

uint16_t Veml6070ReadUv()
{
  if (Wire.requestFrom(VEML6070_ADDR_H, 1) != 1) {
    return -1;
  }
  uint16_t uvi = Wire.read();
  uvi <<= 8;
  if (Wire.requestFrom(VEML6070_ADDR_L, 1) != 1) {
    return -1;
  }
  uvi |= Wire.read();

  return uvi;
}

/********************************************************************************************/

boolean Veml6070Detect()
{
  if (veml6070_type) {
    return true;
  }

  uint8_t status;
  uint8_t itime = VEML6070_INTEGRATION_TIME;
  boolean success = false;

  veml6070_address = VEML6070_ADDR_L;
  Wire.beginTransmission(veml6070_address);
  Wire.write((itime << 2) | 0x02);
  status = Wire.endTransmission();
  if (!status) {
    success = true;
    veml6070_type = 1;
    strcpy_P(veml6070_types, PSTR("VEML6070"));
  }
  if (success) {
    AddLog_PP(LOG_LEVEL_DEBUG, S_LOG_I2C_FOUND_AT, veml6070_types, veml6070_address);
  } else {
    veml6070_type = 0;
  }
  return success;
}

#ifdef USE_WEBSERVER
const char HTTP_SNS_ULTRAVIOLET[] PROGMEM =
  "%s{s}VEML6070 " D_UV_LEVEL "{m}%d{e}";  // {s} = <tr><th>, {m} = </th><td>, {e} = </td></tr>
#endif  // USE_WEBSERVER

void Veml6070Show(boolean json)
{
  if (veml6070_type) {
    uint16_t uvlevel = Veml6070ReadUv();

    if (json) {
      mqtt_msg.sprintf_P(F(", \"%s\":{\"" D_UV_LEVEL "\":%d}"), veml6070_types, uvlevel);
#ifdef USE_DOMOTICZ
      DomoticzSensor(DZ_ILLUMINANCE, uvlevel);
#endif  // USE_DOMOTICZ
#ifdef USE_WEBSERVER
    } else {
      mqtt_msg.sprintf_P(FPSTR( HTTP_SNS_ULTRAVIOLET), uvlevel);
#endif  // USE_WEBSERVER
    }
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

#define XSNS_11

boolean Xsns11(byte function, void *arg)
{
  boolean result = false;

  if (i2c_flg) {
    switch (function) {
//      case FUNC_XSNS_INIT:
//        break;
      case FUNC_XSNS_PREP:
        Veml6070Detect();
        break;
      case FUNC_XSNS_JSON_APPEND:
        Veml6070Show(1);
        break;
#ifdef USE_WEBSERVER
      case FUNC_XSNS_WEB:
        Veml6070Show(0);
        break;
#endif  // USE_WEBSERVER
    }
  }
  return result;
}

#endif  // USE_VEML6070
#endif  // USE_I2C


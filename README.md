# E-paper demo

## ESP32-WROOM-32

| 墨水屏驱动板 | 功能       | ESP32-WROOM-32 | 说明                |
| ------ | -------- | -------------: | ----------------- |
| GND    | 电源地      |            GND | 必须共地              |
| 3V3    | 3.3V 电源  |            3V3 | 不建议接 VIN/5V       |
| SCK    | SPI 时钟   |         GPIO18 | ESP32 默认 VSPI SCK |
| SDA    | SPI MOSI |         GPIO23 | 不是 I²C SDA        |
| RST    | 屏幕复位     |         GPIO25 | 普通输出引脚            |
| DC     | 数据/命令选择  |         GPIO26 | 普通输出引脚            |
| CS1    | SPI 片选   |         GPIO27 | 单芯片屏使用 CS1        |
| BUSY   | 屏幕忙信号    |         GPIO33 | ESP32 输入          |
| CS2    | 第二片选     |            不连接 | 2.13 寸单芯片屏通常不用    |

## for Claude

```
墨水屏驱动板        ESP32 DevKit

GND      --------> GND
3V3      --------> 3V3
SCK      --------> GPIO18
SDA      --------> GPIO23
RST      --------> GPIO25
DC       --------> GPIO26
CS       --------> GPIO27
BUSY     --------> GPIO33
```

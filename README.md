# ESP32 W5500 Wake-on-LAN

Dự án ESP-IDF cho ESP32 dùng Wi-Fi kết nối Blynk và W5500 nối trực tiếp với PC (`192.168.10.1`). W5500 dùng IP `192.168.10.2/24`.

| ESP32 | W5500 |
| --- | --- |
| GPIO23 | MO / MOSI |
| GPIO16 | MI / MISO |
| GPIO18 | SCK |
| GPIO5 | CS |
| GPIO4 | INT |
| GPIO19 | RST |
| 3,3 V | V |
| GND | G |

Chạy `setup.ps1` để cài ESP-IDF và công cụ vào thư mục dự án, rồi dùng `idf.ps1 build`. Cấu hình Wi-Fi và Blynk qua AP `WoL-Setup` tại `http://192.168.4.1/`; đặt MAC của PC qua Blynk V3 trước khi gửi WoL bằng V0.

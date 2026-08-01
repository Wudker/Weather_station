# ESP32 Solar Weather Station

ESP32 Solar Weather Station is a solar-assisted environmental monitoring system consisting of an outdoor measurement module and an indoor receiver. The outdoor unit periodically measures temperature, humidity, atmospheric pressure, wind speed, rainfall, battery voltage and solar-panel voltage. The collected data is transmitted via ESP-NOW to the receiver, which presents current measurements and recent trends through a local web dashboard. RTC-controlled wake-up and sleep modes reduce energy consumption, while a Li-Ion battery and photovoltaic panel support long-term outdoor operation.

## Technologies

`ESP32` `C/C++` `ESP-NOW` `BME280` `SS41F Hall sensor` `Rain sensor` `RTC` `Li-Ion battery` `Photovoltaic power` `ESP32 WebServer` `Low-power operation`

## Usage and development status

Configure the wireless and network parameters, then upload the appropriate firmware to the outdoor and receiver ESP32 modules. After starting both devices, measurements can be accessed through the receiver's local web interface. The current prototype is functional and has been tested; planned improvements include a custom PCB, better enclosure sealing, optimized power management and expanded data visualization.

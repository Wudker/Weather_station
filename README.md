# ESP32 Solar Weather Station

## Description

ESP32 Solar Weather Station is an autonomous outdoor measurement system designed for long-term environmental monitoring. The system periodically collects weather data, wirelessly transmits the results to a receiver and displays measurements through a local web interface.

The project was developed with emphasis on low-power operation and partial energy self-sufficiency using a rechargeable battery and photovoltaic panel.

The complete system consists of two cooperating devices:

* Outdoor measurement module
* Indoor receiver module with local web interface

## Features

* Temperature measurement
* Humidity measurement
* Atmospheric pressure measurement
* Wind speed measurement
* Rain detection
* Battery voltage monitoring
* Solar panel monitoring
* Wireless communication
* Local web dashboard
* Low-power operation

## Hardware

### Outdoor module

* ESP32
* BME280
* SS41F Hall sensor
* Rain sensor
* RTC module
* Li-Ion battery
* Solar panel
* Charging module

### Receiver module

* ESP32
* Local WebServer

## Software

* C/C++
* ESP-NOW communication
* WebServer
* Low-power firmware
* RTC-based wake-up
* Sensor integration
* Data visualization

## System operation

The outdoor module periodically wakes up, performs measurements and sends collected data wirelessly to the receiver.

The receiver stores the latest measurements and provides them through a local web interface where environmental data and trends can be monitored.

To reduce power consumption, the measurement module remains in sleep mode for most of its operating cycle.

## Challenges

Main development challenges:

* reducing power consumption,
* designing a stable power path,
* synchronization of wake-up cycles,
* wireless communication reliability,
* environmental protection of the electronics,
* balancing energy consumption with solar charging.

## Current status

Working prototype built and tested.

Future improvements:

* custom PCB design,
* improved enclosure sealing,
* optimized power management,
* extended weather data visualization,
* cloud integration support.

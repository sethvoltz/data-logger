# Home Logger with Raspberry Pi and ESP8266

![hardware photos](https://raw.githubusercontent.com/sethvoltz/data-logger/master/media/header.jpg)

A (hopefully) simple to setup and replicate home sensor logger setup using ESP8266 nodes which log
sensor readings to a central aggregator over MQTT. Aggregator lives on a Raspberry Pi and uses
Node RED, InfluxDB, and Grafana to route, store and visualize the data.

Initial sensor design is for temperature, humidity and ambient light levels (log scale). Additional
interesting measures include barometric pressure, HVAC state, state of smart home switches, &c.
Usage of the DarkSky API allows for direct comparison of outdoor weather conditions in lieu of a
weather resistent sensor node.

## Setup

_Note: These instructions are macOS X-centric, however everything has analogs on Linux and Windows
as well. All instructions on the Raspberry Pi are universal._

### Raspberry Pi

#### Image new SD card for Raspberry Pi and setup

On your computer, install balenaEtcher (e.g. `brew cask install balenaetcher`) then write image to SD card. Re-mount image and `touch /Volumes/boot/ssh` to enable SSH on first-boot. First boot may take a few miniutes. When it's up, SSH in with `ssh pi@raspberrypi.local`.

On the Raspberry Pi, run the rapid setup:

```bash
sudo raspi-config
# Expand volume, set hostname to `logger`, set locale
# Go to "Interfacing Options" > "Serial" - Disable login shell over serial, enable serial port HW
# Lastly, run the update
sudo apt-get update && sudo apt-get upgrade && sudo apt-get autoremove
sudo apt-get install -y avahi-daemon vim python python-pip git
echo gpu_mem=16 | sudo tee -a /boot/config.txt
echo hdmi_force_hotplug=1 | sudo tee -a /boot/config.txt
echo hdmi_drive=2 | sudo tee -a /boot/config.txt
echo 'MODE="0666", SUBSYSTEM=="tty", ATTRS{idVendor}=="0658", ATTRS{idProduct}=="0200", SYMLINK+="zwave"' | sudo tee -a /etc/udev/rules.d/99-usb-serial.rules
sudo reboot
```

#### Setup USB external drive, reformat and configure to boot from drive

[Boot from USB Drive](https://www.tomshardware.com/news/boot-raspberry-pi-from-usb,39782.html),
[Use `gparted` for partitioning](https://pimylifeup.com/partition-and-format-drives-on-linux/).

Connect the external hard drive:

```bash
sudo fdisk -l # ensure /dev/sda is the external drive
sudo parted /dev/sda mklabel gpt
sudo parted /dev/sda mkpart primary ext4 0G 100%
sudo mkfs.ext4 /dev/sda1
sudo mkdir /media/usbdrive
sudo mount /dev/sda1 /media/usbdrive
sudo rsync -avx / /media/usbdrive
sudo nano /boot/cmdline.txt # append the following line to the end of first line
# root=/dev/sda1 rootfstype=ext4 rootwait
sudo reboot
```

_Note:_ You still need the SD Card _and_ the HDD plugged in to boot. Either missing will cause it to fail to boot or kernel panic.

#### Setup Docker

SSH back in with `ssh pi@logger.local`

```bash
curl -sSL https://get.docker.com | sh
sudo usermod -aG docker pi
sudo systemctl enable docker
sudo systemctl start docker
sudo apt-get install docker-compose
```

#### Setup Services

First, be sure the Z-wave controller USB device is plugged in before proceeding.

From your local machine:

```bash
git clone git@github.com:sethvoltz/data-logger.git logger
cd logger
./bin/sync
```

From the Raspberry Pi:

```bash
cd ~/logger
./bin/setup
sudo reboot
```

#### Home Assistant

[http://logger.local:8123](http://logger.local:8123)

* [Z-Wave Docs](https://www.home-assistant.io/docs/z-wave/installation/)
* [Z-Wave Installation](https://blog.mornati.net/install-zwave-home-assistant/)
* [Alias Serial Devices](http://hintshop.ludvig.co.nz/show/persistent-names-usb-serial-devices/)

Software First Setup:

* Follow the onboarding steps according to the on-screen prompts
* When the page asking to set up additional services, clic the `(...)` more button
* In the modal, search for `zwave` and select the option
* In the new modal, enter `/dev/ttyACM0` in the "USB Path" entry

#### Node RED

[http://logger.local:1880](http://logger.local:1880)

* Go to hamburger in upper right, click menu and choose "Manage Palette"
* Go to the search tab, search for InfluxDB and install `node-red-contrib-influxdb`
* Also search and install `node-red-node-darksky`

#### Grafana

[http://logger.local:3000](http://logger.local:3000)

* Add "InfluxDB" source, using URL `http://influxdb:8086`, no credentials, database name `iot`
* Default Grafana u/p is `admin`/`admin`, you'll be required to change password to something else, e.g. `raspberry` ;)

### ESP8266 Nodes

The individual nodes are composed of an [Adafruit ESP8266 Huzzah][huzzah] and a series of sensors to measure temperature, humidity and ambient light levels (lux).

The hardware will be described in a future blog post, including how the case was assembled and wired. The `cad/` directory includes an SVG file for the case, with layers for the main body, living hinge shell and the diffuser. Note that the diffuser is roughly 0.2mm larger to compensate for standard laser cutter kerf. You will likely need to sand or file it down for a tight fit before securing with acrylic cement.

To load the firmware onto the Huzzah, load the `firmware/` directory in the [PlatformIO][pio], which can be installed standalone, or on a number of other editors, such as [VS Code][vs-pio]. From there, plug in your Huzzah with a serial cable, press the button sequence to put it in firmware loading mode, and run the `PlatformIO: Upload` command. Once loaded:

[huzzah]: https://www.adafruit.com/product/2471
[pio]: https://platformio.org/
[vs-ide]: https://platformio.org/install/ide?install=vscode

* Enter Captive Portal mode. If this is your first install, it will automatically be there. If not, press and hold the button for roughly 3 seconds. You are in captive portal mode when the indicator lights are a steady deep blue.
* Connect to the network starting with "Setup Data Collector" -- the value following is the deviceID
* Choose your wifi network and enter credentials, as well as the MQTT server IP and port, and a name describing where the device is, e.g. 'living-room'. Avoid spaces.
* Press save. This will save your information to the onboard memory and reload the config, attempting to connect to the wifi network and MQTT server.

As soon as it is successful, you will be greeted with a breathing white indicator. It is now collecting sensor data and reporting to the MQTT server as a JSON payload, perfect for NodeRED to parse and consume.

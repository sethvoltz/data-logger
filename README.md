# Home Logger with Raspberry Pi and ESP8266

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

### Raspnerry Pi

* Image new SD card for Raspberry Pi and setup
  * Write image to SD card using balenaEtcher, `brew cask install balenaetcher`
  * Re-mount image and `touch /Volumes/boot/ssh` to enable SSH on first-boot
  * Find IP from router via DHCP leases
    * _Note: this may be obviated by [this note][headless] and `ssh pi@raspberrypi.local` would
      work without additional config_
  * `sudo raspi-config` – set hostname to `logger`, locale, and run the update
  * Rapid setup
    ```bash
    sudo apt-get update && sudo apt-get upgrade && sudo apt-get autoremove
    sudo apt-get install -y avahi-daemon vim python python-pip git
    sudo echo "gpu_mem=16" >> /boot/config.txt
    sudo reboot
    ```
* Setup Docker
  * SSH back in with `ssh pi@logger.local`
    ```bash
    curl -sSL https://get.docker.com | sh
    sudo usermod -aG docker pi
    sudo systemctl enable docker
    sudo systemctl start docker
    sudo pip install docker-compose
    ```
* Setup USB external drive and reformat
  * TBD, once I get a new external endrive to use.
* Setup Service
  * From your local machine
    ```bash
    git clone <this-repo> logger
    cd logger
    ./bin/sync
    ```
  * From the Raspberry Pi
    ```bash
    cd ~/logger
    ./bin/setup
    sudo reboot
    ```
* Log into the services and set them up
  * Node RED: [http://logger.local:1880](http://logger.local:1880)
    * Go to hamburger in upper right, click menu and choose "Manage Palette"
    * Go to the search tab, search for InfluxDB and install `node-red-contrib-influxdb`
    * Also search and install `node-red-node-darksky`
  * Grafana: [http://logger.local:3000](http://logger.local:3000)
    * Add "InfluxDB" source, using URL `http://influxdb:8086`, no credentials, database name `iot`
    * Default Grafana u/p is `admin`/`admin`, you'll be required to change password to something else, e.g. `raspberry` ;)

### ESP8266 Nodes

_TBD: once I write the code_

# Hermes App

Wildfires have been ever-increasing, devouring our planet earth, rendering our planet worse day by day. With early detection and mitigation, it is possible to reduce the damage caused by wildfires. To enable our front-line workers, here is Hermes, an AI-powered Computer Vision application that helps in early detection of Wildfires using reconnaissance drones.

## Citations

* [AlexeyAB/darknet](https://github.com/AlexeyAB/darknet)
* [damiafuentes/DJITelloPy](https://github.com/damiafuentes/DJITelloPy)

## Index

1. [Introduction](#Introduction)
2. [Deepstream Setup](#Deepstream-Setup)
    1. [Install System Dependencies](#Install-System-Dependencies)
    2. [Install Deepstream](#Install-Deepstream)
3. [Ryze Tello Setup](#Ryze-Tello-Setup)
    1. [Installing pip packages](#Installing-pip-packages)
    2. [Connecting the Tello](#Connecting-the-Tello)
4. [Running the Application](#Running-the-Application)
    1. [Run with different input sources](#Run-with-different-input-sources)
    2. [Run with the drone](#Run-with-the-drone)

## Introduction

Hermes Application consists of two parts. An Intelligent Video Analytics Pipeline powered by Deepstream and NVIDIA Jetson Xavier NX and a reconnaissance drone, for which we will be using Ryze Tello.

![Tello and Jetson NX](resources/tellonx.jpg)

This project is a proof-of-concept, trying to show that surveillance and mapping of wildfires can be done with a drone and an onboard Jetson platform.

## Deepstream Setup

This post assumes you have a fully functional Jetson device. If not, you can refer the documentation [here](https://docs.nvidia.com/jetson/jetpack/install-jetpack/index.html).

### 1. Install System Dependencies

```sh
sudo apt install \
libssl1.0.0 \
libgstreamer1.0-0 \
gstreamer1.0-tools \
gstreamer1.0-plugins-good \
gstreamer1.0-plugins-bad \
gstreamer1.0-plugins-ugly \
gstreamer1.0-libav \
libgstrtspserver-1.0-0 \
libjansson4=2.11-1
```

### 2. Install Deepstream

Download the DeepStream 5.0.1 Jetson Debian package `deepstream-5.0_5.0.1-1_arm64.deb`, to the Jetson device from [here](https://developer.nvidia.com/assets/Deepstream/5.0/ga/secure/deepstream_sdk_5.0.1_amd64.deb). Then enter the command:

```sh
sudo apt-get install ./deepstream-5.0_5.0.1-1_arm64.deb
```

## Ryze Tello Setup

### 1. Installing pip packages

First, we need to install python dependencies. Make sure you have a working build of python3.7/3.8

```sh
sudo apt install python3-dev python3-pip
```

The dependencies needed are the following:

```sh
djitellopy==1.5
imutils==0.5.3
numpy==1.19.4
opencv-python==4.4.0.46
pygame==2.0.1
```

You can either install them with pip command or use the requirements.txt file. Whatever sails your boat :)

```sh
# For individial packages
pip3 install <packagename>

# For requirements.txt
pip3 install -r requirements.txt
```

### 2. Connecting the Tello

First, connect the Jetson Device to the WiFi network of Tello.

![Ryze Tello WiFi](resources/dronewifi.png)

Next, run the following code to verify connectivity

```python
# Importing the Tello Drone Library
from djitellopy import Tello
pkg = Tello()
pkg.connect()
```

On successful connection, your output will look something like this

```json
Send command: command
Response: b'ok'
```

If you get the following output, you may want to check your connection with the drone

```json
Send command: command
Timeout exceed on command command
Command command was unsuccessful. Message: False
```

## Running the Application

### 1. Run with different input sources

The computer vision part of the solution can be run on one or many input sources of multiple types, all powered using NVIDIA Deepstream.

First, build the application by running the following command:

```sh
make clean && make -j$(nproc)
```

This will generate the binary called `hermes-app`. This is a one-time step and you need to do this only when you make source-code changes.

Next, create a file called `inputsources.txt` and paste the path of videos or rtsp url.

```sh
file:///home/astr1x/Videos/Wildfire1.mp4
rtsp://admin:admin%40123@192.168.1.1:554/stream
```

Now, run the application by running the following command:

```sh
./hermes-app
```

### 2. Run with the drone

We utilize the livestream of the camera for real-time detection of wildfires.

(WIP)
from djitellopy import Tello
import cv2
import time
import os
import sys
import traceback
from pynput import keyboard

import numpy as np
import cv2
import gi
import struct
import redis
from threading import Thread

gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GObject

# Redis
redis_queue = redis.Redis(host='localhost', port=6379, db=0)

GObject.threads_init()
Gst.init(None)

global drone

class SensorFactory(GstRtspServer.RTSPMediaFactory):
    def __init__(self, width, height, fps, **properties):
        super(SensorFactory, self).__init__(**properties)

        self.key = 'tello'
        self.number_frames = 0

        self.duration = 1 / fps * Gst.SECOND  # duration of a frame in nanoseconds

        self.launch_string = 'appsrc name=source is-live=true block=true format=GST_FORMAT_TIME ' \
                             'caps=video/x-raw,format=BGR,width={},height={},framerate={}/1 ' \
                             '! videoconvert ! video/x-raw,format=I420 ' \
                             '! x264enc speed-preset=ultrafast tune=zerolatency ' \
                             '! rtph264pay config-interval=1 name=pay0 pt=96'.format(width, height, fps)

    def on_need_data(self, src, length):
        try:
            encoded = redis_queue.get(self.key)
            h, w = struct.unpack('>II',encoded[:8])
            frame = np.frombuffer(encoded, dtype=np.uint8, offset=8).reshape(h,w,3)
            data = frame.tostring()
            buf = Gst.Buffer.new_allocate(None, len(data), None)
            buf.fill(0, data)
            buf.duration = self.duration
            timestamp = self.number_frames * self.duration
            buf.pts = buf.dts = int(timestamp)
            buf.offset = timestamp
            self.number_frames += 1
            retval = src.emit('push-buffer', buf)
            if retval != Gst.FlowReturn.OK:
                print(retval)

        except:
            traceback.print_exc()

    def do_create_element(self, url):
        return Gst.parse_launch(self.launch_string)

    def do_configure(self, rtsp_media):
        self.number_frames = 0
        appsrc = rtsp_media.get_element().get_child_by_name('source')
        appsrc.connect('need-data', self.on_need_data)

class GstServer(GstRtspServer.RTSPServer):
    def __init__(self, width, height, fps, endpoint='/stream', port=6969, **properties):
        super(GstServer, self).__init__(**properties)
        self.factory = SensorFactory(width, height, fps)
        self.factory.set_shared(True)
        self.set_service(str(port))
        self.get_mount_points().add_factory(endpoint, self.factory)

        self.attach(None)

class Apollo:
    ''' Class to capture tello frames and push it. '''

    def __init__(self):
        ''' Method called when object of class is created. '''

        self.key = 'tello'
        self.width = 960
        self.height = 720
        self.fps = 30
        self.endpoint = '/hermes'
        self.port = 6969

        capthread = Thread(target=self.capFrames,
                           name='cap_thread')
        capthread.daemon = True
        capthread.start()

        GstServer(
            self.width,
            self.height,
            self.fps,
            self.endpoint,
            self.port
        )

    def capFrames(self):
        ''' Method to capture frames from 360 camera. '''

        while True:
            try:
                global drone
                frame = drone.get_frame()
                h, w = frame.shape[:2]
                shape = struct.pack('>II', h, w)
                encoded_frame = shape + frame.tobytes()
                redis_queue.set(self.key, encoded_frame)

            except KeyboardInterrupt:
                sys.exit(0)

            except:
                traceback.print_exc()

class Keys:

    def __init__(self):
        self.utilize = False

    def on_press(self, key):
        try:
            if key == keyboard.Key.shift_l:
                self.utilize = not self.utilize
            elif self.utilize and key == keyboard.Key.shift_r:
                pkg.takeoff()
            elif self.utilize and key == keyboard.Key.space:
                pkg.land()
            elif self.utilize and key == keyboard.Key.up:
                pkg.move_up(30)
            elif self.utilize and key == keyboard.Key.down:
                pkg.move_down(30)
            elif self.utilize and key == keyboard.Key.left:
                pkg.rotate_clockwise(30)
            elif self.utilize and key == keyboard.Key.right:
                pkg.rotate_counter_clockwise(30)
            elif self.utilize and key.char == 'w':
                pkg.move_forward(30)
            elif self.utilize and key.char == 'a':
                pkg.move_left(30)
            elif self.utilize and key.char == 's':
                pkg.move_back(30)
            elif self.utilize and key.char == 'd':
                pkg.move_right(30)

            else:
                pass
        except:
            pkg.land()

    def on_release(self, key):
        if key == keyboard.Key.esc:
            # Stop listener
            print('Stopping keyboard listener..')
            return False

class Drone:

    def __init__(self):

        # Keyboard input
        keys = Keys()
        listener = keyboard.Listener(
            on_press=keys.on_press,
            on_release=keys.on_release)
        listener.start()

    def get_status(self):
    	battery = pkg.get_battery()
    	fly_time = pkg.get_flight_time()
    	drone_height = pkg.get_height()
    	atmospheric_pressure = pkg.get_barometer()
    	temperature = pkg.get_temperature()
    	yaw_velocity = pkg.get_yaw()
    	speed_x = pkg.get_speed_x()
    	speed_y = pkg.get_speed_y()
    	speed_z = pkg.get_speed_z()
    	acceleration_x = pkg.get_acceleration_x()
    	acceleration_y = pkg.get_acceleration_y()
    	acceleration_z = pkg.get_acceleration_z()

    	# Function to return a dictionary of the status
    	status_files = {
        	'battery': battery,
        	'fly_time': fly_time,
        	'drone_height': drone_height,
        	'atmospheric_pressure': atmospheric_pressure,
        	'temperature': temperature,
        	'yaw_velocity': yaw_velocity,
        	'speed': (speed_x, speed_y, speed_z),
        	'acceleration':(acceleration_x,acceleration_y, acceleration_z)
    	}
    	return status_files.items()

    def switch_stream_on(self):
    	pkg.streamon()

    def switch_stream_off(self):
    	pkg.streamoff()

    def get_frame(self):
    	frame_read = pkg.get_frame_read()
    	return frame_read.frame

if __name__ == '__main__':

    # Run Gst Server

    # Instantiate the drone
    pkg = Tello()
    pkg.connect()

    global drone
    drone = Drone()
    drone.switch_stream_on()

    apollo = Apollo.__new__(Apollo)
    apollo.__init__()

    # Initiate GObject Loop
    loop = GObject.MainLoop()
    loop.run()

    drone.switch_stream_off()
    cv2.destroyAllWindows()

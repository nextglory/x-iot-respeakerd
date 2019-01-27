# -*- coding: utf-8 -*-

import os
import time
import logging
import signal
import threading
from respeakerd_source import RespeakerdSource
# from respeakerd_volume_ctl import VolumeCtl
from avs.alexa import Alexa
import sys
import mraa
from pixel_ring import pixel_ring

def main():
    logging.basicConfig(level=logging.DEBUG)
    #logging.getLogger('avs.alexa').setLevel(logging.INFO)
    logging.getLogger('hpack.hpack').setLevel(logging.INFO)

    en = mraa.Gpio(12)
    if os.geteuid() != 0 :
        time.sleep(1)
    en.dir(mraa.DIR_OUT)
    en.write(0)

    srcRespkr = RespeakerdSource()
    alexa = Alexa()
    # ctl = VolumeCtl()

    srcRespkr.link(alexa)
    pixel_ring.think()

    xStat = 'thinking'
    xDirect = 0

    """
    -------------------------------------
    Alexa Events
    -------------------------------------
    """
    def on_ready():
        global xStat
        print("===== on_ready =====\r\n")
        xStat = 'off'
        pixel_ring.off()
        srcRespkr.on_cloud_ready()

    def on_listening():
        global xStat
        global xDirect
        print("===== on_listening =====\r\n")
        if xStat != 'detected':
            print('The last dir is {}'.format(xDirect))
            pixel_ring.wakeup(xDirect)
        xStat = 'listening'
        pixel_ring.listen()

    def on_speaking():
        global xStat
        print("===== on_speaking =====\r\n")
        xStat = 'speaking'
        srcRespkr.on_speak()
        pixel_ring.speak()

    def on_thinking():
        global xStat
        print("===== on_thinking =====\r\n")
        xStat = 'thinking'
        srcRespkr.stop_capture()
        pixel_ring.think()

    def on_off():
        global xStat
        print("===== on_off =====\r\n")
        xStat = 'off'
        pixel_ring.off()

    alexa.state_listener.on_listening = on_listening
    alexa.state_listener.on_thinking = on_thinking
    alexa.state_listener.on_speaking = on_speaking
    alexa.state_listener.on_finished = on_off
    alexa.state_listener.on_ready = on_ready
    # alexa.Speaker.CallbackSetVolume(ctl.setVolume)
    # alexa.Speaker.CallbackGetVolume(ctl.getVolume)
    # alexa.Speaker.CallbackSetMute(ctl.setMute)


    """
    -------------------------------------
    Respeaker Events
    -------------------------------------
    """
    def on_detected(dir, index):
        global xStat
        global xDirect
        logging.info('detected hotword:{} at {}`'.format(index, dir))
        xStat = 'detected'
        xDirect = (dir + 360 - 60)%360
        alexa.listen()
        pixel_ring.wakeup(xDirect)

    def on_vad():
        # when someone is talking   
        print(">"),
        sys.stdout.flush()

    def on_silence():
        # when it is silent 
        pass
    
    srcRespkr.set_callback(on_detected)
    srcRespkr.set_vad_callback(on_vad)
    srcRespkr.set_silence_callback(on_silence)
    srcRespkr.recursive_start()

    is_quit = threading.Event()
    def signal_handler(signal, frame):
        print('Quit')
        is_quit.set()

    signal.signal(signal.SIGINT, signal_handler)
    while not is_quit.is_set():
        try:
            time.sleep(1)
        except SyntaxError:
            pass
        except NameError:
            pass
    srcRespkr.recursive_stop()
    en.write(1)


if __name__ == '__main__':
    main()







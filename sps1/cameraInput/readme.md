
This is the modification of the zwo application, so the zwo building rules apply.  

Make sure you use the provided Makefile and that you have installed the necessary dependencies.  


terminal 1 - build: /demo> run ```make platform=x64```

terminal 2 - start: /demo/bin/x64/> ```sudo ./test_gui2_video```

terminal 3 - watch: ```gst-launch-1.0 -v playbin uri=rtsp://192.168.1.107:8554/test uridecodebin0::source::latency=0```

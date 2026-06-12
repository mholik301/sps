
terminal 1 - build: ```g++ -o test-launch server.cpp 'pkg-config --cflags --libs opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0'```

terminal 2 - start: ```GST_DEBUG=3 ./test-launch```

terminal 3 - watch: ```gst-launch-1.0 -v playbin uri=rtsp://192.168.1.108:8554/test uridecodebin0::source::latency=0```  


note:
 - make sure the server ip hardcoded in app and used in commands matches the one actual IP of the computer. The value can be see by using the ```ip address``` command
 - increase the value of GTS_DEBUG parameter to get a more detailed output

### Viewing using VLC: rtsp://192.168.1.105:8554/test

### To see RTSP information you can use the LIVE555 Streaming Media utilities, namely openRTSP  

install guide: http://www.live555.com/liveMedia/  

example of connecting to a stream: ```./openRTSP -Q rtsp://192.168.1.105:8554/test.mkv```


### Debugging using gdb:

```G_DEBUG=fatal-warnings gdb ./test-launch```  

note: this will stop the program when an error happens, then we can use 'bt' or something else

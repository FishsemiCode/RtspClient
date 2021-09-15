LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := songrtspclient
LOCAL_SRC_FILES := songRTSPclient.c
LOCAL_SHARED_LIBRARIES := gstreamer_android libc++_shared
LOCAL_LDLIBS := -llog -landroid
include $(BUILD_SHARED_LIBRARY)

GSTREAMER_ROOT_ANDROID_arm64 := $(APP_ROOT_DIR)/../gstreamer-1.0-android-arm64-1.16.2
GSTREAMER_ROOT_ANDROID_armv7 := $(APP_ROOT_DIR)/../gstreamer-1.0-android-armv7-1.16.2

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
GSTREAMER_ROOT        := $(GSTREAMER_ROOT_ANDROID_arm64)
else
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
GSTREAMER_ROOT        := $(GSTREAMER_ROOT_ANDROID_armv7)
else
endif
#$(error Target arch ABI not supported: $(TARGET_ARCH_ABI))
endif

GSTREAMER_NDK_BUILD_PATH  := $(GSTREAMER_ROOT)/share/gst-android/ndk-build/
include $(GSTREAMER_NDK_BUILD_PATH)/plugins.mk
GSTREAMER_PLUGINS         := coreelements autodetect videoparsersbad androidmedia rtsp rtp rtpmanager \
                             udp opengl srt hls dashdemux taglib flv rtmp rtspclientsink app
G_IO_MODULES              := gnutls
GSTREAMER_EXTRA_DEPS      := gstreamer-video-1.0
include $(GSTREAMER_NDK_BUILD_PATH)/gstreamer-1.0.mk

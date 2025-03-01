LOCAL_PATH := $(call my-dir)


include $(CLEAR_VARS)
LOCAL_MODULE := test_memory

LOCAL_CFLAGS := -fvisibility=hidden -w
LOCAL_CPPFLAGS := -std=c++17
LOCAL_CPPFLAGS += -fvisibility=hidden

LOCAL_SRC_FILES := test_memory.cpp
LOCAL_SRC_FILES += hakutaku.cpp

LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3
LOCAL_CFLAGS := -mllvm -irobf-indbr -mllvm -irobf-icall -mllvm -irobf-indgv -mllvm -irobf-cse -mllvm -irobf-cff
include $(BUILD_EXECUTABLE)

# Copyright 2013 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	VendorTagDescriptorTests.cpp \
	CameraBinderTests.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \
	system/media/camera/tests \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/native/include \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_client_test
LOCAL_MODULE_TAGS := tests

include $(BUILD_NATIVE_TEST)

include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	CameraStreamingTest.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \
	system/media/camera/tests \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/native/include \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_streaming_test
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	CameraRecordingTest.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libmedia \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \
	system/media/camera/tests \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/native/include \
	frameworks/av/media/libmedia \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_recording_test
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	CameraCaptureTest.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \
	system/media/camera/tests \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/native/include \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_capture_test
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	CameraWifiDisplayTest.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libmedia \
	libstagefright \
	libstagefright_foundation \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \
	system/media/camera/tests \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/av/include/media/stagefright \
	frameworks/native/include \
	frameworks/native/include/media/openmax \
	frameworks/av/media/libmedia \
	frameworks/av/media/libstagefright \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_widi_test
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk

LOCAL_SRC_FILES:= \
	CameraMultiStreamsTest.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libcamera_metadata \
	libcamera_client \
	libmedia \
	libstagefright \
	libstagefright_foundation \
	libgui \
	libsync \
	libui \
	libdl \
	libbinder

LOCAL_C_INCLUDES += \
	system/media/camera/include \
	system/media/private/camera/include \
	system/media/camera/tests \
	frameworks/av/services/camera/libcameraservice \
	frameworks/av/include/camera \
	frameworks/av/include/media/stagefright \
	frameworks/native/include \
	frameworks/native/include/media/openmax \
	frameworks/av/media/libmedia \
	frameworks/av/media/libstagefright \

LOCAL_CFLAGS += -Wall -Wextra

LOCAL_MODULE:= camera_multi_streams_test
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "CameraMultiStreamsTests"

#include <binder/IInterface.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/List.h>
#include <utils/String8.h>
#include <utils/String16.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <system/graphics.h>
#include <hardware/gralloc.h>

#include <camera/CameraMetadata.h>
#include <camera/ICameraService.h>
#include <camera/ICameraServiceListener.h>
#include <camera/camera2/CaptureRequest.h>
#include <camera/camera2/ICameraDeviceUser.h>
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/OutputConfiguration.h>

#include <media/mediarecorder.h>

#include <gui/CpuConsumer.h>
#include <gui/BufferItemConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <utility>
#include <vector>
#include <map>

using namespace android;

#define SETUP_TIMEOUT 2000000000 // ns
#define IDLE_TIMEOUT 2000000000 // ns

#define BITRATE (8*1000*1000)
#define WIDTH 1280
#define HEIGHT 720
#define MAX_IMAGES 4
#define ALIGN(x, mask) ( ((x) + (mask) - 1) & ~((mask) - 1) )

//Service listener implementation
class CameraServiceListener : public BnCameraServiceListener {
    std::map<String16, TorchStatus> mCameraTorchStatuses;
    std::map<int32_t, Status> mCameraStatuses;
    mutable Mutex mLock;
    mutable Condition mCondition;
    mutable Condition mTorchCondition;
public:
    virtual ~CameraServiceListener() {};

    virtual void onStatusChanged(Status status, int32_t cameraId) {
		ALOGD("%s: onStatusChanged: %d", __FUNCTION__, status);
        Mutex::Autolock l(mLock);
        mCameraStatuses[cameraId] = status;
        mCondition.broadcast();
    };

    virtual void onTorchStatusChanged(TorchStatus status, const String16& cameraId) {
		ALOGD("%s: onTorchStatusChanged: %s", __FUNCTION__, status);
        Mutex::Autolock l(mLock);
        mCameraTorchStatuses[cameraId] = status;
        mTorchCondition.broadcast();
    };

    bool waitForNumCameras(size_t num) const {
        Mutex::Autolock l(mLock);

        if (mCameraStatuses.size() == num) {
            return true;
        }

        while (mCameraStatuses.size() < num) {
            if (mCondition.waitRelative(mLock, SETUP_TIMEOUT) != OK) {
                return false;
            }
        }
        return true;
    };

    bool waitForTorchState(TorchStatus status, int32_t cameraId) const {
        Mutex::Autolock l(mLock);

        const auto& iter = mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
        if (iter != mCameraTorchStatuses.end() && iter->second == status) {
            return true;
        }

        bool foundStatus = false;
        while (!foundStatus) {
            if (mTorchCondition.waitRelative(mLock, SETUP_TIMEOUT) != OK) {
                return false;
            }
            const auto& iter =
                    mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
            foundStatus = (iter != mCameraTorchStatuses.end() && iter->second == status);
        }
        return true;
    };

    TorchStatus getTorchStatus(int32_t cameraId) const {
        Mutex::Autolock l(mLock);
        const auto& iter = mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
        if (iter == mCameraTorchStatuses.end()) {
            return ICameraServiceListener::TORCH_STATUS_UNKNOWN;
        }
        return iter->second;
    };

    Status getStatus(int32_t cameraId) const {
        Mutex::Autolock l(mLock);
        const auto& iter = mCameraStatuses.find(cameraId);
        if (iter == mCameraStatuses.end()) {
            return ICameraServiceListener::STATUS_UNKNOWN;
        }
        return iter->second;
    };
};

//Device Callback implementation
class CameraDeviceCallbacks : public BnCameraDeviceCallbacks {
public:
    enum Status {
        IDLE,
        ERROR,
        PREPARED,
        RUNNING,
        SENT_RESULT,
        UNINITIALIZED
    };

protected:
    bool mError;
    Status mLastStatus;
    mutable std::vector<Status> mStatusesHit;
    mutable Mutex mLock;
    mutable Condition mStatusCondition;
public:
    CameraDeviceCallbacks() : mError(false), mLastStatus(UNINITIALIZED) {}

    virtual ~CameraDeviceCallbacks() {}

    virtual void onDeviceError(CameraErrorCode errorCode,
            const CaptureResultExtras& resultExtras) {
        ALOGE("%s: onDeviceError occurred with: %d", __FUNCTION__, static_cast<int>(errorCode));
        Mutex::Autolock l(mLock);
        mError = true;
        mLastStatus = ERROR;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    virtual void onDeviceIdle() {
		ALOGD("%s: onDeviceIdle", __FUNCTION__);
        Mutex::Autolock l(mLock);
        mLastStatus = IDLE;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    virtual void onCaptureStarted(const CaptureResultExtras& resultExtras,
            int64_t timestamp) {
		ALOGD("%s: onCaptureStarted", __FUNCTION__);
        Mutex::Autolock l(mLock);
        mLastStatus = RUNNING;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }


    virtual void onResultReceived(const CameraMetadata& metadata,
            const CaptureResultExtras& resultExtras) {
		ALOGD("%s: onResultReceived", __FUNCTION__);
        Mutex::Autolock l(mLock);
        mLastStatus = SENT_RESULT;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    virtual void onPrepared(int streamId) {
		ALOGD("%s: onPrepared, streamId: %d", __FUNCTION__, streamId);
        Mutex::Autolock l(mLock);
        mLastStatus = PREPARED;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    bool waitForStatus(Status status) const {
        Mutex::Autolock l(mLock);
        if (mLastStatus == status) {
            return true;
        }

        while (std::find(mStatusesHit.begin(), mStatusesHit.end(), status)
                == mStatusesHit.end()) {

            if (mStatusCondition.waitRelative(mLock, IDLE_TIMEOUT) != OK) {
                mStatusesHit.clear();
                return false;
            }
        }
        mStatusesHit.clear();

        return true;

    }

    void clearStatus() const {
        Mutex::Autolock l(mLock);
        mStatusesHit.clear();
    }

    bool waitForIdle() const {
        return waitForStatus(IDLE);
    }

};

int main(int argc, char **argv) {
	
	ProcessState::self()->startThreadPool();
	sp<IServiceManager> sm = defaultServiceManager();
	sp<IBinder> binder = sm->getService(String16("media.camera"));
	if (binder == NULL) {
		return -1;
	}
	//ASSERT_NOT_NULL(binder);
	sp<ICameraService> service = interface_cast<ICameraService>(binder);

	int32_t numCameras = service->getNumberOfCameras();

	sp<CameraServiceListener> listener(new CameraServiceListener());
	service->addListener(listener);

	if (!listener->waitForNumCameras(numCameras)) {
		return -1;
	}

	ALOGE("Stage 1 -------------- camera number:%d", numCameras);
	for (int32_t i = 0; i < numCameras; i++) {
		status_t camera2Support = service->supportsCameraApi(i, ICameraService::API_VERSION_2);

		if (camera2Support != OK) {
			continue;
		}

		ALOGE("Stage 2");
		ICameraServiceListener::Status s = listener->getStatus(i);
		if (ICameraServiceListener::STATUS_AVAILABLE != s) {
			continue;
		}

		ALOGE("Stage 3");
		sp<CameraDeviceCallbacks> callbacks(new CameraDeviceCallbacks());
		sp<ICameraDeviceUser> device;
		if (OK != service->connectDevice(callbacks, i, String16("test.3streams"), ICameraService::USE_CALLING_UID, /*out*/device)) {
			continue;
		}

		if (device.get() == NULL) {
			continue;
		}

		int video_fd = open("/data/local/tmp/camera_recording_test.mp4", O_RDWR|O_CREAT, S_IRUSR|S_IWUSR);
		if (video_fd < 0) {
			ALOGE("open video file failed!");
			return -1;
		}
		sp<MediaRecorder> mediarecorder(new MediaRecorder(String16("MultiStreams-recording-test")));
		mediarecorder->setVideoSource(VIDEO_SOURCE_SURFACE);
		mediarecorder->setAudioSource(1);	// MIC
		mediarecorder->setOutputFormat(OUTPUT_FORMAT_MPEG_4);
		mediarecorder->setOutputFile(video_fd, 0, 0);
		mediarecorder->setParameters(String8("video-param-encoding-bitrate=8000000"));	// bitrate 8M
		mediarecorder->setVideoFrameRate(30);
		/* config recording stream size here */
		mediarecorder->setVideoSize(WIDTH, HEIGHT);
		mediarecorder->setVideoEncoder(VIDEO_ENCODER_H264);
		mediarecorder->setAudioEncoder(AUDIO_ENCODER_AAC);
		if (OK != mediarecorder->prepare()) {
			ALOGE("MediaRecorder failed prepare!");
			return -1;
		}
		/* get surface from MediaRecorder object */
		sp<IGraphicBufferProducer> bufferProducer = mediarecorder->querySurfaceMediaSourceFromMediaServer();
		sp<Surface> surface(new Surface(bufferProducer, true));

		OutputConfiguration output(bufferProducer, /*rotation*/0);
		
		ALOGE("Stage 4");
		device->beginConfigure();
		/* create streams and allocation */
		status_t streamId = device->createStream(output);
		device->endConfigure();

		ALOGE("Stage 5");
		/* use recording template */
		CameraMetadata requestTemplate;
		device->createDefaultRequest(/*recording template*/3, /*out*/&requestTemplate);
		sp<CaptureRequest> request(new CaptureRequest());
		request->mMetadata = requestTemplate;
		request->mSurfaceList.add(surface);
		request->mIsReprocess = false;
		int64_t lastFrameNumber = 0;
		callbacks->clearStatus();
		ALOGE("Stage 6");
		/* submit streaming request */
		int requestId = device->submitRequest(request, /*streaming*/true, /*out*/&lastFrameNumber);
		callbacks->waitForStatus(CameraDeviceCallbacks::SENT_RESULT);
		
		/* start recording */
		if (OK != mediarecorder->start()) {
			return -1;
		}
		/* record duration 5s */
		sleep(5);
		/* stop and release recording */
		mediarecorder->stop();
		mediarecorder->reset();
		close(video_fd);
		/* cancel request and release device resources */
		if (OK != device->cancelRequest(requestId, /*out*/&lastFrameNumber)) {
			ALOGE("Failed cancel request!");
		}

		mediarecorder->release();
		device->disconnect();
		break;
	}

	return 0;
}

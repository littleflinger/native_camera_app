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

#include <gui/CpuConsumer.h>
#include <gui/BufferItemConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>

#include <unistd.h>
#include <stdint.h>
#include <utility>
#include <vector>
#include <map>

using namespace android;

#define SETUP_TIMEOUT 2000000000 // ns
#define IDLE_TIMEOUT 2000000000 // ns

#define WIDTH 1920
#define HEIGHT 1080
#define MAX_IMAGES 2
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

struct FrameListener : public ConsumerBase::FrameAvailableListener {

	FrameListener(int maxImages, sp<CpuConsumer> consumer) {
		mPendingFrames = 0;
		for (int i = 0; i < maxImages; i++) {
			CpuConsumer::LockedBuffer *buffer = new CpuConsumer::LockedBuffer;
			mBuffers.push_back(buffer);
		}
		mConsumer = consumer;
	}   

	// CpuConsumer::FrameAvailableListener implementation
	virtual void onFrameAvailable(const BufferItem& /* item */) {
		ALOGD("Frame now available (start)");

		Mutex::Autolock lock(mMutex);
		
		CpuConsumer::LockedBuffer* buffer = getLockedBuffer();
		if (buffer == NULL) {
			return;
		}
		ALOGD("start lock buffer");
		status_t res = mConsumer->lockNextBuffer(buffer);

		/* print buffer info */
		dumpLockedBuffer(buffer);
		/* now you can access buffer here 
		*
		*
		*
		*/

		ALOGD("start unlock buffer");
		mConsumer->unlockBuffer(*buffer);
		returnLockedBuffer(buffer);
		
		mPendingFrames++;
		mCondition.signal();

		ALOGD("Frame now available (end)");
	}   

	status_t waitForFrame(nsecs_t timeout) {
		status_t res;
		Mutex::Autolock lock(mMutex);
		while (mPendingFrames == 0) {
			res = mCondition.waitRelative(mMutex, timeout);
			if (res != OK) return res;
		}   
		mPendingFrames--;
		return OK; 
	} 

	CpuConsumer::LockedBuffer* getLockedBuffer() {
		if (mBuffers.empty()) {
			return NULL;
		}

		List<CpuConsumer::LockedBuffer*>::iterator it = mBuffers.begin();
		CpuConsumer::LockedBuffer* buffer = *it;
		mBuffers.erase(it);
		return buffer;
	}

	void returnLockedBuffer(CpuConsumer::LockedBuffer* buffer) {
		mBuffers.push_back(buffer);
	}

	void dumpLockedBuffer(CpuConsumer::LockedBuffer* buffer) {
		if (buffer == NULL) {
			ALOGE("Invalid buffer!");
			return;
		}

		ALOGD("print buffer info (start)");
		ALOGD("width:%d, height:%d, format:0x%x, stride:%d, frameNumber:%d, flexFormat:0x%x", buffer->width, buffer->height, buffer->format, buffer->stride, buffer->frameNumber, buffer->flexFormat);
		ALOGD("print buffer info (end)");
	}

	void DumpJpegToFile(const String8 &fileName, const CpuConsumer::LockedBuffer &img) {
		//TODO: save to jpeg file.
	}
	void DumpYuvToFile(const String8 &fileName, const CpuConsumer::LockedBuffer &img) {
		uint8_t *dataCb, *dataCr;
		uint32_t stride;
		uint32_t chromaStride;
		uint32_t chromaStep;

		switch (img.format) {
			case HAL_PIXEL_FORMAT_YCbCr_420_888:
				stride = img.stride;
				chromaStride = img.chromaStride;
				chromaStep = img.chromaStep;
				dataCb = img.dataCb;
				dataCr = img.dataCr;
				break;
			case HAL_PIXEL_FORMAT_YCrCb_420_SP:
				stride = img.width;
				chromaStride = img.width;
				chromaStep = 2;
				dataCr = img.data + img.width * img.height;
				dataCb = dataCr + 1;
				break;
			case HAL_PIXEL_FORMAT_YV12:
				stride = img.stride;
				chromaStride = ALIGN(img.width / 2, 16);
				chromaStep = 1;
				dataCr = img.data + img.stride * img.height;
				dataCb = dataCr + chromaStride * img.height/2;
				break;
			default:
				ALOGE("Unknown format %d, not dumping", img.format);
				return;
		}

		// Write Y
		FILE *yuvFile = fopen(fileName.string(), "w");

		size_t bytes;

		for (size_t y = 0; y < img.height; ++y) {
			bytes = fwrite(
					reinterpret_cast<const char*>(img.data + stride * y),
					1, img.width, yuvFile);
			if (bytes != img.width) {
				ALOGE("Unable to write to file %s", fileName.string());
				fclose(yuvFile);
				return;
			}
		}

		// Write Cb/Cr
		uint8_t *src = dataCb;
		for (int c = 0; c < 2; ++c) {
			for (size_t y = 0; y < img.height / 2; ++y) {
				uint8_t *px = src + y * chromaStride;
				if (chromaStep != 1) {
					for (size_t x = 0; x < img.width / 2; ++x) {
						fputc(*px, yuvFile);
						px += chromaStep;
					}
				} else {
					bytes = fwrite(reinterpret_cast<const char*>(px),
							1, img.width / 2, yuvFile);
					if (bytes != img.width / 2) {
						ALOGE("Unable to write to file %s", fileName.string());
						fclose(yuvFile);
						return;
					}
				}
			}
			src = dataCr;
		}
		fclose(yuvFile);
	}

	private:
	Mutex mMutex;
	Condition mCondition;
	int mPendingFrames;
	sp<CpuConsumer> mConsumer;
	List<CpuConsumer::LockedBuffer*> mBuffers;
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
			ALOGE("connect failed!");
			continue;
		}
		if (device.get() == NULL) {
			ALOGE("Invalid device!");
			continue;
		}

		sp<IGraphicBufferProducer> gbProducer;
		sp<IGraphicBufferConsumer> gbConsumer;
		BufferQueue::createBufferQueue(&gbProducer, &gbConsumer);
		
		String8 consumerName = String8::format("ImageReader-%dx%d-format:0x%x-maxImages:%d-pid:%d", WIDTH, HEIGHT, HAL_PIXEL_FORMAT_BLOB, MAX_IMAGES, getpid());
		sp<CpuConsumer> cpuConsumer = new CpuConsumer(gbConsumer, MAX_IMAGES, /*controlledByApp*/true);
		cpuConsumer->setName(consumerName);
		sp<FrameListener> mFrameListener = new FrameListener(MAX_IMAGES, cpuConsumer);
		if (cpuConsumer != 0) {
			cpuConsumer->setFrameAvailableListener(mFrameListener);
		}

		/* config image size */
		gbConsumer->setDefaultBufferSize(WIDTH, HEIGHT);
		/* config image format - JPEG */
		gbConsumer->setDefaultBufferFormat(HAL_PIXEL_FORMAT_BLOB);
		sp<Surface> surface(new Surface(gbProducer, /*controlledByApp*/true));

		OutputConfiguration output(gbProducer, /*rotation*/0);
		
		ALOGE("Stage 4");
		device->beginConfigure();
		/* create streams and allocation */
		status_t streamId = device->createStream(output);
		device->endConfigure();

		ALOGE("Stage 5");
		/* use still capture template to create capture request */
		CameraMetadata requestTemplate;
		device->createDefaultRequest(/*still capture template*/2, /*out*/&requestTemplate);
		sp<CaptureRequest> request(new CaptureRequest());
		request->mMetadata = requestTemplate;
		request->mSurfaceList.add(surface);
		request->mIsReprocess = false;
		int64_t lastFrameNumber = 0;
		callbacks->clearStatus();
		ALOGE("Stage 6");
		/* submit still capture request, streaming is off */
		int requestId = device->submitRequest(request, /*streaming*/false, /*out*/&lastFrameNumber);
		callbacks->waitForStatus(CameraDeviceCallbacks::SENT_RESULT);
		callbacks->waitForIdle();

		if (OK != device->cancelRequest(requestId, /*out*/&lastFrameNumber)) {
			ALOGE("Failed cancel request!");
		}
		device->disconnect();
		break;
	}

	return 0;
}

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
#include <utils/RefBase.h>
#include <system/graphics.h>
#include <hardware/gralloc.h>

#include <camera/CameraMetadata.h>
#include <camera/ICameraService.h>
#include <camera/ICameraServiceListener.h>
#include <camera/camera2/CaptureRequest.h>
#include <camera/camera2/ICameraDeviceUser.h>
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/OutputConfiguration.h>

#include <media/ICrypto.h>
#include <media/mediarecorder.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <media/stagefright/SurfaceMediaSource.h>
#include <media/stagefright/Utils.h>
#include <OMX_IVCommon.h>
#include <OMX_Video.h>

#include <gui/CpuConsumer.h>
#include <gui/BufferItemConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>

#include <sys/time.h>
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

enum {kWhatCallbackNotify};

class CodecHandler: public AHandler {
private:
	sp<MediaCodec> mCodec;
	void handleCallback(const sp<AMessage> &msg);
	int32_t mDuration;
	int64_t mLastTime, mCurrentTime;
public:
	CodecHandler(sp<MediaCodec> codec);
	virtual void onMessageReceived(const sp<AMessage> &msg);
};

CodecHandler::CodecHandler(sp<MediaCodec> codec) {
	mCodec = codec;
	mDuration = 0;
	mLastTime = 0;
	mCurrentTime = 0;
}

void CodecHandler::onMessageReceived(const sp<AMessage> &msg) {
	switch (msg->what()) {
		case kWhatCallbackNotify:
		{
			ALOGD("onMessageReceived::kWhatCallbackNotify");
			handleCallback(msg);
			break;
		}
		default:
			break;
	}
}

void CodecHandler::handleCallback(const sp<AMessage> &msg) {
	int32_t arg1, arg2 = 0;
	ALOGD("handleCallback");
	CHECK(msg->findInt32("callbackID", &arg1));
	switch (arg1) {
		case MediaCodec::CB_INPUT_AVAILABLE:
		{
			ALOGD("handleCallback::CB_INPUT_AVAILABLE");
			CHECK(msg->findInt32("index", &arg2));
			break;
		}
		case MediaCodec::CB_OUTPUT_AVAILABLE:
		{
			ALOGD("handleCallback::CB_OUTPUT_AVAILABLE");

			int32_t index;
			size_t size, offset;
			int64_t timeUs;
			uint32_t flags;
			CHECK(msg->findInt32("index", &index));
			CHECK(msg->findSize("size", &size));
			CHECK(msg->findSize("offset", &offset));
			CHECK(msg->findInt64("timeUs", &timeUs));
			CHECK(msg->findInt32("flags", (int32_t *)&flags));

			ALOGD("handleCallback::CB_OUTPUT_AVAILABLE::index[%d], size[%ld], offset[%ld]", index, size, offset);

			sp<ABuffer> mEncoderOutputBuffers;
			status_t err = mCodec->getOutputBuffer(index, &mEncoderOutputBuffers);
			if (err != OK) {
				ALOGE("handleCallback::CB_OUTPUT_AVAILABLE::ERROR::getOutputBuffer");
				return;
			}
/*		
			ALOGD("handleCallback::CB_OUTPUT_AVAILABLE::capacity[%ld], rangeOffset[%ld], rangeLength[%ld]", mEncoderOutputBuffers->capacity(), mEncoderOutputBuffers->offset(), mEncoderOutputBuffers->size());
			MediaBuffer *mbuf = new MediaBuffer(mEncoderOutputBuffers->size());
			memcpy(mbuf->data(), mEncoderOutputBuffers->data(), mEncoderOutputBuffers->size());

			if (flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) {
				mbuf->meta_data()->setInt32(kKeyIsCodecConfig, true);
			}
			if (flags & MediaCodec::BUFFER_FLAG_SYNCFRAME) {
				mbuf->meta_data()->setInt32(kKeyIsSyncFrame, true);
			}
#ifdef MTK_AOSP_ENHANCEMENT
            if (flags & MediaCodec::BUFFER_FLAG_MULTISLICE) {
                mbuf->meta_data()->setInt32(KKeyMultiSliceBS, true);
            }
#endif
*/			
			err = mCodec->releaseOutputBuffer(index);
			if (err != OK) {
				ALOGE("handleCallback::CB_OUTPUT_AVAILABLE::ERROR::releaseOutputBuffer");
				return;
			}

			struct timeval tv;
			gettimeofday(&tv, NULL);
			mCurrentTime = (int64_t)tv.tv_sec*1000 + (int64_t)tv.tv_usec/1000;
			if (mLastTime == 0) {
				mLastTime = mCurrentTime;
				break;
			}
			mDuration = mCurrentTime - mLastTime;
			mLastTime = mCurrentTime;
			ALOGD("handleCallback::CB_OUTPUT_AVAILABLE::duration = %d", mDuration);

			break;
		}
		case MediaCodec::CB_ERROR:
		{
			ALOGD("handleCallback::CB_ERROR");
			int32_t err, actionCode;
			CHECK(msg->findInt32("err", &err));
			CHECK(msg->findInt32("actionCode", &actionCode));

			break;
		}
		case MediaCodec::CB_OUTPUT_FORMAT_CHANGED:
		{
			ALOGD("handleCallback::CB_OUTPUT_FORMAT_CHANGED");
			sp<AMessage> format;
			CHECK(msg->findMessage("format", &format));

			break;
		}
		default:
			break;
	}
}
struct TMediaCodec: public RefBase {

	TMediaCodec (const char *name, bool nameIsType, bool encoder);

	status_t setCallback();
	status_t configure(const sp<AMessage> &format, const sp<IGraphicBufferProducer> &bufferProducer, int flags);
	status_t createInputSurface(sp<IGraphicBufferProducer>* bufferProducer);
	status_t start();
	status_t stop();
	status_t reset();
	status_t flush();
	status_t queueInputBuffer(size_t index, size_t offset, size_t size, int64_t timeUs, uint32_t flags, AString *errorDetailMsg);

	status_t dequeueInputBuffer(size_t *index, int64_t timeoutUs);
	status_t dequeueOutputBuffer(size_t *index, int64_t timeoutUs);
	status_t releaseOutputBuffer(size_t index);
	status_t signalEndOfInputStream();
	status_t getOutputFormat(size_t index, sp<AMessage> *format);
	status_t getBuffer(bool input, size_t index, sp<ABuffer> *buf);

protected:
	virtual ~TMediaCodec();

private:

	sp<CodecHandler> mHandler;
	sp<ALooper> mLooper;
	sp<ALooper> mStreamingLooper;
	sp<MediaCodec> mCodec;
	sp<Surface> mSurfaceTextureClient;

	sp<AMessage> mCallbackNotification;
	status_t mInitStatus;

	DISALLOW_EVIL_CONSTRUCTORS(TMediaCodec);
};

enum {
	EVENT_CALLBACK = 1,
	EVENT_SET_CALLBACK = 2,
};

TMediaCodec::TMediaCodec (const char *name, bool nameIsType, bool encoder) {
	mLooper = new ALooper;
	mLooper->setName("codec_looper");
	//mLooper->start(
	//	false,      // runOnCallingThread
	//	false,      // canCallJava
	//	PRIORITY_FOREGROUND);
	mLooper->start();

	mStreamingLooper = new ALooper;
	mStreamingLooper->setName("streaming_looper");
	mStreamingLooper->start();

	if (nameIsType) {
		mCodec = MediaCodec::CreateByType(mLooper, name, encoder, &mInitStatus);
	} else {
		mCodec = MediaCodec::CreateByComponentName(mLooper, name, &mInitStatus);
	}
	CHECK((mCodec != NULL) != (mInitStatus != OK));
	
	mHandler = new CodecHandler(mCodec);
	mStreamingLooper->registerHandler(mHandler);
}

TMediaCodec::~TMediaCodec () {
	if (mCodec != NULL) {
		mCodec->release();
		mCodec.clear();
		mInitStatus = NO_INIT;
	}
	if (mLooper != NULL) {
		if(mHandler != NULL) {
			mLooper->unregisterHandler(mHandler->id());
		}
		mLooper->stop();
		mLooper.clear();
	}
}

status_t TMediaCodec::setCallback() {
	if (mCallbackNotification == NULL) {
		mCallbackNotification = new AMessage(kWhatCallbackNotify, mHandler);
	}

	return mCodec->setCallback(mCallbackNotification);
}

status_t TMediaCodec::configure(const sp<AMessage> &format, const sp<IGraphicBufferProducer> &bufferProducer, int flags) {
	if (bufferProducer != NULL) {
		mSurfaceTextureClient = new Surface(bufferProducer, true /* controlledByApp */);
	} else {
		mSurfaceTextureClient.clear();
	}

	return mCodec->configure(format, mSurfaceTextureClient, NULL, flags);
}

status_t TMediaCodec::createInputSurface(sp<IGraphicBufferProducer>* bufferProducer) {
	return mCodec->createInputSurface(bufferProducer);
}

status_t TMediaCodec::start() {
	return mCodec->start();
}

status_t TMediaCodec::stop() {
	mSurfaceTextureClient.clear();

	return mCodec->stop();
}

status_t TMediaCodec::flush() {
	return mCodec->flush();
}

status_t TMediaCodec::reset() {
	return mCodec->reset();
}

status_t TMediaCodec::queueInputBuffer(size_t index, size_t offset, size_t size, int64_t timeUs, uint32_t flags, AString *errorDetailMsg) {
	return mCodec->queueInputBuffer(index, offset, size, timeUs, flags, errorDetailMsg);
}

status_t TMediaCodec::dequeueInputBuffer(size_t *index, int64_t timeoutUs) {
	return mCodec->dequeueInputBuffer(index, timeoutUs);
}

status_t TMediaCodec::dequeueOutputBuffer(size_t *index, int64_t timeoutUs) {
	size_t size, offset;
	int64_t timeUs;
	uint32_t flags;
	status_t err = mCodec->dequeueOutputBuffer(index, &offset, &size, &timeUs, &flags, timeoutUs);

	if (err != OK) {
		return err;
	}
	return OK;
}

status_t TMediaCodec::releaseOutputBuffer(size_t index) {
	return mCodec->releaseOutputBuffer(index);
}

status_t TMediaCodec::signalEndOfInputStream() {
	return  mCodec->signalEndOfInputStream();
}

status_t TMediaCodec::getOutputFormat(size_t index, sp<AMessage> *format) {
	return mCodec->getOutputFormat(index, format);
}

status_t TMediaCodec::getBuffer(bool input, size_t index, sp<ABuffer> *buf) {
	
	status_t err = input? mCodec->getInputBuffer(index, buf) : mCodec->getOutputBuffer(index, buf);

	return err;
}
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

		ALOGE("Stage 3.1");
		sp<AMessage> format = new AMessage;
		format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
		format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
		format->setInt32("width", 1280);
		format->setInt32("height", 720);
		format->setInt32("bitrate", 8000000);
		format->setInt32("frame-rate", 30);
		format->setInt32("i-frame-interval", 1);
		
		ALOGE("Stage 3.2");
		sp<TMediaCodec> encoder = new TMediaCodec(MEDIA_MIMETYPE_VIDEO_AVC, true, true);

		ALOGE("Stage 3.3");
		encoder->setCallback();

		ALOGE("Stage 3.4");
		encoder->configure(format, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);

		ALOGE("Stage 3.5");
		sp<IGraphicBufferProducer> mGraphicBufferProducer;
		encoder->createInputSurface(&mGraphicBufferProducer);

		//ALOGE("Stage 3.6");
		//encoder->start();

/*
		ALOGE("Stage 3.1");
		sp<ABuffer> mEncoderInputBuffers;
		sp<ABuffer> mEncoderOutputBuffers;
		sp<ALooper> codecLooper = new ALooper;
		codecLooper->setName("codec_looper");
		codecLooper->start(false, false, PRIORITY_FOREGROUND);
		
		sp<MediaCodec> mEncoder = MediaCodec::CreateByType(codecLooper, "video/avc", true);

		ALOGE("Stage 3.2");
		sp<AMessage> format = new AMessage;
		format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
		format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
		format->setInt32("width", 1280);
		format->setInt32("height", 720);
		format->setInt32("bitrate", 4000000);
		format->setInt32("frame-rate", 30);
		format->setInt32("i-frame-interval", 1);
		
		ALOGE("Stage 3.4");
		status_t err = NO_INIT;
		err = mEncoder->configure(format, NULL, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);
		if (err != OK) {
			return err;
		}

		ALOGE("Stage 3.5");
		sp<IGraphicBufferProducer> mGraphicBufferProducer;
		err = mEncoder->createInputSurface(&mGraphicBufferProducer);
		if (err != OK) {
			return err;
		}
*/
		ALOGE("Stage 3.6");

		sp<Surface> surface(new Surface(mGraphicBufferProducer, true));

		OutputConfiguration output(mGraphicBufferProducer, /*rotation*/0);
		
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
		//callbacks->waitForStatus(CameraDeviceCallbacks::SENT_RESULT);
	
		sleep(1);

		ALOGE("Stage start");
		encoder->start();
/*		
		ALOGE("Stage 7");
		err = mEncoder->start();
		if (err != OK) {
			return err;
		}
		int64_t timeUs = 0;
		for (;;) {
			for (;;) {
				size_t inputBufferIndex;
				err = mEncoder->dequeueInputBuffer(&inputBufferIndex, timeUs);
				ALOGD("dequeueInputBuffer");
				if (err != OK) {
					ALOGE("getInputBuffer failed!");
					break;
				}
				if (inputBufferIndex > 0) {
					ALOGD("getInputBuffer");
					err = mEncoder->getInputBuffer(inputBufferIndex, &mEncoderInputBuffers);
				}
			}

			for (;;) {
				size_t outputBufferIndex;
				size_t offset;
				size_t size;
				int64_t timeUs;
				uint32_t flags;
				err = mEncoder->dequeueOutputBuffer(&outputBufferIndex, &offset, &size, &timeUs, &flags);

				ALOGD("dequeueOutputBuffer1");
				if (err != OK) {
					if (err == INFO_FORMAT_CHANGED) {
						ALOGE("INFO_FORMAT_CHANGED");
						continue;
					} else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
						ALOGE("INFO_OUTPUT_BUFFERS_CHANGED");
						mEncoder->getOutputBuffer(outputBufferIndex, &mEncoderOutputBuffers);
						continue;
					}

					if (err == -EAGAIN) {
						ALOGE("EAGAIN");
						err = OK;
					}
					break;
				}
				ALOGD("dequeueOutputBuffer2");

				if (outputBufferIndex > 0) {
					ALOGD("------------------------------------------onOutputBufferAvailable:index[%ld] offset[%ld] size[%ld] timeUs[%ld] flags[%ld]", outputBufferIndex, offset, size, timeUs, flags);
					err = mEncoder->getOutputBuffer(outputBufferIndex, &mEncoderOutputBuffers);
					if (err != OK) {
						return err;
					}
					native_handle_t* handle = NULL;
					sp<ABuffer> outbuf = mEncoderOutputBuffers;
					if (outbuf->meta()->findPointer("handle", (void**)&handle) && handle != NULL) {
						int32_t rangeLength, rangeOffset;
						outbuf->meta()->findInt32("rangeOffset", &rangeOffset);
						outbuf->meta()->findInt32("rangeLength", &rangeLength);
						outbuf->meta()->setPointer("handle", NULL);

						ALOGD("rangeOffset:%ld, rangeLength:%ld", rangeOffset, rangeLength);

						if (!handle) {
							mEncoder->releaseOutputBuffer(outputBufferIndex);
						}

					}
				}
			}

		}
*/
		/* record duration 5s */
		//sleep(60);
		while(1);
		/* stop and release recording */
		
		/* cancel request and release device resources */
		if (OK != device->cancelRequest(requestId, /*out*/&lastFrameNumber)) {
			ALOGE("Failed cancel request!");
		}

		device->disconnect();
		break;
	}

	return 0;
}

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
		
		// print buffer info
		dumpLockedBuffer(buffer);
		
		/* you can access buffer here */

		// dump to yuv file verify
/*
		String8 dumpName = String8::format("/data/local/tmp/camera2_test_variable_frame_%ld.yuv", buffer->frameNumber);
		DumpYuvToFile(dumpName, *buffer);
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
			continue;
		}
		if (device.get() == NULL) {
			continue;
		}

		ALOGE("Stage 4 ------------------------------ Stream1 initiate");
		/* Stream1 initiate */
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
		sp<IGraphicBufferProducer> recordingGbProducer = mediarecorder->querySurfaceMediaSourceFromMediaServer();

		ALOGE("Stage 5 ------------------------------ Stream2 initiate");
		/* Stream2 initiate */
		sp<AMessage> format = new AMessage;
		format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
		format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
		format->setInt32("width", 1280);
		format->setInt32("height", 720);
		format->setInt32("bitrate", 8000000);
		format->setInt32("frame-rate", 30);
		format->setInt32("i-frame-interval", 1);
		
		sp<TMediaCodec> encoder = new TMediaCodec(MEDIA_MIMETYPE_VIDEO_AVC, true, true);

		encoder->setCallback();

		encoder->configure(format, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);

		sp<IGraphicBufferProducer> widiGbProducer;
		encoder->createInputSurface(&widiGbProducer);

		ALOGE("Stage 6 ------------------------------ Stream3 initiate");
		/* Stream3 initiate */
		sp<IGraphicBufferProducer> streamingGbProducer;
		sp<IGraphicBufferConsumer> streamingGbConsumer;
		BufferQueue::createBufferQueue(&streamingGbProducer, &streamingGbConsumer);
		
		String8 consumerName = String8::format("ImageReader-%dx%d-format:0x%x-maxImages:%d-pid:%d", WIDTH, HEIGHT, HAL_PIXEL_FORMAT_YV12, MAX_IMAGES, getpid());
		sp<CpuConsumer> streamingCpuConsumer = new CpuConsumer(streamingGbConsumer, MAX_IMAGES, /*controlledByApp*/true);
		streamingCpuConsumer->setName(consumerName);
		sp<FrameListener> mFrameListener = new FrameListener(MAX_IMAGES, streamingCpuConsumer);
		if (streamingCpuConsumer != 0) {
			streamingCpuConsumer->setFrameAvailableListener(mFrameListener);
		}

		/* config stream size */
		streamingGbConsumer->setDefaultBufferSize(WIDTH, HEIGHT);
		/* config stream format */
		streamingGbConsumer->setDefaultBufferFormat(HAL_PIXEL_FORMAT_YV12);

		ALOGE("Stage 7 ------------------------------ create 3 Surfaces");
		/* create 3 Surfaces according to 3 streams */
		sp<Surface> recordingSurface(new Surface(recordingGbProducer, /*controlledByApp*/true));
		sp<Surface> widiSurface(new Surface(widiGbProducer, /*controlledByApp*/true));
		sp<Surface> streamingSurface(new Surface(streamingGbProducer, /*controlledByApp*/true));

		OutputConfiguration output1(recordingGbProducer, /*rotation*/0);
		OutputConfiguration output2(widiGbProducer, /*rotation*/0);
		OutputConfiguration output3(streamingGbProducer, /*rotation*/0);
		
		ALOGE("Stage 8 ------------------------------ create 3 streams");
		device->beginConfigure();
		/* create 3 streams and allocation */
		status_t streamId1 = device->createStream(output1);
		status_t streamId2 = device->createStream(output2);
		status_t streamId3 = device->createStream(output3);
		device->endConfigure();

		ALOGE("Stage 9 ------------------------------ ready to submit request with 3 streams");
		/* use record template to create an new request */
		CameraMetadata requestTemplate;
		device->createDefaultRequest(/*recording template*/3, /*out*/&requestTemplate);
		sp<CaptureRequest> request(new CaptureRequest());
		request->mMetadata = requestTemplate;
		request->mSurfaceList.add(recordingSurface);
		request->mSurfaceList.add(widiSurface);
		request->mSurfaceList.add(streamingSurface);
		request->mIsReprocess = false;
		int64_t lastFrameNumber = 0;
		callbacks->clearStatus();
		ALOGE("Stage 10 ------------------------------ submitting");
		/* submit streaming request, streaming is on */
		int requestId = device->submitRequest(request, /*streaming*/true, /*out*/&lastFrameNumber);
		//callbacks->waitForStatus(CameraDeviceCallbacks::SENT_RESULT);
		//callbacks->waitForIdle();

		ALOGE("Stage 11 ------------------------------ start recording");
		/* start recording */
		if (OK != mediarecorder->start()) {
			return -1;
		}

		ALOGE("Stage 12 ------------------------------ start wifi display");
		/* start wifi display */
		encoder->start();

		ALOGE("Stage 13");
		while(1);
		ALOGE("Stage 14");

		/* stop and release recording */
		mediarecorder->stop();
		mediarecorder->reset();
		close(video_fd);
		if (OK != device->cancelRequest(requestId, /*out*/&lastFrameNumber)) {
			ALOGE("Failed cancel request!");
		}
		device->disconnect();
		break;
	}

	return 0;
}

#include <obs-module.h>
#include <util/profiler.hpp>

#include "mf-common.hpp"
#include "mf-av1-encoder.hpp"

#include <codecapi.h>
#include <mferror.h>

using namespace MF;

static eAVEncAV1VProfile MapProfile(AV1Profile profile)
{
	switch (profile) {
	case AV1ProfileMain:
		return eAVEncAV1VProfile_Main_420_8;
	default:
		return eAVEncAV1VProfile_Main_420_8;
	}
}

static eAVEncCommonRateControlMode MapRateControl(AV1RateControl rc)
{
	switch (rc) {
	case AV1RateControlCBR:
		return eAVEncCommonRateControlMode_CBR;
	case AV1RateControlVBR:
		return eAVEncCommonRateControlMode_UnconstrainedVBR;
	default:
		return eAVEncCommonRateControlMode_CBR;
	}
}

static UINT32 MapQpToQuality(AV1QP &qp)
{
	return 100 - (UINT32)floor(100.0 / 51.0 * qp.defaultQp + 0.5f);
}

static bool ProcessNV12(std::function<void(UINT32 height, INT32 plane)> func,
	UINT32 height)
{
	INT32 plane = 0;

	func(height, plane++);
	func(height / 2, plane);

	return true;
}

AV1Encoder::AV1Encoder(const obs_encoder_t *encoder,
	std::shared_ptr<EncoderDescriptor> descriptor,
	UINT32 width,
	UINT32 height,
	UINT32 framerateNum,
	UINT32 framerateDen,
	AV1Profile profile,
	UINT32 bitrate)
	: encoder(encoder),
	descriptor(descriptor),
	width(width),
	height(height),
	framerateNum(framerateNum),
	framerateDen(framerateDen),
	initialBitrate(bitrate),
	profile(profile)
{}

AV1Encoder::~AV1Encoder()
{
	HRESULT hr;

	if (!descriptor->Async() || !eventGenerator || !pendingRequests)
		return;

	// Make sure all events have finished before releasing, and drain
	// all output requests until it makes an input request.
	// If you do not do this, you risk it releasing while there's still
	// encoder activity, which can cause a crash with certain interfaces.
	while (inputRequests == 0) {
		hr = ProcessOutput();
		if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr)) {
			MF_LOG_COM(LOG_ERROR, "AV1Encoder::~AV1Encoder: "
					"ProcessOutput()", hr);
			break;
		}

		if (inputRequests == 0)
			Sleep(1);
	}
}

HRESULT AV1Encoder::CreateMediaTypes(ComPtr<IMFMediaType> &i,
		ComPtr<IMFMediaType> &o)
{
	HRESULT hr;
	HRC(MFCreateMediaType(&i));
	HRC(MFCreateMediaType(&o));

	HRC(i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	HRC(i->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
	HRC(MFSetAttributeSize(i, MF_MT_FRAME_SIZE, width, height));
	HRC(MFSetAttributeRatio(i, MF_MT_FRAME_RATE, framerateNum,
			framerateDen));
	HRC(i->SetUINT32(MF_MT_INTERLACE_MODE,
			MFVideoInterlaceMode::MFVideoInterlace_Progressive));
	HRC(MFSetAttributeRatio(i, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

	HRC(o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	HRC(o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AV1));
	HRC(MFSetAttributeSize(o, MF_MT_FRAME_SIZE, width, height));
	HRC(MFSetAttributeRatio(o, MF_MT_FRAME_RATE, framerateNum,
			framerateDen));
	HRC(o->SetUINT32(MF_MT_AVG_BITRATE, initialBitrate * 1000));
	HRC(o->SetUINT32(MF_MT_INTERLACE_MODE,
			MFVideoInterlaceMode::MFVideoInterlace_Progressive));
	HRC(MFSetAttributeRatio(o, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
	HRC(o->SetUINT32(MF_MT_MPEG2_LEVEL, (UINT32)-1));
	//HRC(o->SetUINT32(MF_MT_MPEG2_PROFILE, MapProfile(profile)));

	return S_OK;

fail:
	return hr;
}

HRESULT AV1Encoder::DrainEvents()
{
	HRESULT hr;
	while ((hr = DrainEvent(false)) == S_OK);
	if (hr == MF_E_NO_EVENTS_AVAILABLE)
		hr = S_OK;
	return hr;
}

HRESULT AV1Encoder::DrainEvent(bool block)
{
	HRESULT hr, eventStatus;
	ComPtr<IMFMediaEvent> event;
	MediaEventType type;

	hr = eventGenerator->GetEvent(
		block ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);

	if (hr != MF_E_NO_EVENTS_AVAILABLE && FAILED(hr))
		goto fail;
	if (hr == MF_E_NO_EVENTS_AVAILABLE)
		return hr;

	HRC(event->GetType(&type));
	HRC(event->GetStatus(&eventStatus));

	if (SUCCEEDED(eventStatus)) {
		if (type == METransformNeedInput) {
			inputRequests++;
		}
		else if (type == METransformHaveOutput) {
			outputRequests++;
		}
	}

	return S_OK;

fail:
	return hr;
}

HRESULT AV1Encoder::InitializeEventGenerator()
{
	HRESULT hr;

	HRC(transform->QueryInterface(&eventGenerator));

	return S_OK;

fail:
	return hr;
}

HRESULT AV1Encoder::InitializeExtraData()
{
	HRESULT hr;
	ComPtr<IMFMediaType> inputType;
	UINT32 headerSize;

	extraData.clear();

	HRC(transform->GetOutputCurrentType(0, &inputType));

	HRC(inputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerSize));

	extraData.resize(headerSize);

	HRC(inputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, extraData.data(),
			headerSize, NULL));

	return S_OK;

fail:
	return hr;
}

static HRESULT SetCodecProperty(ComPtr<ICodecAPI> &codecApi, GUID guid,
	bool value)
{
	VARIANT v;
	v.vt = VT_BOOL;
	v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
	return codecApi->SetValue(&guid, &v);
}

static HRESULT SetCodecProperty(ComPtr<ICodecAPI> &codecApi, GUID guid,
	UINT32 value)
{
	VARIANT v;
	v.vt = VT_UI4;
	v.ulVal = value;
	return codecApi->SetValue(&guid, &v);
}

static HRESULT SetCodecProperty(ComPtr<ICodecAPI> &codecApi, GUID guid,
	UINT64 value)
{
	VARIANT v;
	v.vt = VT_UI8;
	v.ullVal = value;
	return codecApi->SetValue(&guid, &v);
}

bool AV1Encoder::SetBitrate(UINT32 bitrate)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
				CODECAPI_AVEncCommonMeanBitRate,
				UINT32(bitrate * 1000)));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetQP(AV1QP &qp)
{
	HRESULT hr;
	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
				CODECAPI_AVEncCommonQuality,
				UINT32(MapQpToQuality(qp))));
		HRL(SetCodecProperty(codecApi,
				CODECAPI_AVEncVideoEncodeQP,
				UINT64(qp.Pack(true))));
		HRL(SetCodecProperty(codecApi,
				CODECAPI_AVEncVideoEncodeFrameTypeQP,
				UINT64(qp.Pack(false))));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetMinQP(UINT32 minQp)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
				CODECAPI_AVEncVideoMinQP,
				UINT32(minQp)));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetMaxQP(UINT32 maxQp)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
				CODECAPI_AVEncVideoMaxQP,
				UINT32(maxQp)));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetRateControl(AV1RateControl rateControl)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
				CODECAPI_AVEncCommonRateControlMode,
				UINT32(MapRateControl(rateControl))));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetKeyframeInterval(UINT32 seconds)
{
	HRESULT hr;

	if (codecApi) {
		float gopSize = float(framerateNum) / framerateDen * seconds;
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
				CODECAPI_AVEncMPVGOPSize,
				UINT32(gopSize)));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetMaxBitrate(UINT32 maxBitrate)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
			CODECAPI_AVEncCommonMaxBitRate,
			UINT32(maxBitrate * 1000)));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetLowLatency(bool lowLatency)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
			CODECAPI_AVEncCommonLowLatency,
			lowLatency));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetBufferSize(UINT32 bufferSize)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
			CODECAPI_AVEncCommonBufferSize,
			UINT32(bufferSize * 1000)));
	}

	return true;

fail:
	return false;
}

bool AV1Encoder::SetBFrameCount(UINT32 bFrames)
{
	HRESULT hr;

	if (codecApi) {
		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
			CODECAPI_AVEncMPVDefaultBPictureCount,
			UINT32(bFrames)));
	}

	return true;

fail:
	return false;
}

//bool AV1Encoder::SetEntropyEncoding(AV1EntropyEncoding entropyEncoding)
//{
//	HRESULT hr;
//
//	if (codecApi) {
//		HR_CHECK(LOG_WARNING, SetCodecProperty(codecApi,
//				CODECAPI_AVEncAV1CABACEnable,
//				entropyEncoding == AV1EntropyEncodingCABAC));
//	}
//
//	return true;
//
//fail:
//	return false;
//}

bool AV1Encoder::Initialize(std::function<bool(void)> func)
{
	ProfileScope("AV1Encoder::Initialize");

	HRESULT hr;

	ComPtr<IMFMediaType> inputType, outputType;
	ComPtr<IMFAttributes> transformAttributes;
	MFT_OUTPUT_STREAM_INFO streamInfo = {0};

	HRC(CoCreateInstance(descriptor->Guid(), NULL, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&transform)));

	HRC(CreateMediaTypes(inputType, outputType));

	if (descriptor->Async()) {
		HRC(transform->GetAttributes(&transformAttributes));
		HRC(transformAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK,
				TRUE));
	}

	HRC(transform->QueryInterface(&codecApi));

	if (func && !func()) {
		MF_LOG(LOG_ERROR, "Failed setting custom properties");
		goto fail;
	}

	MF_LOG(LOG_INFO, "Activating encoder: %s",
			typeNames[(int)descriptor->Type()]);

	MF_LOG(LOG_INFO, "  Setting output type to transform:");
	LogMediaType(outputType.Get());
	HRC(transform->SetOutputType(0, outputType.Get(), 0));

	MF_LOG(LOG_INFO, "  Setting input type to transform:");
	LogMediaType(inputType.Get());
	HRC(transform->SetInputType(0, inputType.Get(), 0));

	HRC(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
		NULL));

	HRC(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,
		NULL));

	if (descriptor->Async())
		HRC(InitializeEventGenerator());

	HRC(transform->GetOutputStreamInfo(0, &streamInfo));
	createOutputSample = !(streamInfo.dwFlags &
			       (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
			        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));

	return true;

fail:
	return false;
}

bool AV1Encoder::ExtraData(UINT8 **data, UINT32 *dataLength)
{
	if (extraData.empty())
		return false;

	*data = extraData.data();
	*dataLength = (UINT32)extraData.size();

	return true;
}

HRESULT AV1Encoder::CreateEmptySample(ComPtr<IMFSample> &sample,
	ComPtr<IMFMediaBuffer> &buffer, DWORD length)
{
	HRESULT hr;

	HRC(MFCreateSample(&sample));
	HRC(MFCreateMemoryBuffer(length, &buffer));
	HRC(sample->AddBuffer(buffer.Get()));
	return S_OK;

fail:
	return hr;
}

HRESULT AV1Encoder::EnsureCapacity(ComPtr<IMFSample> &sample, DWORD length)
{
	HRESULT hr;
	ComPtr<IMFMediaBuffer> buffer;
	DWORD currentLength;

	if (!sample) {
		HRC(CreateEmptySample(sample, buffer, length));
	}
	else {
		HRC(sample->GetBufferByIndex(0, &buffer));
	}

	HRC(buffer->GetMaxLength(&currentLength));
	if (currentLength < length) {
		HRC(sample->RemoveAllBuffers());
		HRC(MFCreateMemoryBuffer(length, &buffer));
		HRC(sample->AddBuffer(buffer));
	}
	else {
		buffer->SetCurrentLength(0);
	}

	return S_OK;

fail:
	return hr;
}

HRESULT AV1Encoder::ProcessInput(ComPtr<IMFSample> &sample)
{
	ProfileScope("AV1Encoder::ProcessInput(sample)");

	HRESULT hr = S_OK;
	if (descriptor->Async()) {
		if (inputRequests == 1 && inputSamples.empty()) {
			inputRequests--;
			return transform->ProcessInput(0, sample, 0);
		}

		inputSamples.push(sample);

		while (inputRequests > 0) {
			if (inputSamples.empty())
				return hr;
			ComPtr<IMFSample> queuedSample = inputSamples.front();
			inputSamples.pop();
			inputRequests--;
			HRC(transform->ProcessInput(0, queuedSample, 0));
		}
	} else {
		return transform->ProcessInput(0, sample, 0);
	}

fail:
	return hr;
}

bool AV1Encoder::ProcessInput(UINT8 **data, UINT32 *linesize, UINT64 pts,
		Status *status)
{
	ProfileScope("AV1Encoder::ProcessInput");

	HRESULT hr;
	ComPtr<IMFSample> sample;
	ComPtr<IMFMediaBuffer> buffer;
	BYTE *bufferData;
	UINT64 sampleDur;
	UINT32 imageSize;

	HRC(MFCalculateImageSize(MFVideoFormat_NV12, width, height, &imageSize));

	HRC(CreateEmptySample(sample, buffer, imageSize));

	{
		ProfileScope("AV1EncoderCopyInputSample");

		HRC(buffer->Lock(&bufferData, NULL, NULL));

		ProcessNV12([&, this](DWORD height, int plane) {
			MFCopyImage(bufferData, width, data[plane],
					linesize[plane], width, height);
			bufferData += width * height;
		}, height);
	}

	HRC(buffer->Unlock());
	HRC(buffer->SetCurrentLength(imageSize));

	MFFrameRateToAverageTimePerFrame(framerateNum, framerateDen, &sampleDur);

	HRC(sample->SetSampleTime(pts * sampleDur));
	HRC(sample->SetSampleDuration(sampleDur));

	if (descriptor->Async()) {
		HRC(DrainEvents());

		while (outputRequests > 0 && (hr = ProcessOutput()) == S_OK);

		if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr)) {
			MF_LOG_COM(LOG_ERROR, "ProcessOutput()", hr);
			goto fail;
		}


		while (inputRequests == 0) {
			hr = DrainEvent(false);
			if (hr == MF_E_NO_EVENTS_AVAILABLE) {
				Sleep(1);
				continue;
			}
			if (FAILED(hr)) {
				MF_LOG_COM(LOG_ERROR, "DrainEvent()", hr);
				goto fail;
			}
			if (outputRequests > 0) {
				hr = ProcessOutput();
				if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT &&
				    FAILED(hr))
					goto fail;
			}
		}
	}

	HRC(ProcessInput(sample));

	pendingRequests++;

	*status = SUCCESS;
	return true;

fail:
	*status = FAILURE;
	return false;
}

HRESULT AV1Encoder::ProcessOutput()
{
	HRESULT hr;
	ComPtr<IMFSample> sample;
	MFT_OUTPUT_STREAM_INFO outputInfo = { 0 };

	DWORD outputStatus = 0;
	MFT_OUTPUT_DATA_BUFFER output = { 0 };
	ComPtr<IMFMediaBuffer> buffer;
	BYTE *bufferData;
	DWORD bufferLength;
	INT64 samplePts;
	INT64 sampleDts;
	INT64 sampleDur;
	std::unique_ptr<std::vector<BYTE>> data(new std::vector<BYTE>());
	ComPtr<IMFMediaType> type;
	std::unique_ptr<AV1Frame> frame;
	bool keyframe = false;

	if (descriptor->Async()) {
		HRC(DrainEvents());

		if (outputRequests == 0)
			return S_OK;

		outputRequests--;
	}

	if (createOutputSample) {
		HRC(transform->GetOutputStreamInfo(0, &outputInfo));
		HRC(CreateEmptySample(sample, buffer, outputInfo.cbSize));
		output.pSample = sample;
	} else {
		output.pSample = NULL;
	}

	while (true) {
		hr = transform->ProcessOutput(0, 1, &output,
				&outputStatus);
		ComPtr<IMFCollection> events(output.pEvents);

		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
			return hr;

		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			HRC(transform->GetOutputAvailableType(0, 0, &type));
			HRC(transform->SetOutputType(0, type, 0));
			MF_LOG(LOG_INFO, "Updating output type to transform");
			LogMediaType(type);
			if (descriptor->Async() && outputRequests > 0) {
				outputRequests--;
				continue;
			} else {
				return MF_E_TRANSFORM_NEED_MORE_INPUT;
			}
		}

		if (hr != S_OK) {
			MF_LOG_COM(LOG_ERROR, "transform->ProcessOutput()",
					hr);
			return hr;
		}

		break;
	}

	if (!createOutputSample)
		sample.Set(output.pSample);


	HRC(sample->GetBufferByIndex(0, &buffer));

	keyframe = !!MFGetAttributeUINT32(sample,
			MFSampleExtension_CleanPoint, FALSE);

	HRC(buffer->Lock(&bufferData, NULL, &bufferLength));

	if (keyframe && extraData.empty())
		HRC(InitializeExtraData());

	data->reserve(bufferLength + extraData.size());

	if (keyframe)
		data->insert(data->end(), extraData.begin(), extraData.end());

	data->insert(data->end(), &bufferData[0], &bufferData[bufferLength]);
	HRC(buffer->Unlock());

	HRC(sample->GetSampleDuration(&sampleDur));
	HRC(sample->GetSampleTime(&samplePts));

	sampleDts = MFGetAttributeUINT64(sample,
			MFSampleExtension_DecodeTimestamp, samplePts);

	frame.reset(new AV1Frame(keyframe,
			samplePts / sampleDur,
			sampleDts / sampleDur,
			std::move(data)));

	encodedFrames.push(std::move(frame));

	return S_OK;

fail:
	return hr;
}

bool AV1Encoder::ProcessOutput(UINT8 **data, UINT32 *dataLength,
		UINT64 *pts, UINT64 *dts, bool *keyframe, Status *status)
{
	ProfileScope("AV1Encoder::ProcessOutput");

	HRESULT hr;

	hr = ProcessOutput();

	if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || encodedFrames.empty()) {
		*status = NEED_MORE_INPUT;
		return true;
	}

	if (FAILED(hr) && encodedFrames.empty()) {
		*status = FAILURE;
		return false;
	}

	activeFrame = std::move(encodedFrames.front());
	encodedFrames.pop();

	*data = activeFrame.get()->Data();
	*dataLength = activeFrame.get()->DataLength();
	*pts = activeFrame.get()->Pts();
	*dts = activeFrame.get()->Dts();
	*keyframe = activeFrame.get()->Keyframe();
	*status = SUCCESS;

	pendingRequests--;

	return true;
}

#include "FFInterpolator.h"
#include <frameinterpolation/ffx_frameinterpolation_private.h>
#include <ffx_object_management.h>
#include <utility>

FFInterpolator::FFInterpolator(
	const FfxInterface& BackendInterface,
	const FfxInterface& SharedBackendInterface,
	FfxUInt32 SharedEffectContextId,
	uint32_t MaxRenderWidth,
	uint32_t MaxRenderHeight,
	std::function<void(FfxCommandList, const FfxResource *, const FfxResource *)> CopyTextureFn)
	: m_BackendInterface(BackendInterface),
	  m_SharedBackendInterface(SharedBackendInterface),
	  m_SharedEffectContextId(SharedEffectContextId),
	  m_CopyTextureFn(std::move(CopyTextureFn)),
	  m_MaxRenderWidth(MaxRenderWidth),
	  m_MaxRenderHeight(MaxRenderHeight)
{
}

FFInterpolator::~FFInterpolator()
{
	DestroyContext();
}

FfxErrorCode FFInterpolator::Dispatch(const FFInterpolatorDispatchParameters& Parameters)
{
	if (auto status = CreateContextDeferred(Parameters); status != FFX_OK) // Massive frame hitch on first call
		return status;

	FfxFrameInterpolationDispatchDescription dispatchDesc = {};
	{
		if (Parameters.DebugTearLines)
			dispatchDesc.flags |= FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES;

		if (Parameters.DebugView)
			dispatchDesc.flags |= FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW;

		dispatchDesc.commandList = Parameters.CommandList;
		dispatchDesc.displaySize = Parameters.OutputSize;
		dispatchDesc.renderSize = Parameters.RenderSize;

		dispatchDesc.currentBackBuffer = Parameters.InputColorBuffer;

		dispatchDesc.currentBackBuffer_HUDLess = Parameters.InputHUDLessColorBuffer;
		const bool hudlessFormatMismatch = dispatchDesc.currentBackBuffer_HUDLess.resource &&
										   dispatchDesc.currentBackBuffer_HUDLess.description.format !=
											   dispatchDesc.currentBackBuffer.description.format;

		if (hudlessFormatMismatch)
		{
			auto compatibleHudless = GetHUDLessCompatibleResource(dispatchDesc.currentBackBuffer, dispatchDesc.displaySize);
			if (compatibleHudless.resource && m_CopyTextureFn)
			{
				m_CopyTextureFn(Parameters.CommandList, &compatibleHudless, &dispatchDesc.currentBackBuffer_HUDLess);
				compatibleHudless.state = FFX_RESOURCE_STATE_COMPUTE_READ;
				dispatchDesc.currentBackBuffer_HUDLess = compatibleHudless;
			}
			else
			{
				dispatchDesc.currentBackBuffer_HUDLess = {};
			}
		}
		dispatchDesc.output = Parameters.OutputInterpolatedColorBuffer;

		dispatchDesc.interpolationRect = { 0,
										   0,
										   static_cast<int>(dispatchDesc.displaySize.width),
										   static_cast<int>(dispatchDesc.displaySize.height) };

		dispatchDesc.opticalFlowVector = Parameters.InputOpticalFlowVector;
		dispatchDesc.opticalFlowSceneChangeDetection = Parameters.InputOpticalFlowSceneChangeDetection;
		// dispatchDesc.opticalFlowBufferSize = Parameters.OpticalFlowBufferSize; // Completely unused?
		dispatchDesc.opticalFlowScale = Parameters.OpticalFlowScale;
		dispatchDesc.opticalFlowBlockSize = Parameters.OpticalFlowBlockSize;

		dispatchDesc.cameraNear = Parameters.CameraNear;
		dispatchDesc.cameraFar = Parameters.CameraFar;
		dispatchDesc.cameraFovAngleVertical = Parameters.CameraFovAngleVertical;
		dispatchDesc.viewSpaceToMetersFactor = 1.0f;

		dispatchDesc.frameTimeDelta = 1000.0f / 60.0f; // Unused
		dispatchDesc.reset = Parameters.Reset;

		dispatchDesc.backBufferTransferFunction = Parameters.HDR ? FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ
																 : FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		dispatchDesc.minMaxLuminance[0] = Parameters.MinMaxLuminance.x;
		dispatchDesc.minMaxLuminance[1] = Parameters.MinMaxLuminance.y;

		dispatchDesc.frameID = 0; // Not async and not bindless. Don't bother.

		dispatchDesc.dilatedDepth = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_DilatedDepth);
		dispatchDesc.dilatedMotionVectors = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_DilatedMotionVectors);
		dispatchDesc.reconstructedPrevDepth = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_ReconstructedPrevDepth);
		dispatchDesc.distortionField = Parameters.InputDistortionField;
	}

	FfxFrameInterpolationPrepareDescription prepareDesc = {};
	{
		prepareDesc.flags = dispatchDesc.flags;
		prepareDesc.commandList = dispatchDesc.commandList;
		prepareDesc.renderSize = dispatchDesc.renderSize;
		prepareDesc.jitterOffset = Parameters.MotionVectorJitterOffsets;
		prepareDesc.motionVectorScale = Parameters.MotionVectorScale;

		prepareDesc.frameTimeDelta = dispatchDesc.frameTimeDelta;
		prepareDesc.cameraNear = dispatchDesc.cameraNear;
		prepareDesc.cameraFar = dispatchDesc.cameraFar;
		prepareDesc.viewSpaceToMetersFactor = 1.0f;
		prepareDesc.cameraFovAngleVertical = dispatchDesc.cameraFovAngleVertical;

		prepareDesc.depth = Parameters.InputDepth;
		prepareDesc.motionVectors = Parameters.InputMotionVectors;

		prepareDesc.frameID = dispatchDesc.frameID;

		prepareDesc.dilatedDepth = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_DilatedDepth);
		prepareDesc.dilatedMotionVectors = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_DilatedMotionVectors);
		prepareDesc.reconstructedPrevDepth = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_ReconstructedPrevDepth);
	}

	if (auto status = ffxFrameInterpolationPrepare(&m_FSRContext.value(), &prepareDesc); status != FFX_OK)
		return status;

	if (Parameters.MotionVectorsDilated && m_CopyTextureFn)
	{
		FfxResource overrideDestination = prepareDesc.dilatedMotionVectors;
		overrideDestination.state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
		FfxResource overrideSource = Parameters.InputMotionVectors;
		m_CopyTextureFn(Parameters.CommandList, &overrideDestination, &overrideSource);
	}

	return ffxFrameInterpolationDispatch(&m_FSRContext.value(), &dispatchDesc);
}

FfxErrorCode FFInterpolator::CreateContextDeferred(const FFInterpolatorDispatchParameters& Parameters)
{
	FfxFrameInterpolationContextDescription desc = {};
	desc.backendInterface = m_BackendInterface;

	if (Parameters.DepthInverted)
		desc.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;

	if (Parameters.DepthPlaneInfinite)
		desc.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;

	if (Parameters.HDR)
		desc.flags |= FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT;

	if (Parameters.MotionVectorsFullResolution)
		desc.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;

	if (Parameters.MotionVectorJitterCancellation)
		desc.flags |= FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS;

	desc.maxRenderSize = { m_MaxRenderWidth, m_MaxRenderHeight };
	desc.displaySize = desc.maxRenderSize;

	desc.backBufferFormat = Parameters.InputColorBuffer.description.format;
	desc.previousInterpolationSourceFormat = desc.backBufferFormat;

	if (std::exchange(m_ContextFlushPending, false))
		DestroyContext();

	if (m_FSRContext)
	{
		if (memcmp(&desc, &m_ContextDescription, sizeof(m_ContextDescription)) == 0)
			return FFX_OK;

		m_ContextFlushPending = true;
		return FFX_EOF; // Description changed. Return fake status to request a flush from our parent.
	}

	m_ContextDescription = desc;
	auto status = ffxFrameInterpolationContextCreate(&m_FSRContext.emplace(), &desc);

	if (status != FFX_OK)
	{
		m_FSRContext.reset();
		return status;
	}

	OverrideDefaultDistortionField();

	FfxFrameInterpolationSharedResourceDescriptions fsrFiSharedDescriptions = {};
	status = ffxFrameInterpolationGetSharedResourceDescriptions(&m_FSRContext.value(), &fsrFiSharedDescriptions);

	if (status != FFX_OK)
	{
		DestroyContext();
		return status;
	}

	status = m_SharedBackendInterface.fpCreateResource(
		&m_SharedBackendInterface,
		&fsrFiSharedDescriptions.dilatedDepth,
		m_SharedEffectContextId,
		&m_DilatedDepth.emplace());

	if (status != FFX_OK)
	{
		m_DilatedDepth.reset();
		DestroyContext();

		return status;
	}

	status = m_SharedBackendInterface.fpCreateResource(
		&m_SharedBackendInterface,
		&fsrFiSharedDescriptions.dilatedMotionVectors,
		m_SharedEffectContextId,
		&m_DilatedMotionVectors.emplace());

	if (status != FFX_OK)
	{
		m_DilatedMotionVectors.reset();
		DestroyContext();

		return status;
	}

	status = m_SharedBackendInterface.fpCreateResource(
		&m_SharedBackendInterface,
		&fsrFiSharedDescriptions.reconstructedPrevNearestDepth,
		m_SharedEffectContextId,
		&m_ReconstructedPrevDepth.emplace());

	if (status != FFX_OK)
	{
		m_ReconstructedPrevDepth.reset();
		DestroyContext();

		return status;
	}

	return FFX_OK;
}

void FFInterpolator::DestroyContext()
{
	if (m_FSRContext)
		ffxFrameInterpolationContextDestroy(&m_FSRContext.value());

	if (m_DilatedDepth)
		m_SharedBackendInterface.fpDestroyResource(&m_SharedBackendInterface, *m_DilatedDepth, m_SharedEffectContextId);

	if (m_DilatedMotionVectors)
		m_SharedBackendInterface.fpDestroyResource(&m_SharedBackendInterface, *m_DilatedMotionVectors, m_SharedEffectContextId);

	if (m_ReconstructedPrevDepth)
		m_SharedBackendInterface.fpDestroyResource(&m_SharedBackendInterface, *m_ReconstructedPrevDepth, m_SharedEffectContextId);

	if (m_HUDLessCompatibleColor)
		m_SharedBackendInterface.fpDestroyResource(&m_SharedBackendInterface, *m_HUDLessCompatibleColor, m_SharedEffectContextId);

	m_FSRContext.reset();
	m_DilatedDepth.reset();
	m_DilatedMotionVectors.reset();
	m_ReconstructedPrevDepth.reset();
	m_HUDLessCompatibleColor.reset();
}

// Replace the SDK's UNORM default distortion field with the SNORM version we need.
void FFInterpolator::OverrideDefaultDistortionField()
{
	if (!m_FSRContext)
		return;

	auto *contextPrivate = reinterpret_cast<FfxFrameInterpolationContext_Private *>(&m_FSRContext.value());
	const uint32_t defaultDistortionId = FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEFAULT_DISTORTION_FIELD;
	FfxResourceInternal& defaultDistortion = contextPrivate->srvResources[defaultDistortionId];

	uint8_t defaultData[4] = { 0, 0, 0, 0 };
	FfxCreateResourceDescription createDesc = {};
	createDesc.heapType = FFX_HEAP_TYPE_DEFAULT;
	createDesc.resourceDescription = { FFX_RESOURCE_TYPE_TEXTURE2D, FFX_SURFACE_FORMAT_R8G8B8A8_SNORM, 1, 1, 1, 1,
									   FFX_RESOURCE_FLAGS_NONE,		FFX_RESOURCE_USAGE_READ_ONLY };
	createDesc.initialState = FFX_RESOURCE_STATE_COMPUTE_READ;
	createDesc.name = L"FI_DefaultDistortionField";
	createDesc.id = defaultDistortionId;
	createDesc.initData = FfxResourceInitData::FfxResourceInitBuffer(sizeof(defaultData), defaultData);

	const int32_t originalDefaultIndex = defaultDistortion.internalIndex;
	FfxResourceInternal replacement = {};
	if (contextPrivate->contextDescription.backendInterface.fpCreateResource(
			&contextPrivate->contextDescription.backendInterface,
			&createDesc,
			contextPrivate->effectContextId,
			&replacement) == FFX_OK)
	{
		ffxSafeReleaseCopyResource(
			&contextPrivate->contextDescription.backendInterface,
			defaultDistortion,
			contextPrivate->effectContextId);
		defaultDistortion = replacement;
		contextPrivate->uavResources[defaultDistortionId] = replacement;

		const uint32_t distortionFieldId = FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISTORTION_FIELD;
		FfxResourceInternal& distortionField = contextPrivate->srvResources[distortionFieldId];
		if (distortionField.internalIndex == 0 || distortionField.internalIndex == originalDefaultIndex)
		{
			distortionField = replacement;
			contextPrivate->uavResources[distortionFieldId] = replacement;
		}
	}
}

FfxResource FFInterpolator::GetHUDLessCompatibleResource(const FfxResource& Reference, FfxDimensions2D OutputSize)
{
	if (m_HUDLessCompatibleColor)
	{
		auto currentDesc = m_SharedBackendInterface.fpGetResourceDescription(&m_SharedBackendInterface, *m_HUDLessCompatibleColor);
		const bool sizeMismatch = currentDesc.width != OutputSize.width || currentDesc.height != OutputSize.height;
		const bool formatMismatch = currentDesc.format != Reference.description.format;

		if (sizeMismatch || formatMismatch)
		{
			m_SharedBackendInterface.fpDestroyResource(&m_SharedBackendInterface, *m_HUDLessCompatibleColor, m_SharedEffectContextId);
			m_HUDLessCompatibleColor.reset();
		}
	}

	if (!m_HUDLessCompatibleColor)
	{
		FfxCreateResourceDescription desc = {};
		desc.heapType = FFX_HEAP_TYPE_DEFAULT;
		desc.resourceDescription = { FFX_RESOURCE_TYPE_TEXTURE2D, Reference.description.format, OutputSize.width, OutputSize.height, 1, 1,
									 FFX_RESOURCE_FLAGS_NONE,	  FFX_RESOURCE_USAGE_READ_ONLY };
		desc.initialState = FFX_RESOURCE_STATE_COMPUTE_READ;
		desc.name = L"DLSSG_HUDLessCompat";
		desc.id = 0;
		desc.initData = { FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED };

		FfxResourceInternal resource = {};
		if (m_SharedBackendInterface.fpCreateResource(&m_SharedBackendInterface, &desc, m_SharedEffectContextId, &resource) == FFX_OK)
		{
			m_HUDLessCompatibleColor = resource;
		}
	}

	if (!m_HUDLessCompatibleColor)
		return {};

	auto result = m_SharedBackendInterface.fpGetResource(&m_SharedBackendInterface, *m_HUDLessCompatibleColor);
	result.state = FFX_RESOURCE_STATE_COMPUTE_READ;
	return result;
}

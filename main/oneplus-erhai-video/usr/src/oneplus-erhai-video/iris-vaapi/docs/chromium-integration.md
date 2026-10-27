# Chromium integration notes

The driver targets Chromium's Linux VA-API decode path without carrying a private
Chromium fork. The most relevant upstream files are:

- [`media/gpu/vaapi/vaapi_wrapper.cc`](https://chromium.googlesource.com/chromium/src/+/main/media/gpu/vaapi/vaapi_wrapper.cc)
- [`media/gpu/vaapi/vaapi_video_decoder.cc`](https://chromium.googlesource.com/chromium/src/+/main/media/gpu/vaapi/vaapi_video_decoder.cc)
- [`media/gpu/vaapi/h264_vaapi_video_decoder_delegate.cc`](https://chromium.googlesource.com/chromium/src/+/main/media/gpu/vaapi/h264_vaapi_video_decoder_delegate.cc)
- [`media/gpu/vaapi/h265_vaapi_video_decoder_delegate.cc`](https://chromium.googlesource.com/chromium/src/+/main/media/gpu/vaapi/h265_vaapi_video_decoder_delegate.cc)

The repository previously contained unversioned copies of two Chromium `.cc` files.
Those snapshots were removed because they become stale quickly and were not build inputs.
Use the upstream links and record the Chromium revision when investigating behavior.

Important compatibility points implemented by iris-vaapi:

- surfaces can be synchronized and exported before their first decode;
- exported NV12 uses separate linear layers and stable DMA-BUF backing;
- surface generations prevent a late CAPTURE frame from overwriting a reused target;
- reservation fences cover asynchronous writes to exported DMA-BUFs;
- H.264/HEVC parameter buffers and slices are converted to complete Annex-B access units;
- the stateful decoder is drained or given access-unit boundaries so the final picture is
  not replaced by stale surface contents during Chromium reset/flush.

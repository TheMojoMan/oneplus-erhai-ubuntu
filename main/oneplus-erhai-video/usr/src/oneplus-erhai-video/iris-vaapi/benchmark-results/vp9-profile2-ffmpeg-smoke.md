# VP9 Profile 2 smoke check

- Input: `The World in HDR.mkv`
- Stream: VP9 Profile 2, 3840x2160, 10-bit HDR, approximately 59.94 fps
- Direct V4L2 selected `vp9_v4l2m2m` and `/dev/video0`, but FFmpeg
  finished with an empty output (`frame=0`) while returning exit code 0.
- VA-API selected the local Iris driver and produced all requested 180 smoke-test
  frames at approximately 103 fps.

This result applies only to FFmpeg's `vp9_v4l2m2m` wrapper, which did not select
P010 capture. The aggregate benchmark instead uses the repository's
explicit-P010 V4L2 harness; that path decoded all 1,200 frames without corrupt
output.

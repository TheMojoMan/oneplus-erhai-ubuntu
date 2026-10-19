# HEVC Main10 smoke check

- Input: `Test Jellyfin 4K HEVC 10bit 60M.mp4`
- Stream: HEVC Main10, 3840x2160, 60 fps, approximately 59 Mb/s
- Direct V4L2 command selected `hevc_v4l2m2m` and `/dev/video0`, but FFmpeg
  finished with an empty output (`frame=0`) while returning exit code 0.
- VA-API selected the local Iris driver and produced all requested 180 smoke-test
  frames at approximately 127 fps.

This result applies only to FFmpeg's `hevc_v4l2m2m` wrapper, which did not
select P010 capture. The aggregate benchmark instead uses the repository's
explicit-P010 V4L2 harness; that path decoded all 1,797 frames without corrupt
output.

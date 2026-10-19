WITH runs(codec, decoder, repetition, decode_fps, realtime_speed_x,
          cpu_core_percent, max_rss_mib, wall_seconds) AS (
  VALUES
    ('h264', 'v4l2',  1, 167.167, 2.7861, 31.21, 414.414, 10.749711),
    ('h264', 'vaapi', 1,  65.692, 1.0949, 31.64, 544.801, 27.355108),
    ('hevc', 'v4l2',  1, 176.449, 2.9408, 32.63, 410.039, 10.184246),
    ('hevc', 'vaapi', 1, 173.571, 2.8929, 52.48, 674.383, 10.353097),
    ('hevc', 'vaapi', 2, 174.326, 2.9054, 51.79, 677.961, 10.308257),
    ('hevc', 'v4l2',  2, 176.182, 2.9364, 31.18, 412.574, 10.199679),
    ('h264', 'vaapi', 2,  65.688, 1.0948, 31.67, 544.719, 27.356616),
    ('h264', 'v4l2',  2, 167.639, 2.7940, 29.59, 414.414, 10.719439),
    ('h264', 'v4l2',  3, 167.649, 2.7942, 29.37, 417.512, 10.718812),
    ('h264', 'vaapi', 3,  65.256, 1.0876, 31.47, 544.180, 27.537861),
    ('hevc', 'v4l2',  3, 176.280, 2.9380, 31.32, 412.273, 10.194004),
    ('hevc', 'vaapi', 3, 173.602, 2.8934, 52.59, 674.137, 10.351240),
    ('vp9',  'v4l2',  1, 146.328, 2.4388, 36.28, 519.012, 4.100371),
    ('vp9',  'vaapi', 1, 142.873, 2.3812, 44.64, 556.438, 4.199544),
    ('vp9',  'vaapi', 2, 145.331, 2.4222, 45.52, 556.363, 4.128506),
    ('vp9',  'v4l2',  2, 146.293, 2.4382, 39.94, 517.145, 4.101360),
    ('vp9',  'v4l2',  3, 146.387, 2.4398, 36.44, 515.582, 4.098717),
    ('vp9',  'vaapi', 3, 146.055, 2.4343, 49.26, 556.523, 4.108039),
    ('vp9',  'vaapi', 4, 142.792, 2.3799, 42.84, 556.414, 4.201904),
    ('vp9',  'v4l2',  4, 146.364, 2.4394, 33.31, 517.012, 4.099382),
    ('vp9',  'v4l2',  5, 146.859, 2.4477, 33.09, 515.383, 4.085544),
    ('vp9',  'vaapi', 5, 145.397, 2.4233, 42.31, 558.211, 4.126641),
    ('hevc-main10', 'v4l2',  1, 148.389, 2.4732, 26.39, 798.371, 12.110033),
    ('hevc-main10', 'vaapi', 1, 148.580, 2.4763, 85.60, 936.375, 12.094462),
    ('vp9-profile2', 'v4l2',  1, 116.421, 1.9423, 20.76, 739.066, 10.307453),
    ('vp9-profile2', 'vaapi', 1, 114.073, 1.9031, 41.84, 798.750, 10.519602),
    ('vp9-profile2', 'vaapi', 2, 113.888, 1.9000, 42.16, 799.559, 10.536696),
    ('vp9-profile2', 'v4l2',  2, 116.271, 1.9398, 20.32, 739.129, 10.320757),
    ('hevc-main10', 'vaapi', 2, 147.371, 2.4562, 86.25, 940.125, 12.193688),
    ('hevc-main10', 'v4l2',  2, 148.211, 2.4702, 26.54, 798.309, 12.124636),
    ('hevc-main10', 'v4l2',  3, 148.263, 2.4711, 26.69, 798.375, 12.120321),
    ('hevc-main10', 'vaapi', 3, 149.687, 2.4948, 85.26, 937.008, 12.005069),
    ('vp9-profile2', 'v4l2',  3, 116.285, 1.9400, 20.45, 739.133, 10.319440),
    ('vp9-profile2', 'vaapi', 3, 113.494, 1.8935, 42.27, 797.457, 10.573240)
),
aggregate AS (
  SELECT
    codec,
    decoder,
    AVG(decode_fps) AS decode_fps,
    AVG(realtime_speed_x) AS realtime_speed_x,
    AVG(cpu_core_percent) AS cpu_core_percent,
    AVG(max_rss_mib) AS max_rss_mib,
    AVG(wall_seconds) AS wall_seconds,
    100.0 * SQRT(
      (SUM(decode_fps * decode_fps)
       - SUM(decode_fps) * SUM(decode_fps) / COUNT(*))
      / (COUNT(*) - 1)
    ) / AVG(decode_fps) AS fps_cv_percent,
    COUNT(*) AS repetitions
  FROM runs
  GROUP BY codec, decoder
)
SELECT
  current.codec,
  current.decoder,
  current.decode_fps,
  current.realtime_speed_x,
  current.cpu_core_percent,
  current.max_rss_mib,
  current.wall_seconds,
  current.fps_cv_percent,
  current.repetitions,
  100.0 * (current.decode_fps / baseline.decode_fps - 1.0)
    AS speed_delta_vs_v4l2_percent,
  100.0 * (current.cpu_core_percent / baseline.cpu_core_percent - 1.0)
    AS cpu_delta_vs_v4l2_percent,
  100.0 * (current.max_rss_mib / baseline.max_rss_mib - 1.0)
    AS rss_delta_vs_v4l2_percent
FROM aggregate AS current
JOIN aggregate AS baseline
  ON baseline.codec = current.codec
 AND baseline.decoder = 'v4l2'
ORDER BY current.codec, current.decoder DESC;

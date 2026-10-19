// SPDX-License-Identifier: GPL-2.0-only
/*
 * ErhaiCam — standalone GTK4 camera app for the OnePlus Pad 3 (erhai).
 *
 * Per-frame image analysis — the autofocus sharpness metric and the
 * auto-exposure luminance statistics — runs on the Hexagon NPU: an ONNX
 * graph executed by ONNX Runtime's QNN execution provider on the HTP, driven
 * through the erhai-npu-analyze.py helper (see the PER-FRAME ANALYSIS MODEL
 * comment).  A CPU implementation of the same metric is the automatic
 * fallback, so AF/AE keep working with no NPU present.
 *
 * Autofocus is a contrast (Tenengrad) sweep driving the kernel cn3927 lens
 * subdev directly (V4L2_CID_FOCUS_ABSOLUTE) — algorithm ported from
 * CFM880/nabu-camera's nabu-autofocus.  Continuous AF with tap-to-focus and
 * refocus-on-sharpness-drop; toggled off, a vertical slider takes over for
 * manual focus.  Auto exposure is a log-domain controller on the sensor's
 * integration time, written straight to the sc1320cs register 0x3e00 (the
 * kernel's V4L2_CID_EXPOSURE is a no-op); toggled off, a vertical slider
 * takes over there too.  Both loops run on the rear camera only (the only
 * one with a lens and a known exposure register).  The live preview is
 * counter-rotated T*90 deg ccw at mutter transform T (1..3, both cameras —
 * see the ORIENTATION MODEL comment) while photo capture saves the frame
 * UNROTATED (the raw chassis-fixed frame is the world record and stays
 * upright as a photo at every orientation); video is recorded as streamed.
 */

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <gtk/gtk.h>

#include <linux/videodev2.h>

#include <cairo.h>
#include <csignal>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr char kRearCamera[] =
	"/base/soc@0/cci@ac7b000/i2c-bus@1/camera-sensor@36";
constexpr char kFrontCamera[] =
	"/base/soc@0/geniqup@8c0000/i2c@884000/camera-sensor@10";
constexpr char kLensName[] = "cn3927 focus";
constexpr int kFocusMin = 0;
constexpr int kFocusMax = 1023;
constexpr int kSettleFrames = 1;
constexpr int kScoreFrames = 2;
constexpr int kMonitorStride = 6;
constexpr int kMonitorFrames = 3;
constexpr int kLowChecks = 4;
constexpr double kTapHoldSeconds = 5.0;
/* "No usable detail" guard for the coarse sweep: the metric is the mean
 * |grad|^2 of a [0,1] grey image (see tenengrad), so a genuinely flat scene
 * sits around 1e-5 and an in-focus one at 1e-3..1e-2.  The real decision is
 * the RELATIVE test next to it; this only rejects scenes with nothing at all. */
constexpr double kAfMinDetail = 5e-5;

/* ---- exposure / the sc1320cs register interface -------------------------
 * Integration time is 0x3e00..0x3e02 as a 3-byte big-endian burst (the
 * sensor auto-increments), holding lines << 5.  VTS is fixed at 6404 lines,
 * so the register's usable range is 0..6396 and the sensor must stay below
 * the frame length.  The soft ISP can invert ("negative") frames if the raw
 * saturates near the top, so AUTO exposure keeps itself below kExposureAeMax
 * and leaves the last quarter of the range to the manual slider. */
constexpr int kSensorAddr = 0x36;
constexpr int kExposureShift = 5;
constexpr int kExposureMin = 1;
constexpr int kExposureMax = 6396;       /* VTS(6404) - 8 */
constexpr int kExposureAeMax = 4800;     /* auto exposure stays clear of the
					  * raw-saturation/ISP-inversion zone */
constexpr int kExposureDefault = 3196;   /* sensor mode-table default */
constexpr double kRawExposureEpsilon = 0.02;  /* highlight window before the
					       * controller refuses to brighten */

/* ---- NPU / auto-exposure loop ------------------------------------------- */
/* The analyzer input is a FIXED 320x240 grey frame (the ONNX graph has static
 * shapes for the HTP): the AF region and the whole frame are stride-sampled
 * into it.  The AF region of a 3200x2400 preview is 1920x1440, i.e. sampled
 * 1/6 — the same scale class real AF systems use. */
constexpr int kStatsW = 320;
constexpr int kStatsH = 240;
constexpr double kAeTarget = 0.45;       /* target mean luminance */
constexpr int kAeStride = 3;             /* analyse every 3rd frame */
constexpr int kAeSettle = 2;             /* frames to wait after a change */
constexpr double kAeDeadband = 0.06;     /* |log2(target/luma)| below this: hold */
constexpr double kAeGain = 0.5;          /* log2 step per log2 error */
constexpr double kAeMaxStep = 0.25;      /* <= ~19 % change per update */
constexpr int kSliderDebounceMs = 60;

/* Negotiation fallbacks. The soft ISP GPU-debayers and scales to any
 * standard size at ~30 fps — ask for 8MP photos first, fall back to proven
 * sizes. NOTE: never ask for the bare native sizes (front 3264x2448, rear
 * 4152x3120) — the soft-ISP streams are 3256/4152 wide-cropped and the bare
 * native width fails caps negotiation. */
const std::vector<std::pair<int, int>> kRearResolutions = {
	{ 3200, 2400 },
	{ 1280, 960 },
};
const std::vector<std::pair<int, int>> kFrontResolutions = {
	{ 3200, 2400 },
	{ 1600, 1200 },
};

using Steady = std::chrono::steady_clock;

std::string read_text_file(const std::filesystem::path &path)
{
	std::ifstream input(path);
	std::string value;

	std::getline(input, value);
	return value;
}

class Lens {
public:
	static std::optional<std::string> find()
	{
		const std::filesystem::path video_class("/sys/class/video4linux");

		for (const auto &entry :
		     std::filesystem::directory_iterator(video_class)) {
			const std::string node = entry.path().filename();

			if (!node.starts_with("v4l-subdev"))
				continue;
			if (read_text_file(entry.path() / "name") == kLensName)
				return "/dev/" + node;
		}

		return std::nullopt;
	}

	explicit Lens(const std::string &path) : path_(path)
	{
		fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd_ < 0)
			throw std::runtime_error("cannot open " + path + ": " +
						 std::strerror(errno));

		v4l2_control control = { V4L2_CID_FOCUS_ABSOLUTE, 0 };
		if (ioctl(fd_, VIDIOC_G_CTRL, &control) == 0)
			position_ = control.value;
	}

	~Lens()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	Lens(const Lens &) = delete;
	Lens &operator=(const Lens &) = delete;

	bool set_focus(int position)
	{
		v4l2_control control = {
			V4L2_CID_FOCUS_ABSOLUTE,
			std::clamp(position, kFocusMin, kFocusMax),
		};

		if (ioctl(fd_, VIDIOC_S_CTRL, &control) < 0)
			return false;
		position_ = control.value;
		return true;
	}

	int position() const { return position_; }

private:
	std::string path_;
	int fd_ = -1;
	int position_ = 0;
};

/* ---------------------- sensor register access (exposure) ---------------- */

/* Manual/auto exposure writes the sc1320cs integration-time register
 * directly.  The kernel driver accepts V4L2_CID_EXPOSURE but its s_ctrl is a
 * no-op ("accepted but not yet written to hardware", drivers/media/i2c/
 * sc1320cs.c), so the control is useless and the app is the register's owner.
 *
 * The sensor's rails are only powered while it streams, so every write must
 * be guarded by app->camera_ok or the CCI transaction NAKs.  udev rule
 * 70-erhai-cam-i2c.rules grants the session user rw on the CCI adapter
 * (uaccess); the two CCI masters enumerate in probe order (i2c-10 and i2c-11
 * swap between boots), so the bus is resolved from the i2c client named
 * "sc1320cs" — never hardcoded. */
class Sensor {
public:
	static std::optional<std::string> find()
	{
		const std::filesystem::path bus("/sys/bus/i2c/devices");
		std::error_code ec;

		for (const auto &entry :
		     std::filesystem::directory_iterator(bus, ec)) {
			if (read_text_file(entry.path() / "name") != "sc1320cs")
				continue;
			const std::string dev = entry.path().filename();
			const auto dash = dev.find('-');
			if (dash == std::string::npos)
				continue;
			return "/dev/i2c-" + dev.substr(0, dash);
		}
		return std::nullopt;
	}

	explicit Sensor(const std::string &path) : path_(path)
	{
		fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd_ < 0)
			throw std::runtime_error("cannot open " + path + ": " +
						 std::strerror(errno));
	}

	~Sensor()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	Sensor(const Sensor &) = delete;
	Sensor &operator=(const Sensor &) = delete;

	bool set_exposure_lines(int lines)
	{
		lines = std::clamp(lines, kExposureMin, kExposureMax);
		const unsigned value = unsigned(lines) << kExposureShift;
		unsigned char msg[5] = {
			0x3e, 0x00,
			(unsigned char)((value >> 16) & 0xff),
			(unsigned char)((value >> 8) & 0xff),
			(unsigned char)(value & 0xff),
		};
		struct i2c_msg m = {};
		struct i2c_rdwr_ioctl_data xfer = { &m, 1 };

		m.addr = kSensorAddr;
		m.flags = 0;
		m.len = sizeof(msg);
		m.buf = msg;
		/* I2C_RDWR, not I2C_SLAVE + write(): the kernel sc1320cs driver
		 * holds the CCI bus while the sensor streams and a second
		 * I2C_SLAVE claim of the same address gets EBUSY, whereas an
		 * I2C_RDWR transaction (what i2ctransfer does) always works. */
		if (ioctl(fd_, I2C_RDWR, &xfer) < 0)
			return false;
		exposure_ = lines;
		return true;
	}

	int exposure_lines() const { return exposure_; }

	/* Read the register back (2-byte address, read flag = bit 15 of the
	 * first address byte) so the manual slider starts where the sensor
	 * really is.  Best effort — a silent sensor keeps the cached value. */
	bool read_exposure_lines(int *lines)
	{
		unsigned char addr[2] = { 0xbe, 0x00 };  /* 0x3e00 | 0x8000 */
		unsigned char data[3] = {};
		struct i2c_msg msgs[2] = {};
		struct i2c_rdwr_ioctl_data xfer = { msgs, 2 };

		msgs[0].addr = kSensorAddr;
		msgs[0].flags = 0;
		msgs[0].len = sizeof(addr);
		msgs[0].buf = addr;
		msgs[1].addr = kSensorAddr;
		msgs[1].flags = I2C_M_RD;
		msgs[1].len = sizeof(data);
		msgs[1].buf = data;

		if (ioctl(fd_, I2C_RDWR, &xfer) < 0)
			return false;

		const unsigned value = (unsigned(data[0]) << 16) |
				       (unsigned(data[1]) << 8) | data[2];
		const int decoded = int(value >> kExposureShift);

		if (decoded <= 0 || decoded > kExposureMax)
			return false;
		exposure_ = decoded;
		if (lines)
			*lines = decoded;
		return true;
	}

private:
	std::string path_;
	int fd_ = -1;
	int exposure_ = kExposureDefault;
};

/* ------------------------ frame analysis (the NPU) ----------------------- */

/* PER-FRAME ANALYSIS MODEL (2026-09-26)
 *
 * ErhaiCam's autofocus and auto-exposure loops are driven by image statistics
 * computed on the Hexagon NPU: an ONNX graph (Sobel/Tenengrad energy + mean
 * luminance + two 10 %-wide clipping windows, see src/gen-cam-stats-onnx.py)
 * executed by ONNX Runtime's QNN execution provider on the HTP.  The helper
 * process (src/erhai-npu-analyze.py, installed at
 * /usr/local/lib/oneplus-erhai-erhaicam/) owns that session and answers one
 * request per analysed frame over a pipe:
 *   app -> helper   "ANALYZE 320 240 76800\n" + 76800 grey bytes
 *   helper -> app   "STAT <sharp> <luma> <hi> <lo>\n"
 * The search/tracking policy (sweep, monitor, controller) stays in the app —
 * only the pixel crunching is offloaded.
 *
 * Three implementations of ONE metric — the graph, the numpy reference in the
 * helper and tenengrad()/stats_cpu() here — so when the NPU is unavailable
 * (no CDSP, no onnxruntime, no fastrpc group, helper died) the app falls back
 * to the CPU and behaves IDENTICALLY apart from speed.  Which backend is live
 * is logged at startup and surfaced in the status line. */

struct FrameStats {
	bool ok = false;
	bool from_npu = false;
	double sharp = 0.0;   /* mean |grad|^2, VALID Sobel, [0,1] grey */
	double luma = 0.0;    /* mean grey */
	double hi = 0.0;      /* mean clip(x, 0.9, 1.0)  -> highlight window */
	double lo = 0.0;      /* mean clip(x, 0.0, 0.1)  -> shadow window */
};

static inline double stats_hi_signal(double hi)
{
	return std::max(hi - 0.9, 0.0);   /* E[(x-0.9)+] */
}

/* Stride-sample a BGRA region into the fixed kStatsW x kStatsH 8-bit grey
 * buffer the analyzer expects.  Decimation (not averaging) keeps the high
 * frequencies the focus metric lives on. */
static void sample_gray(const GstVideoFrame &frame, int x0, int x1, int y0,
			int y1, uint8_t *out)
{
	const int w = GST_VIDEO_INFO_WIDTH(&frame.info);
	const int h = GST_VIDEO_INFO_HEIGHT(&frame.info);
	const uint8_t *base = static_cast<const uint8_t *>(
		GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
	const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);

	x0 = std::clamp(x0, 0, w - 1);
	y0 = std::clamp(y0, 0, h - 1);
	x1 = std::clamp(x1, x0 + 1, w);
	y1 = std::clamp(y1, y0 + 1, h);

	const int sx = std::max(1, (x1 - x0) / kStatsW);
	const int sy = std::max(1, (y1 - y0) / kStatsH);

	for (int j = 0; j < kStatsH; j++) {
		const uint8_t *row = base + size_t(std::min(y0 + j * sy, h - 1)) *
						     stride + size_t(x0) * 4;
		uint8_t *dst = out + size_t(j) * kStatsW;

		for (int i = 0; i < kStatsW; i++) {
			const size_t o = size_t(std::min(x0 + i * sx, w - 1)) * 4;

			dst[i] = uint8_t((row[o + 2] * 77 + row[o + 1] * 150 +
					  row[o] * 29) >> 8);
		}
	}
}

/* Tenengrad over the sampled buffer — reference implementation of the graph's
 * `sharp` output (same kernels, same VALID interior). */
static double tenengrad(const uint8_t *gray)
{
	const double k = 1.0 / 255.0;
	double sum = 0.0;

	for (int y = 0; y + 2 < kStatsH; y++) {
		const uint8_t *r0 = gray + size_t(y) * kStatsW;
		const uint8_t *r1 = r0 + kStatsW;
		const uint8_t *r2 = r1 + kStatsW;

		for (int x = 0; x + 2 < kStatsW; x++) {
			const double a = r0[x] * k, b = r0[x + 1] * k,
				     c = r0[x + 2] * k;
			const double d = r1[x] * k, f = r1[x + 2] * k;
			const double g = r2[x] * k, h = r2[x + 1] * k,
				     i = r2[x + 2] * k;
			const double gx = (c - a) + 2.0 * (f - d) + (i - g);
			const double gy = (g - a) + 2.0 * (h - b) + (i - c);

			sum += gx * gx + gy * gy;
		}
	}
	return sum / double((kStatsW - 2) * (kStatsH - 2));
}

/* CPU fallback: the same four numbers without the NPU. */
static void stats_cpu(const uint8_t *gray, FrameStats *out)
{
	const double k = 1.0 / 255.0;
	double luma = 0.0, hi = 0.0, lo = 0.0;

	for (size_t i = 0; i < size_t(kStatsW) * kStatsH; i++) {
		const double v = gray[i] * k;

		luma += v;
		hi += std::clamp(v, 0.9, 1.0);
		lo += std::clamp(v, 0.0, 0.1);
	}
	const double n = double(kStatsW) * kStatsH;

	out->sharp = tenengrad(gray);
	out->luma = luma / n;
	out->hi = hi / n;
	out->lo = lo / n;
	out->from_npu = false;
	out->ok = true;
}


/* ---------------------- analyzer process (HTP client) -------------------- */

/* Owns the helper process and the pipe protocol.  start() spawns it and
 * returns immediately; poll_ready() (main-loop tick) picks up the READY line
 * so the GUI never waits on the NPU; analyze() runs from the streaming thread
 * and falls back to the CPU while the helper is starting or after it died. */
class Analyzer {
public:
	enum State { Starting = 0, Ready = 1, Dead = -1 };

	static std::string helper_path()
	{
		if (const char *env = std::getenv("ERHAI_CAM_NPU_HELPER"))
			if (*env)
				return env;

		const std::filesystem::path installed =
			"/usr/local/lib/oneplus-erhai-erhaicam/erhai-npu-analyze.py";
		if (std::filesystem::exists(installed))
			return installed.string();

		/* dev tree: walk up from the binary looking for the source tree. */
		std::error_code ec;
		const std::filesystem::path exe =
			std::filesystem::read_symlink("/proc/self/exe", ec);
		if (!ec) {
			for (std::filesystem::path dir = exe.parent_path();
			     dir != dir.root_path(); dir = dir.parent_path()) {
				const std::filesystem::path a = dir /
					"usr/local/lib/oneplus-erhai-erhaicam/erhai-npu-analyze.py";
				const std::filesystem::path b = dir /
					"src/erhai-npu-analyze.py";

				if (std::filesystem::exists(a))
					return a.string();
				if (std::filesystem::exists(b))
					return b.string();
			}
		}
		return {};
	}

	~Analyzer() { stop(); }

	void start()
	{
		if (pid_)
			return;
		if (const char *off = std::getenv("ERHAI_CAM_NPU");
		    off && std::string(off) == "0") {
			state_ = Dead;
			detail_ = "disabled by ERHAI_CAM_NPU=0";
			return;
		}
		const std::string helper = helper_path();
		if (helper.empty()) {
			state_ = Dead;
			detail_ = "helper not found";
			return;
		}
		char *argv[] = { const_cast<char *>("python3"),
				 const_cast<char *>(helper.c_str()), nullptr };
		GError *gerr = nullptr;
		gint in = -1, out = -1;

		if (!g_spawn_async_with_pipes(
			    nullptr, argv, nullptr,
			    static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
						     G_SPAWN_DO_NOT_REAP_CHILD),
			    nullptr, nullptr, &pid_, &in, &out, nullptr, &gerr)) {
			state_ = Dead;
			detail_ = gerr ? gerr->message : "spawn failed";
			g_clear_error(&gerr);
			pid_ = 0;
			return;
		}
		in_ = in;
		out_ = out;
		state_ = Starting;
		detail_ = "starting";
	}

	void stop()
	{
		std::lock_guard lock(lock_);

		if (pid_) {
			if (in_ >= 0)
				(void)!write(in_, "QUIT\n", 5);
			kill(pid_, SIGTERM);
			g_spawn_close_pid(pid_);
			pid_ = 0;
		}
		if (in_ >= 0) {
			close(in_);
			in_ = -1;
		}
		if (out_ >= 0) {
			close(out_);
			out_ = -1;
		}
		if (state_ == Ready || state_ == Starting)
			state_ = Dead;
	}

	State state() const { return state_; }
	const std::string &backend() const { return backend_; }
	const std::string &detail() const { return detail_; }
	unsigned long frames() const { return frames_; }

	/* Main-loop tick while starting: returns true on a state change. */
	bool poll_ready()
	{
		if (state_ != Starting || out_ < 0)
			return false;

		std::lock_guard lock(lock_);
		struct pollfd pfd = { out_, POLLIN, 0 };

		if (poll(&pfd, 1, 0) <= 0)
			return false;
		std::string line;
		if (!read_line(&line, 1000)) {
			state_ = Dead;
			detail_ = "helper exited";
			return true;
		}
		if (line.rfind("READY ", 0) != 0)
			return false;
		backend_ = line.substr(6);
		if (backend_ == "none" || backend_.empty()) {
			state_ = Dead;
			detail_ = "helper has no analyzer backend";
		} else {
			state_ = Ready;
			detail_ = backend_;
		}
		return true;
	}

	bool analyze(const uint8_t *gray, FrameStats *out)
	{
		std::lock_guard lock(lock_);

		if (state_ != Ready)
			return false;

		char head[64];
		const int n = std::snprintf(head, sizeof(head), "ANALYZE %d %d %d\n",
					    kStatsW, kStatsH, kStatsW * kStatsH);

		if (!write_all(head, size_t(n)) ||
		    !write_all(gray, size_t(kStatsW) * kStatsH)) {
			fail("write to the analyzer failed");
			return false;
		}
		std::string line;
		if (!read_line(&line, 3000)) {
			fail("no reply from the analyzer");
			return false;
		}
		if (std::sscanf(line.c_str(), "STAT %lf %lf %lf %lf", &out->sharp,
				&out->luma, &out->hi, &out->lo) != 4) {
			fail(("analyzer error: " + line).c_str());
			return false;
		}
		out->from_npu = true;
		out->ok = true;
		frames_++;
		return true;
	}

private:
	void fail(const char *why)
	{
		std::cerr << "erhai-cam: NPU analyzer stopped (" << why
			  << "); using the CPU metric\n";
		state_ = Dead;
		detail_ = why;
	}

	bool write_all(const void *data, size_t len)
	{
		const uint8_t *p = static_cast<const uint8_t *>(data);

		while (len) {
			const ssize_t n = write(in_, p, len);

			if (n <= 0)
				return false;
			p += n;
			len -= size_t(n);
		}
		return true;
	}

	bool read_line(std::string *line, int timeout_ms)
	{
		for (;;) {
			const size_t nl = rbuf_.find('\n');

			if (nl != std::string::npos) {
				*line = rbuf_.substr(0, nl);
				rbuf_.erase(0, nl + 1);
				return true;
			}
			struct pollfd pfd = { out_, POLLIN, 0 };

			if (poll(&pfd, 1, timeout_ms) <= 0)
				return false;
			char tmp[256];
			const ssize_t n = read(out_, tmp, sizeof(tmp));

			if (n <= 0)
				return false;
			rbuf_.append(tmp, size_t(n));
			if (rbuf_.size() > 4096)
				return false;
		}
	}

	GPid pid_ = 0;
	int in_ = -1;
	int out_ = -1;
	State state_ = Starting;
	std::string backend_ = "starting";
	std::string detail_ = "starting";
	std::string rbuf_;
	unsigned long frames_ = 0;
	std::mutex lock_;
};

/* --------------------------- application state --------------------------- */

enum class AfMode { Idle, Sweep, Monitor };

struct Af {
	bool enabled = true;
	AfMode mode = AfMode::Idle;
	std::vector<int> positions;
	size_t idx = 0;
	int settle = 0;
	std::vector<double> scores;
	int best_pos = 0;
	double best_score = -1.0;
	double best_min = std::numeric_limits<double>::infinity();
	int prev_pos = 0;
	bool fine = false;
	int stride_count = 0;
	std::vector<double> check;
	int low_checks = 0;
	double locked = 0.0;
	Steady::time_point hold_until = Steady::time_point::min();
	bool want_point = false;
	double point_x = 0.0;
	double point_y = 0.0;
	bool one_shot = false;
};

/* Auto exposure: a log-domain proportional controller on the sensor's
 * integration time (there is no gain register we can drive — the app owns
 * exposure only).  Every kAeStride analysed frames the whole preview frame is
 * measured on the NPU and the exposure is nudged by up to kAeMaxStep in log2
 * space; a highlight window over kRawExposureEpsilon vetoes brightening, and
 * the loop never leaves kExposureAeMax so the raw cannot saturate into the
 * soft ISP's inverting region. */
struct Ae {
	bool enabled = true;
	int exposure = kExposureDefault;   /* lines currently programmed */
	int stride = 0;
	int settle = 0;
	bool at_limit = false;
	bool measured = false;            /* at least one analysis completed */
	double last_luma = 0.0;
	double last_hi = 0.0;
};

struct App {
	/* GTK */
	GtkWidget *win = nullptr;
	GtkWidget *preview = nullptr;
	GtkWidget *status = nullptr;
	GtkWidget *btn_switch = nullptr;
	GtkWidget *btn_af = nullptr;
	GtkWidget *btn_ae = nullptr;
	GtkWidget *focus_scale = nullptr;
	GtkWidget *focus_value = nullptr;
	GtkWidget *exp_scale = nullptr;
	GtkWidget *exp_value = nullptr;
	guint slider_timer = 0;
	int pending_exposure = -1;
	int pending_focus = -1;
	bool slider_sync = false;   /* programmatic slider write in flight */
	GtkWidget *btn_shot = nullptr;
	GtkWidget *btn_rec = nullptr;
	guint shutter_flash_id = 0;
	GdkPaintable *paintable = nullptr;
	guint status_timer = 0;
	bool draw_pending = false;
	/* geometry cache for tap mapping */
	double g_scale = 1.0;
	bool g_swap = false;
	int g_rot = 0;
	int g_w = 1;
	int g_h = 1;
	/* GStreamer */
	GstElement *pipe = nullptr;
	GstElement *appsink = nullptr;
	GstElement *rpipe = nullptr;
	GstElement *rsrc = nullptr;
	bool recording = false;
	std::string rec_path;
	/* latest frame (owned, protected) */
	std::mutex frame_lock;
	GstSample *last_sample = nullptr;
	/* camera */
	std::string camera = kRearCamera;
	bool rear = true;
	bool rear_ok = false;
	bool camera_ok = false;
	guint watchdog = 0;
	int stream_w = 0;
	int stream_h = 0;
	/* orientation */
	int orient = 0;   /* net photo rotation, degrees cw */
	int sensor_angle = 0;
	int mutter_transform = 0;   /* logical monitor transform, 0 = normal */
	bool transform_ok = false;  /* mutter DisplayConfig reachable */
	guint transform_requery = 0;
	/* debug: override the counter-rotation applied by the preview widget
	 * (-1 = auto from mutter_transform).  Cycle with the 'o' key. */
	int rot_override = -1;
	int rot_override_idx = 0;
	/* AF */
	Lens *lens = nullptr;
	bool lens_ok = false;
	Af af;
	/* exposure + the NPU frame analyzer */
	Sensor *sensor = nullptr;
	bool sensor_ok = false;
	Ae ae;
	Analyzer analyzer;
	guint analyzer_poll = 0;
	bool analyzer_error_shown = false;
	std::array<uint8_t, size_t(kStatsW) * kStatsH> stats_buf{};
	/* headless */
	bool headless = false;
	bool no_af = false;
	bool ae_probe = false;      /* log every AE update (probes) */
	int ae_probe_seconds = 0;
	bool once = false;
	bool shot = false;
	bool rec_started = false;
	int record_seconds = 0;
	int headless_ticks = 0;
	std::string out_path;
	GMainLoop *loop = nullptr;
	bool quit_when_af_done = false;
};

static App *g_app = nullptr;

/* ------------------------------ UI helpers ------------------------------- */
/* appsink callbacks run in the streaming thread; marshal GTK calls. */

struct StatusMsg {
	App *app;
	std::string text;
};

static gboolean status_show_cb(gpointer data)
{
	StatusMsg *msg = static_cast<StatusMsg *>(data);

	if (!msg->app->status) {
		delete msg;
		return G_SOURCE_REMOVE;
	}
	if (msg->app->status_timer)
		g_source_remove(msg->app->status_timer);
	gtk_label_set_text(GTK_LABEL(msg->app->status), msg->text.c_str());
	gtk_widget_set_visible(msg->app->status, TRUE);
	msg->app->status_timer = g_timeout_add(
		4000, [](gpointer d) -> gboolean {
			auto *app = static_cast<App *>(d);
			app->status_timer = 0;
			gtk_label_set_text(GTK_LABEL(app->status), "");
			gtk_widget_set_visible(app->status, FALSE);
			return G_SOURCE_REMOVE;
		}, msg->app);
	delete msg;
	return G_SOURCE_REMOVE;
}

static void set_status(App *app, const std::string &text)
{
	g_main_context_invoke(nullptr, status_show_cb,
			      new StatusMsg{ app, text });
}

/* ------------------------------- autofocus ------------------------------- */

static int af_position(const App *app)
{
	return app->lens ? app->lens->position() : 0;
}

static void lens_set(App *app, int pos)
{
	if (app->lens_ok)
		app->lens->set_focus(pos);
}

static void start_sweep(App *app, const std::vector<int> &positions,
			bool fine)
{
	Af &af = app->af;

	af.mode = AfMode::Sweep;
	af.positions = positions;
	af.idx = 0;
	af.settle = 0;
	af.scores.clear();
	af.best_pos = positions.front();
	af.best_score = -1.0;
	af.best_min = std::numeric_limits<double>::infinity();
	af.fine = fine;
	lens_set(app, positions.front());
	if (!app->headless)
		set_status(app, "Autofocus scanning\u2026");
}

static void autofocus_start(App *app)
{
	Af &af = app->af;
	static const std::vector<int> coarse = {
		0, 128, 256, 384, 512, 640, 768, 896, kFocusMax
	};

	af.prev_pos = af_position(app);
	start_sweep(app, coarse, false);
}

static void af_finish_sweep(App *app)
{
	Af &af = app->af;

	if (!af.fine) {
		/* Coarse done: check the scene offered usable detail. */
		if (af.best_score - af.best_min < kAfMinDetail ||
		    af.best_score < af.best_min * 1.15) {
			lens_set(app, af.prev_pos);
			af.mode = af.one_shot ? AfMode::Idle : AfMode::Monitor;
			af.one_shot = false;
			af.locked = std::max(af.best_score, 0.0);
			if (app->once)
				app->quit_when_af_done = true;
			if (!app->headless)
				set_status(app, "AF: no detail to lock on");
			return;
		}

		std::set<int> fine_set;
		for (int offset = -96; offset <= 96; offset += 32)
			fine_set.insert(std::clamp(af.best_pos + offset,
						   kFocusMin, kFocusMax));
		start_sweep(app,
			    std::vector<int>(fine_set.begin(), fine_set.end()),
			    true);
		return;
	}

	/* Fine done: lock. */
	lens_set(app, af.best_pos);
	af.mode = af.one_shot ? AfMode::Idle : AfMode::Monitor;
	af.one_shot = false;
	af.locked = af.best_score;
	af.hold_until = Steady::now() +
		std::chrono::duration_cast<Steady::duration>(
			std::chrono::duration<double>(kTapHoldSeconds));
	if (app->once)
		app->quit_when_af_done = true;
	if (!app->headless)
		set_status(app, "AF locked at focus=" +
				     std::to_string(af.best_pos));
}

static double af_frame_score(App *app, const GstVideoFrame &frame)
{
	const int w = GST_VIDEO_INFO_WIDTH(&frame.info);
	const int h = GST_VIDEO_INFO_HEIGHT(&frame.info);
	int x0, x1, y0, y1;
	Af &af = app->af;

	if (af.want_point) {
		const int rw = w * 3 / 10;
		const int rh = h * 3 / 10;
		const int cx = std::clamp(int(std::lround(af.point_x)), 0, w - 1);
		const int cy = std::clamp(int(std::lround(af.point_y)), 0, h - 1);

		x0 = std::clamp(cx - rw / 2, 1, w - rw - 1);
		y0 = std::clamp(cy - rh / 2, 1, h - rh - 1);
		x1 = x0 + rw;
		y1 = y0 + rh;
	} else {
		x0 = w / 5;
		x1 = w - x0;
		y0 = h / 5;
		y1 = h - y0;
	}

	uint8_t *buf = app->stats_buf.data();
	FrameStats stats;

	sample_gray(frame, x0, x1, y0, y1, buf);
	if (app->analyzer.analyze(buf, &stats))
		return stats.sharp;

	stats_cpu(buf, &stats);
	return stats.sharp;
}

/* --------------------- manual controls / AE-AF buttons ------------------- */

/* ONE place decides how the AF/AE buttons and the two manual sliders look:
 * each button is a toggle whose "off" state summons the matching vertical
 * slider underneath, and a slider is only usable while the camera it drives
 * is streaming (the VCM rails and the sensor register only exist then). */
static void app_sync_controls(App *app)
{
	if (app->btn_ae) {
		gtk_widget_set_sensitive(app->btn_ae, app->rear && app->sensor_ok);
		if (app->ae.enabled) {
			gtk_widget_add_css_class(app->btn_ae, "on");
			gtk_widget_remove_css_class(app->btn_ae, "off");
		} else {
			gtk_widget_add_css_class(app->btn_ae, "off");
			gtk_widget_remove_css_class(app->btn_ae, "on");
		}
	}
	if (app->btn_af) {
		gtk_widget_set_sensitive(app->btn_af, app->rear && app->lens_ok);
		if (app->af.enabled) {
			gtk_widget_add_css_class(app->btn_af, "on");
			gtk_widget_remove_css_class(app->btn_af, "off");
		} else {
			gtk_widget_add_css_class(app->btn_af, "off");
			gtk_widget_remove_css_class(app->btn_af, "on");
		}
	}

	const bool exp_manual = app->sensor_ok && app->rear && !app->ae.enabled;
	if (app->exp_scale) {
		if (exp_manual) {
			app->slider_sync = true;
			gtk_range_set_value(GTK_RANGE(app->exp_scale),
					    app->ae.exposure);
			gtk_label_set_text(GTK_LABEL(app->exp_value),
					   std::to_string(app->ae.exposure).c_str());
			app->slider_sync = false;
		}
		gtk_widget_set_visible(app->exp_scale, exp_manual);
		gtk_widget_set_visible(app->exp_value, exp_manual);
	}

	const bool focus_manual = app->lens_ok && app->rear && !app->af.enabled;
	if (app->focus_scale) {
		if (focus_manual) {
			const int pos = af_position(app);

			app->slider_sync = true;
			gtk_range_set_value(GTK_RANGE(app->focus_scale), pos);
			gtk_label_set_text(GTK_LABEL(app->focus_value),
					   std::to_string(pos).c_str());
			app->slider_sync = false;
		}
		gtk_widget_set_visible(app->focus_scale, focus_manual);
		gtk_widget_set_visible(app->focus_value, focus_manual);
	}
}

static gboolean sync_controls_cb(gpointer ud)
{
	app_sync_controls(static_cast<App *>(ud));
	return G_SOURCE_REMOVE;
}

/* Safe from the streaming thread (AE can switch itself off on a write
 * failure). */
static void app_sync_controls_async(App *app)
{
	g_main_context_invoke(nullptr, sync_controls_cb, app);
}


static void af_step(App *app, GstSample *sample)
{
	Af &af = app->af;

	if (!af.enabled || !app->lens_ok || !app->rear)
		return;
	if (af.mode == AfMode::Idle)
		return;

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	if (!buffer)
		return;

	GstVideoInfo info;
	GstVideoFrame frame;
	if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
	    !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
		return;

	if (af.mode == AfMode::Sweep) {
		if (af.settle < kSettleFrames) {
			af.settle++;
		} else {
			af.scores.push_back(af_frame_score(app, frame));
			if (af.scores.size() >= kScoreFrames) {
				std::vector<double> sorted = af.scores;
				std::sort(sorted.begin(), sorted.end());
				const double score =
					sorted[sorted.size() / 2];

				af.best_min = std::min(af.best_min, score);
				if (score > af.best_score) {
					af.best_score = score;
					af.best_pos = af.positions[af.idx];
				}
				if (app->headless)
					std::cout << "erhai-cam: af pos="
						  << af.positions[af.idx]
						  << " score=" << score << "\n";

				af.idx++;
				af.settle = 0;
				af.scores.clear();
				if (af.idx < af.positions.size()) {
					lens_set(app, af.positions[af.idx]);
				} else {
					af_finish_sweep(app);
				}
			}
		}
	} else if (af.mode == AfMode::Monitor) {
		if (++af.stride_count >= kMonitorStride) {
			af.stride_count = 0;
			if (Steady::now() >= af.hold_until) {
				af.check.push_back(af_frame_score(app, frame));
				if (af.check.size() >= kMonitorFrames) {
					std::vector<double> sorted = af.check;
					std::sort(sorted.begin(),
						  sorted.end());
					const double score =
						sorted[sorted.size() / 2];
					af.check.clear();

					if (score < af.locked * 0.55) {
						if (++af.low_checks >=
						    kLowChecks) {
							af.low_checks = 0;
							autofocus_start(app);
						}
					} else {
						af.low_checks = 0;
						af.locked = af.locked * 0.95 +
							    score * 0.05;
					}
				}
			}
		}
	}

	gst_video_frame_unmap(&frame);
}

/* ---------------------------- auto exposure ------------------------------ */

static bool apply_exposure(App *app, int lines)
{
	if (!app->sensor_ok || !app->camera_ok)
		return false;
	return app->sensor->set_exposure_lines(lines);
}

/* One auto-exposure control step, driven by the NPU's luminance statistics:
 * a log-domain proportional controller on the sensor's integration time.
 * There is no gain register we can drive (the sc1320cs analogue-gain control
 * is as no-op as the exposure one), so brightness is integration time only;
 * that is why the loop is multiplicative — exposure is linear in brightness
 * and a fixed log step is a constant visual step.
 *
 * Runs on the streaming thread for every frame; only every kAeStride-th frame
 * does any work, and a change is followed by kAeSettle frames of silence so
 * the sensor/ISP pipeline can settle before the next measurement. */
static void ae_step(App *app, GstSample *sample)
{
	Ae &ae = app->ae;

	if (!ae.enabled || !app->sensor_ok || !app->rear || !app->camera_ok)
		return;
	if (++ae.stride < kAeStride)
		return;
	ae.stride = 0;
	if (ae.settle > 0) {
		ae.settle--;
		return;
	}

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	if (!buffer)
		return;
	GstVideoInfo info;
	GstVideoFrame frame;
	if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
	    !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
		return;

	uint8_t *buf = app->stats_buf.data();
	FrameStats stats;

	sample_gray(frame, 0, GST_VIDEO_INFO_WIDTH(&frame.info), 0,
		    GST_VIDEO_INFO_HEIGHT(&frame.info), buf);
	gst_video_frame_unmap(&frame);
	if (!app->analyzer.analyze(buf, &stats)) {
		if (app->analyzer.state() == Analyzer::Dead && !app->headless &&
		    !app->analyzer_error_shown) {
			app->analyzer_error_shown = true;
			set_status(app, "NPU analyzer unavailable — "
					"focus/exposure metrics on the CPU");
		}
		stats_cpu(buf, &stats);
	}

	ae.measured = true;
	ae.last_luma = stats.luma;
	ae.last_hi = stats_hi_signal(stats.hi);

	const double luma = std::clamp(stats.luma, 0.004, 1.0);
	const double err = std::log2(kAeTarget / luma);
	double step = std::clamp(err * kAeGain, -kAeMaxStep, kAeMaxStep);

	if (std::fabs(err) < kAeDeadband)
		step = 0.0;                       /* close enough: hold */
	if (ae.last_hi > kRawExposureEpsilon && step > 0.0)
		step = 0.0;                       /* highlights are washing out */
	if (step < 0.0 && ae.exposure <= kExposureMin)
		step = 0.0;

	int next = ae.exposure;
	if (step != 0.0) {
		next = std::clamp(int(std::lround(ae.exposure * std::exp2(step))),
				  kExposureMin, kExposureAeMax);
		if (next == ae.exposure)   /* rounding at the very bottom */
			next = std::clamp(ae.exposure + (step > 0.0 ? 1 : -1),
					  kExposureMin, kExposureAeMax);
	}
	if (next != ae.exposure) {
		if (apply_exposure(app, next)) {
			ae.exposure = next;
			ae.settle = kAeSettle;
		} else {
			ae.enabled = false;
			std::cerr << "erhai-cam: exposure write failed; "
				     "auto exposure off\n";
			set_status(app, "Auto exposure off: exposure write failed "
					"(is a camera streaming?)");
			app_sync_controls_async(app);
		}
	}

	const bool dark_end = ae.exposure >= kExposureAeMax && err > kAeDeadband;
	const bool bright_end = ae.exposure <= kExposureMin && err < -kAeDeadband;

	ae.at_limit = dark_end || bright_end;
	if (app->headless || app->ae_probe) {
		std::cout << "erhai-cam: ae luma=" << stats.luma
			  << " hi=" << ae.last_hi << " lo=" << stats.lo
			  << " err=" << err << " exp=" << ae.exposure
			  << (ae.at_limit ? " (limit)" : "")
			  << (stats.from_npu ? " npu" : " cpu") << "\n";
	}
}

static std::string timestamp_name(const char *prefix, const char *ext)
{
	char buf[64];
	std::time_t t = std::time(nullptr);
	std::tm tm{};
	localtime_r(&t, &tm);
	std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
	return std::string(prefix) + "_" + buf + ext;
}

static std::filesystem::path media_dir(GUserDirectory special, const char *fb)
{
	const char *dir = g_get_user_special_dir(special);
	std::filesystem::path base = dir ? std::filesystem::path(dir)
					: std::filesystem::path(g_get_home_dir()) / fb;
	std::filesystem::path out = base / "ErhaiCam";
	std::error_code ec;
	std::filesystem::create_directories(out, ec);
	return out;
}

/* glycin-backed JPEG saver rejects RGBA pixbufs — copy to plain RGB. */
static GdkPixbuf *pixbuf_to_rgb(GdkPixbuf *src)
{
	if (!gdk_pixbuf_get_has_alpha(src)) {
		g_object_ref(src);
		return src;
	}

	const int w = gdk_pixbuf_get_width(src);
	const int h = gdk_pixbuf_get_height(src);
	GdkPixbuf *dst = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
	if (!dst)
		return nullptr;

	const uint8_t *sp = gdk_pixbuf_read_pixels(src);
	const int ss = gdk_pixbuf_get_rowstride(src);
	const int sc = gdk_pixbuf_get_n_channels(src);
	uint8_t *dp = gdk_pixbuf_get_pixels(dst);
	const int ds = gdk_pixbuf_get_rowstride(dst);

	for (int y = 0; y < h; y++) {
		const uint8_t *srow = sp + size_t(y) * ss;
		uint8_t *drow = dp + size_t(y) * ds;
		for (int x = 0; x < w; x++) {
			drow[x * 3] = srow[x * sc];
			drow[x * 3 + 1] = srow[x * sc + 1];
			drow[x * 3 + 2] = srow[x * sc + 2];
		}
	}
	return dst;
}

/* Insert a minimal EXIF APP1 Orientation marker into a JPEG file.
 * The 36-byte APP1 segment is written right after the SOI (FFD8) marker.
 * Skips insertion if the file already starts with an APP1 segment (i.e.
 * has EXIF data) to avoid duplicating the tag. */
static void insert_exif_orientation(const std::string &path, int orientation)
{
	if (orientation < 1 || orientation > 8)
		return;

	std::ifstream in(path, std::ios::binary);
	if (!in)
		return;
	std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)),
					std::istreambuf_iterator<char>());
	in.close();

	if (buf.size() < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
		return;  /* not a JPEG */

	/* Skip if an APP1 segment already follows SOI */
	if (buf.size() > 4 && buf[2] == 0xFF && buf[3] == 0xE1)
		return;

	unsigned char exif[36] = {
		0xFF, 0xE1,                      /* APP1 marker */
		0x00, 0x22,                      /* segment length: 34 */
		'E', 'x', 'i', 'f', 0x00, 0x00,  /* Exif\0\0 identifier */
		/* TIFF header, little-endian ("II") */
		'I', 'I', 0x2A, 0x00,            /* magic 42 */
		0x08, 0x00, 0x00, 0x00,          /* IFD0 offset = 8 */
		/* IFD0 */
		0x01, 0x00,                      /* 1 entry */
		0x12, 0x01,                      /* tag 0x0112 = Orientation */
		0x03, 0x00,                      /* type 3 = SHORT */
		0x01, 0x00, 0x00, 0x00,          /* count 1 */
		0x00, 0x00, 0x00, 0x00,          /* value (SHORT) */
		0x00, 0x00, 0x00, 0x00           /* next IFD = 0 */
	};
	/* SHORT value sits in the first 2 bytes of the 4-byte value field */
	exif[28] = (unsigned char)(orientation & 0xFF);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return;
	out.put((char)0xFF);
	out.put((char)0xD8);
	out.write(reinterpret_cast<const char *>(exif), sizeof(exif));
	out.write(reinterpret_cast<const char *>(buf.data() + 2),
		  buf.size() - 2);
}

/* Save a BGRA sample as JPEG — never pixel-rotated (the chassis-fixed
 * frame is the world record).  An EXIF Orientation flag is written from
 * the physical device rotation (sensor_angle) so viewers display the
 * photo upright: the raw frame is world-rotated by -sensor_angle, so the
 * viewer must rotate +sensor_angle to compensate.
 *   sensor 0   -> EXIF 1 (normal)
 *   sensor 90  -> EXIF 6 (rotate 90 cw)
 *   sensor 180 -> EXIF 3 (rotate 180)
 *   sensor 270 -> EXIF 8 (rotate 270 cw = 90 ccw) */
static bool save_photo(App *app, std::string *out_path, std::string *err)
{
	GstSample *sample;
	{
		std::lock_guard lock(app->frame_lock);
		if (!app->last_sample) {
			*err = "no camera frame yet";
			return false;
		}
		sample = gst_sample_ref(app->last_sample);
	}

	bool ok = false;
	GstVideoInfo info;
	GstVideoFrame frame;
	GstBuffer *buffer = gst_sample_get_buffer(sample);

	if (!buffer || !gst_video_info_from_caps(&info,
						 gst_sample_get_caps(sample)) ||
	    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA ||
	    !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
		*err = "cannot map camera frame";
		gst_sample_unref(sample);
		return false;
	}

	const int w = GST_VIDEO_INFO_WIDTH(&info);
	const int h = GST_VIDEO_INFO_HEIGHT(&info);
	(void)app;
	const int rot = 0;
	const int dw = (rot == 90 || rot == 270) ? h : w;
	const int dh = (rot == 90 || rot == 270) ? w : h;

	cairo_surface_t *src = cairo_image_surface_create_for_data(
		static_cast<uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
		CAIRO_FORMAT_ARGB32, w, h, GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
	cairo_surface_t *dest = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							   dw, dh);
	cairo_t *cr = cairo_create(dest);

	switch (rot) {
	case 90:
		cairo_translate(cr, dh, 0);
		cairo_rotate(cr, M_PI / 2);
		break;
	case 180:
		cairo_translate(cr, dw, dh);
		cairo_rotate(cr, M_PI);
		break;
	case 270:
		cairo_translate(cr, 0, dw);
		cairo_rotate(cr, 3 * M_PI / 2);
		break;
	default:
		break;
	}
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);

	GdkPixbuf *rgba = gdk_pixbuf_get_from_surface(dest, 0, 0, dw, dh);
	GdkPixbuf *pix = rgba ? pixbuf_to_rgb(rgba) : nullptr;
	if (pix) {
		std::filesystem::path path =
			media_dir(G_USER_DIRECTORY_PICTURES, "Pictures") /
			timestamp_name("IMG", ".jpg");
		GError *gerr = nullptr;
		if (gdk_pixbuf_save(pix, path.c_str(), "jpeg", &gerr,
				    "quality", "92", nullptr)) {
			ok = true;
			if (out_path)
				*out_path = path.string();
			/* Tag the physical device rotation so viewers display
			 * the photo upright (see insert_exif_orientation). */
			int exif_orient = 1;
			switch (app->sensor_angle) {
			case 90:  exif_orient = 6; break;
			case 180: exif_orient = 3; break;
			case 270: exif_orient = 8; break;
			default:  exif_orient = 1; break;
			}
			insert_exif_orientation(path.string(), exif_orient);
		} else if (err) {
			*err = gerr ? gerr->message : "cannot save JPEG";
		}
		if (gerr)
			g_error_free(gerr);
		g_object_unref(pix);
	} else if (err) {
		*err = "cannot convert frame";
	}
	if (rgba)
		g_object_unref(rgba);

	cairo_surface_destroy(dest);
	cairo_surface_destroy(src);
	gst_video_frame_unmap(&frame);
	gst_sample_unref(sample);
	return ok;
}

/* Flash the shutter button red for 250 ms after a snapshot is taken. */
static gboolean shutter_flash_off(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->btn_shot)
		gtk_widget_remove_css_class(app->btn_shot, "flash");
	app->shutter_flash_id = 0;
	return G_SOURCE_REMOVE;
}

static void shutter_flash(App *app)
{
	if (!app->btn_shot)
		return;
	if (app->shutter_flash_id) {
		g_source_remove(app->shutter_flash_id);
		app->shutter_flash_id = 0;
	}
	gtk_widget_add_css_class(app->btn_shot, "flash");
	app->shutter_flash_id = g_timeout_add(250, shutter_flash_off, app);
}

static void app_take_photo(App *app)
{
	std::string path, err;

	if (save_photo(app, &path, &err)) {
		set_status(app, "Saved " + path);
		shutter_flash(app);
	} else
		set_status(app, "Photo failed: " + err);
}

/* ------------------------------- recording ------------------------------- */

static gboolean rec_bus_cb(GstBus *, GstMessage *msg, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
		if (app->rpipe) {
			gst_element_set_state(app->rpipe, GST_STATE_NULL);
			gst_object_unref(app->rpipe);
			app->rpipe = nullptr;
			app->rsrc = nullptr;
		}
		app->recording = false;
		if (app->btn_rec) {
			gtk_button_set_label(GTK_BUTTON(app->btn_rec), "Record");
			gtk_widget_remove_css_class(app->btn_rec, "recording");
		}
		set_status(app, "Saved " + app->rec_path);
		if (app->headless && app->record_seconds > 0)
			g_main_loop_quit(app->loop);
		return G_SOURCE_REMOVE;
	}
	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
		GError *gerr = nullptr;
		gchar *dbg = nullptr;
		gst_message_parse_error(msg, &gerr, &dbg);
		set_status(app, std::string("Recording failed: ") +
				     (gerr ? gerr->message : "?"));
		if (gerr)
			g_error_free(gerr);
		g_free(dbg);
		if (app->rpipe) {
			gst_element_set_state(app->rpipe, GST_STATE_NULL);
			gst_object_unref(app->rpipe);
			app->rpipe = nullptr;
			app->rsrc = nullptr;
		}
		app->recording = false;
		if (app->btn_rec) {
			gtk_button_set_label(GTK_BUTTON(app->btn_rec), "Record");
			gtk_widget_remove_css_class(app->btn_rec, "recording");
		}
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void app_record_stop(App *app)
{
	if (!app->recording)
		return;
	app->recording = false;
	if (app->btn_rec) {
		gtk_button_set_label(GTK_BUTTON(app->btn_rec), "Record");
		gtk_widget_remove_css_class(app->btn_rec, "recording");
	}
	if (app->rsrc)
		gst_app_src_end_of_stream(GST_APP_SRC(app->rsrc));
	/* rpipe is finalized by the EOS/error bus callback. */
}

static void app_record_start(App *app)
{
	GstCaps *caps;
	GstPad *pad = gst_element_get_static_pad(app->appsink, "sink");

	caps = pad ? gst_pad_get_current_caps(pad) : nullptr;
	if (pad)
		gst_object_unref(pad);
	if (!caps) {
		set_status(app, "Recording failed: no stream caps");
		return;
	}

	std::filesystem::path base =
		media_dir(G_USER_DIRECTORY_VIDEOS, "Videos");

	static const struct {
		const char *enc;
		const char *parse;
		const char *mux;
		const char *ext;
	} candidates[] = {
		{ "v4l2h264enc", "h264parse", "mp4mux", ".mp4" },
		{ "vp8enc", nullptr, "webmmux", ".webm" },
	};

	for (const auto &c : candidates) {
		if (!gst_element_factory_find(c.enc) ||
		    !gst_element_factory_find(c.mux))
			continue;

		std::string desc = std::string("appsrc name=src do-timestamp=true format=time ! videoconvert ! video/x-raw,format=I420 ! ") +
				   c.enc + (c.parse ? std::string(" ! ") + c.parse : "") +
				   " ! " + c.mux + " ! filesink location=\"" +
				   (base / timestamp_name("VID", c.ext)).string() + "\"";

		GError *gerr = nullptr;
		GstElement *rpipe = gst_parse_launch(desc.c_str(), &gerr);
		if (gerr || !rpipe) {
			g_clear_error(&gerr);
			continue;
		}

		GstElement *src = gst_bin_get_by_name(GST_BIN(rpipe), "src");
		g_object_set(src, "caps", caps, nullptr);
		gst_app_src_set_stream_type(GST_APP_SRC(src),
					    GST_APP_STREAM_TYPE_STREAM);

		app->rec_path = (base / timestamp_name("VID", c.ext)).string();
		gst_bus_add_watch(gst_element_get_bus(rpipe), rec_bus_cb, app);
		if (gst_element_set_state(rpipe, GST_STATE_PLAYING) ==
		    GST_STATE_CHANGE_FAILURE) {
			gst_object_unref(src);
			gst_object_unref(rpipe);
			continue;
		}

		app->rpipe = rpipe;
		app->rsrc = src;
		app->recording = true;
		if (app->btn_rec) {
			gtk_button_set_label(GTK_BUTTON(app->btn_rec), "Stop");
			gtk_widget_add_css_class(app->btn_rec, "recording");
		}
		set_status(app, "Recording\u2026");
		gst_caps_unref(caps);
		return;
	}

	gst_caps_unref(caps);
	set_status(app, "Recording failed: no usable encoder");
}

static void app_record_toggle(App *app)
{
	if (!app->camera_ok || !app->appsink) {
		set_status(app, "Camera is not running");
		return;
	}
	if (app->recording)
		app_record_stop(app);
	else
		app_record_start(app);
}

/* ---------------------------- preview pipeline --------------------------- */

static bool preview_pipeline_start(App *app);

static gboolean preview_redraw_cb(gpointer ud);
static void paintable_invalidate_cb(GdkPaintable *, gpointer ud);

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer ud)
{
	auto *app = static_cast<App *>(ud);
	GstSample *sample = gst_app_sink_pull_sample(sink);

	if (!sample)
		return GST_FLOW_ERROR;

	{
		std::lock_guard lock(app->frame_lock);
		if (app->last_sample)
			gst_sample_unref(app->last_sample);
		app->last_sample = gst_sample_ref(sample);
	}

	if (!app->camera_ok) {
		app->camera_ok = true;
		if (app->rear)
			app->rear_ok = true;
		if (app->watchdog) {
			g_source_remove(app->watchdog);
			app->watchdog = 0;
		}
		if (app->af.enabled && app->lens_ok && app->rear)
			autofocus_start(app);
		/* The sensor resets to its power-on default when the stream
		 * starts: re-assert the manual exposure if AE is off. */
		if (!app->ae.enabled)
			apply_exposure(app, app->ae.exposure);
		if (!app->rear && !app->headless)
			set_status(app, "Front camera");
		app_sync_controls_async(app);
	}

	if (app->recording && app->rsrc) {
		GstBuffer *buffer = gst_buffer_ref(gst_sample_get_buffer(sample));
		gst_app_src_push_buffer(GST_APP_SRC(app->rsrc), buffer);
	}

	af_step(app, sample);
	ae_step(app, sample);
	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

static void preview_pipeline_stop(App *app)
{
	if (app->pipe) {
		gst_element_set_state(app->pipe, GST_STATE_NULL);
		gst_object_unref(app->pipe);
		app->pipe = nullptr;
	}
	if (app->appsink) {
		gst_object_unref(app->appsink);
		app->appsink = nullptr;
	}
	if (app->paintable) {
		g_object_unref(app->paintable);
		app->paintable = nullptr;
	}
	std::lock_guard lock(app->frame_lock);
	if (app->last_sample) {
		gst_sample_unref(app->last_sample);
		app->last_sample = nullptr;
	}
}

static gboolean camera_watchdog_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app->watchdog = 0;
	if (app->camera_ok)
		return G_SOURCE_REMOVE;

	if (!app->rear && app->rear_ok) {
		set_status(app, "Front camera did not stream — back to rear");
		app->rear = true;
		app->camera = kRearCamera;
		preview_pipeline_stop(app);
		preview_pipeline_start(app);
	} else {
		set_status(app, "Camera did not start");
	}
	return G_SOURCE_REMOVE;
}

static bool preview_pipeline_start(App *app)
{
	static GstAppSinkCallbacks cbs = { nullptr, nullptr, on_new_sample,
					   nullptr, nullptr };

	for (const auto &[w, h] :
	     (app->rear ? kRearResolutions : kFrontResolutions)) {
		/* OWNER DECISION (REVISED 2026-09-07 ~18:30, supersedes
		 * the "fully honest" choice of this morning): MIRRORED
		 * preview, HONEST capture — the front preview is flipped
		 * so it moves with the user like every phone camera, but
		 * photos and video are saved UN-flipped (what an outside
		 * viewer sees; text readable).  The flip therefore lives
		 * on the PREVIEW BRANCH ONLY, after the tee; the appsink
		 * branch (photo frames, video frames, AF) stays raw.
		 * Rear stays raw everywhere. */
		const char *mirror = app->rear ? "" :
			"videoflip method=horizontal-flip ! ";
		std::string desc =
			"libcamerasrc camera-name=\"" + app->camera +
			"\" ! queue ! videoconvert ! "
			"video/x-raw,format=BGRA,width=" +
			std::to_string(w) + ",height=" + std::to_string(h) +
			" ! tee name=t "
			"t. ! queue leaky=downstream max-size-buffers=2 ! " +
			std::string(mirror) +
			"gtk4paintablesink name=preview "
			"t. ! queue leaky=downstream max-size-buffers=1 ! "
			"appsink name=frames emit-signals=false drop=true "
			"sync=false";
		/* Ground truth: log the exact pipeline in use. */
		std::cerr << "erhai-cam: pipe[" << w << "x" << h
			  << "]: " << desc << "\n";

		GError *gerr = nullptr;
		GstElement *pipe = gst_parse_launch(desc.c_str(), &gerr);
		if (gerr || !pipe) {
			std::cerr << "pipeline error: "
				  << (gerr ? gerr->message : "?") << "\n";
			g_clear_error(&gerr);
			continue;
		}

		GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "frames");
		GstElement *psink = gst_bin_get_by_name(GST_BIN(pipe), "preview");
		gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, app, nullptr);
		g_object_get(psink, "paintable", &app->paintable, nullptr);
		if (app->paintable) {
			g_signal_connect(app->paintable, "invalidate-contents",
					 G_CALLBACK(paintable_invalidate_cb),
					 app);
			g_signal_connect(app->paintable, "invalidate-size",
					 G_CALLBACK(paintable_invalidate_cb),
					 app);
		}

		if (gst_element_set_state(pipe, GST_STATE_PLAYING) !=
		        GST_STATE_CHANGE_FAILURE) {
			GstState state = GST_STATE_NULL;
			gst_element_get_state(pipe, &state, nullptr,
					      3 * GST_SECOND);
			if (state == GST_STATE_PLAYING) {
				app->pipe = pipe;
				app->appsink = sink;
				app->stream_w = w;
				app->stream_h = h;
				gst_object_unref(psink);
				app->camera_ok = false;
				app->watchdog = g_timeout_add(
					5000, camera_watchdog_cb, app);
				return true;
			}
		}

		std::cerr << "cannot start stream at " << w << "x" << h
			  << ", trying next resolution\n";
		gst_object_unref(psink);
		gst_object_unref(sink);
		gst_element_set_state(pipe, GST_STATE_NULL);
		gst_object_unref(pipe);
		if (app->paintable) {
			g_object_unref(app->paintable);
			app->paintable = nullptr;
		}
	}

	set_status(app, "Cannot start the camera");
	return false;
}

static void app_switch_camera(App *app)
{
	app->af.mode = AfMode::Idle;
	app->rear = !app->rear;
	app->camera = app->rear ? kRearCamera : kFrontCamera;
	preview_pipeline_stop(app);
	gtk_widget_queue_draw(app->preview);
	if (!preview_pipeline_start(app)) {
		/* Fall back to whatever worked before. */
		app->rear = !app->rear;
		app->camera = app->rear ? kRearCamera : kFrontCamera;
		preview_pipeline_start(app);
	}
	/* AE/AF and the manual sliders are rear-camera features. */
	app_sync_controls(app);
}

/* ------------------------------ orientation ------------------------------ */

static int sensor_angle(App *app)
{
	static const struct {
		const char *name;
		int angle;
	} table[] = {
		{ "normal", 0 },   { "left-up", 90 },
		{ "bottom-up", 180 }, { "right-up", 270 },
	};

	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr,
					      nullptr);
	if (!bus)
		return 0;

	GVariant *result = g_dbus_connection_call_sync(
		bus, "net.hadess.SensorProxy", "/net/hadess/SensorProxy",
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new("(ss)", "net.hadess.SensorProxy",
			      "AccelerometerOrientation"),
		G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
		nullptr);
	int angle = 0;
	if (result) {
		GVariant *inner;
		g_variant_get(result, "(v)", &inner);
		const char *value = g_variant_get_string(inner, nullptr);
		for (const auto &e : table)
			if (std::strcmp(value, e.name) == 0)
				angle = e.angle;
		g_variant_unref(inner);
		g_variant_unref(result);
	} else {
		/* No proxy access: fall back to Shell-owned cached value. */
		angle = app->sensor_angle;
	}
	g_object_unref(bus);
	return angle;
}

/*
 * Mutter logical-monitor transform, from org.gnome.Mutter.DisplayConfig
 * GetCurrentState: reply child 2 is the logical-monitors array, each entry
 * is (x i, y i, scale d, transform u, primary b, monitors ...) — transform
 * 0 = normal, 1/2/3 = 90/180/270 cw, 4..7 = flipped variants.  Returns -1
 * when the state cannot be read (keep the previous value then).
 */
static int mutter_transform_query(void)
{
	GError *gerr = nullptr;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr,
					      &gerr);
	if (!bus) {
		g_clear_error(&gerr);
		return -1;
	}

	GVariant *result = g_dbus_connection_call_sync(
		bus, "org.gnome.Mutter.DisplayConfig",
		"/org/gnome/Mutter/DisplayConfig",
		"org.gnome.Mutter.DisplayConfig", "GetCurrentState", nullptr,
		nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &gerr);
	g_object_unref(bus);
	if (!result) {
		g_clear_error(&gerr);
		return -1;
	}

	int angle = -1;
	GVariant *logicals = g_variant_get_child_value(result, 2);
	if (g_variant_n_children(logicals) > 0) {
		GVariant *mon = g_variant_get_child_value(logicals, 0);
		if (g_variant_n_children(mon) > 3) {
			GVariant *t = g_variant_get_child_value(mon, 3);
			if (g_variant_is_of_type(t, G_VARIANT_TYPE_UINT32))
				angle = int(g_variant_get_uint32(t));
			g_variant_unref(t);
		}
		g_variant_unref(mon);
	}
	g_variant_unref(logicals);
	g_variant_unref(result);
	return angle;
}

static void update_mutter_transform(App *app)
{
	const int t = mutter_transform_query();

	if (t < 0) {
		std::cerr << "erhai-cam: mutter DisplayConfig unreachable"
			  << "\n";
		return;
	}
	app->transform_ok = true;
	if (t == app->mutter_transform)
		return;
	app->mutter_transform = t;
	std::cerr << "erhai-cam: display transform " << t
		  << " -> preview rot "
		  << ((t >= 1 && t <= 3) ? (360 - 90 * t) % 360 : 0)
		  << " (deg cw applied by the widget)\n";
	if (app->preview)
		gtk_widget_queue_draw(app->preview);
}

/* mutter lags the sensor when auto-rotate kicks in; re-check shortly. */
static gboolean transform_requery_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app->transform_requery = 0;
	update_mutter_transform(app);
	return G_SOURCE_REMOVE;
}

static void schedule_transform_requery(App *app)
{
	if (app->transform_requery)
		return;
	app->transform_requery = g_timeout_add(400, transform_requery_cb, app);
}

static void monitors_changed(GDBusConnection *, const char *, const char *,
			     const char *, const char *, GVariant *,
			     gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	update_mutter_transform(app);
	schedule_transform_requery(app);
}

/* Track the display transform on the session bus (connection kept for the
 * process lifetime). */
static void setup_display_watch(App *app)
{
	GError *gerr = nullptr;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr,
					      &gerr);
	if (!bus) {
		g_clear_error(&gerr);
		return;
	}

	g_dbus_connection_signal_subscribe(
		bus, "org.gnome.Mutter.DisplayConfig",
		"org.gnome.Mutter.DisplayConfig", "MonitorsChanged",
		"/org/gnome/Mutter/DisplayConfig", nullptr,
		G_DBUS_SIGNAL_FLAGS_NONE, monitors_changed, app, nullptr);
	update_mutter_transform(app);
}

/*
 * ORIENTATION MODEL (2026-09-07/08, live-tested):
 *
 * Both cameras are fixed in the chassis and look at the WORLD.  With the
 * mutter transform at 0 the desktop buffer is panel-aligned: the raw
 * chassis frame drawn unrotated is world-upright on the display at any
 * physical pad rotation (owner-verified at 0/90/180/270) — rot=0, photos
 * raw.
 *
 * At a NON-ZERO mutter logical-monitor transform T (auto-rotate or manual
 * display rotation, desktop rotated T*90 deg cw) the buffer is composed
 * into a rotated LOGICAL frame, which re-introduces the camera's tilt: a
 * raw chassis-fixed frame drawn unrotated lands +T*90 deg cw in the user's
 * view (front cam owner report 2026-09-07: "head right / body left" at
 * T=1, "upside down" at T=2; rear cam owner report 2026-09-08: the same
 * error).  The LIVE PREVIEW is therefore counter-rotated T*90 deg ccw at
 * T=1..3 for BOTH cameras — gtk_snapshot_rotate is CLOCKWISE positive
 * (Gsk affine verified: rotate(+90) maps local up to screen right), so
 * rot = (360 - 90*T) % 360 applied by the widget; T=0 keeps rot=0.
 * Rotation only affects the preview drawing; photos/video stay raw world
 * records, and tap-to-focus inverts the cached g_rot (identity at T=0).
 *
 * The sensor (iio-sensor-proxy) and mutter DisplayConfig watches are kept
 * as JOURNAL DIAGNOSTICS only ("sensor angle N" / "display transform N").
 * The front module's single source horizontal-flip (handedness, OWNER
 * DECISION 2026-09-07) is independent of all of this.
 */
static void update_orientation(App *app)
{
	const int sensor = sensor_angle(app);

	if (sensor != app->sensor_angle)
		std::cerr << "erhai-cam: sensor angle " << sensor << "\n";
	app->sensor_angle = sensor;
	app->orient = sensor;
	schedule_transform_requery(app);
	if (app->preview)
		gtk_widget_queue_draw(app->preview);
}

static void orientation_changed(GDBusConnection *, const char *, const char *,
				const char *, const char *, GVariant *,
				gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	/* Contents ignored — just re-read the current property. */
	update_orientation(app);
}

/* --------------------------- rotating preview ---------------------------- */

/* gtk4paintablesink invalidates its paintable per frame from the streaming
 * thread; repaint our widget on each invalidation or the preview freezes. */
static gboolean preview_redraw_cb(gpointer ud)
{
	auto *widget = GTK_WIDGET(ud);
	auto *app = static_cast<App *>(g_object_get_data(G_OBJECT(widget),
							 "app"));
	if (app)
		app->draw_pending = false;
	gtk_widget_queue_draw(widget);
	return G_SOURCE_REMOVE;
}

static void paintable_invalidate_cb(GdkPaintable *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (!app->preview || app->draw_pending)
		return;
	app->draw_pending = true;
	g_main_context_invoke(nullptr, preview_redraw_cb, app->preview);
}

typedef struct {
	GtkWidget parent_instance;
	App *app;
} RotPreview;

typedef struct {
	GtkWidgetClass parent_class;
} RotPreviewClass;

G_DEFINE_TYPE(RotPreview, rot_preview, GTK_TYPE_WIDGET)

static void rot_preview_snapshot(GtkWidget *widget, GtkSnapshot *snap)
{
	auto *r = reinterpret_cast<RotPreview *>(widget);
	App *app = r->app;

	if (!app || !app->paintable)
		return;

	const int w = gtk_widget_get_width(widget);
	const int h = gtk_widget_get_height(widget);
	const float pw = gdk_paintable_get_intrinsic_width(app->paintable);
	const float ph = gdk_paintable_get_intrinsic_height(app->paintable);

	if (w < 2 || h < 2 || pw <= 0 || ph <= 0)
		return;

	/* Display transform compensation (see ORIENTATION MODEL): at mutter
	 * transform T (1..3 = desktop rotated T*90 cw) the raw chassis frame
	 * lands +T*90 cw in the rotated logical frame, so the preview is
	 * counter-rotated T*90 ccw:  rot = (360 - 90*T).
	 * The FRONT pipeline carries a horizontal-flip (mirrored preview,
	 * OWNER DECISION) which reverses handedness and flips the rotation
	 * sense, so the front needs the OPPOSITE sign:  rot = 90*T.
	 * Owner live test 2026-09-08: front OK at T=1..3 with 360-90*T; rear
	 * was upside down at T=1 and T=3 (rot away by 180) with the front
	 * formula, and 90*T fits (T=0/2 identical, T=1: 90, T=3: 270).
	 * gtk_snapshot_rotate is cw-positive, takes DEGREES (NOT radians —
	 * the old M_PI/180 conversion made all rotations invisible ~4.7 deg;
	 * fixed 2026-09-08 17:58).  T=0 keeps rot=0; photos/video/AF raw. */
	int rot = 0;
	if (app->mutter_transform >= 1 && app->mutter_transform <= 3)
		rot = (app->rear ? 90 * app->mutter_transform
				 : (360 - 90 * app->mutter_transform)) %
		      360;
	if (app->rot_override >= 0)
		rot = app->rot_override;
	const bool swap = rot == 90 || rot == 270;
	const double s = swap ? std::min(w / (double)ph, h / (double)pw)
			      : std::min(w / (double)pw, h / (double)ph);

	app->g_scale = s;
	app->g_swap = swap;
	app->g_rot = rot;
	app->g_w = w;
	app->g_h = h;

	gtk_snapshot_save(snap);
	graphene_point_t center = GRAPHENE_POINT_INIT(w / 2.0f, h / 2.0f);
	gtk_snapshot_translate(snap, &center);
	/* gtk_snapshot_rotate takes DEGREES (Gsk.Transform.rotate too — verified:
	 * rotate(270) -> affine (0,-1,1,0); rotate(270*pi/180) -> only ~4.7 deg).
	 * Pass the degree value directly; the old radians conversion made the
	 * rotation essentially invisible (~4.7 deg instead of 90/180/270). */
	if (rot)
		gtk_snapshot_rotate(snap, float(rot));
	gtk_snapshot_scale(snap, float(s), float(s));
	graphene_point_t off = GRAPHENE_POINT_INIT(-pw / 2.0f, -ph / 2.0f);
	gtk_snapshot_translate(snap, &off);
	gdk_paintable_snapshot(app->paintable, snap, pw, ph);
	gtk_snapshot_restore(snap);
}

static void rot_preview_measure(GtkWidget *, GtkOrientation, int, int *minimum,
				int *natural, int *min_baseline, int *nat_baseline)
{
	*minimum = 0;
	*natural = 0;
	*min_baseline = -1;
	*nat_baseline = -1;
}

static void rot_preview_dispose(GObject *object)
{
	G_OBJECT_CLASS(rot_preview_parent_class)->dispose(object);
}

static void rot_preview_class_init(RotPreviewClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	gobject_class->dispose = rot_preview_dispose;
	widget_class->snapshot = rot_preview_snapshot;
	widget_class->measure = rot_preview_measure;
}

static void rot_preview_init(RotPreview *) {}

/* Map a click on the rotated preview back into image coordinates. */
static void preview_click(App *app, double cx, double cy)
{
	Af &af = app->af;

	if (!app->lens_ok || !app->rear)
		return;
	if (af.mode == AfMode::Sweep)
		return;

	const float pw = app->paintable ?
		gdk_paintable_get_intrinsic_width(app->paintable) : 0.f;
	const float ph = app->paintable ?
		gdk_paintable_get_intrinsic_height(app->paintable) : 0.f;
	if (pw <= 0 || ph <= 0)
		return;

	double dx = (cx - app->g_w / 2.0) / app->g_scale;
	double dy = (cy - app->g_h / 2.0) / app->g_scale;
	/* Undo the preview rotation applied in the snapshot (if any). */
	double rx = dx, ry = dy;
	switch (app->g_rot) {
	case 90:
		rx = dy;
		ry = -dx;
		break;
	case 180:
		rx = -dx;
		ry = -dy;
		break;
	case 270:
		rx = -dy;
		ry = dx;
		break;
	default:
		break;
	}

	af.want_point = true;
	af.point_x = pw / 2.0 + rx;
	af.point_y = ph / 2.0 + ry;
	/* With continuous AF off, a tap runs exactly one sweep. */
	af.one_shot = !af.enabled;
	autofocus_start(app);
}

/* ---------------------------------- UI ----------------------------------- */

static void on_shot_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->camera_ok)
		app_take_photo(app);
}

static void on_rec_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app_record_toggle(app);
}

static void on_switch_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app_switch_camera(app);
}

static void app_af_toggle(App *app)
{
	Af &af = app->af;

	af.enabled = !af.enabled;
	if (af.enabled) {
		af.one_shot = false;
		if (app->rear && app->lens_ok)
			autofocus_start(app);
		set_status(app, "Autofocus on");
	} else {
		af.mode = AfMode::Idle;
		af.one_shot = false;
		set_status(app, "Autofocus off \u2014 slide to focus manually");
	}
	app_sync_controls(app);
}

static void on_af_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app_af_toggle(app);
}

/* Auto exposure toggle: on -> the loop owns the exposure register and the
 * slider disappears; off -> the slider takes over, seeded with the exposure
 * the loop last applied (or the sensor's real value, read back). */
static void app_ae_toggle(App *app)
{
	Ae &ae = app->ae;

	ae.enabled = !ae.enabled;
	ae.stride = 0;
	ae.settle = 0;
	if (ae.enabled) {
		if (app->rear && app->sensor_ok) {
			ae.exposure = app->sensor->exposure_lines();
			ae.measured = false;
			set_status(app, "Auto exposure on");
		} else {
			set_status(app, "Auto exposure needs the rear camera");
		}
	} else {
		if (app->sensor_ok && app->camera_ok) {
			/* Show the sensor's real value when it answers (it often
			 * returns zeros for 0x3e00 — the register is largely
			 * write-only — in which case keep the app's tracked
			 * value and let the first drag set what the user
			 * wants).  Do NOT write: with AE off the sensor keeps
			 * whatever it had, which is the visually correct
			 * current state. */
			app->sensor->read_exposure_lines(&ae.exposure);
		}
		set_status(app, "Auto exposure off \u2014 slide to set exposure");
	}
	app_sync_controls(app);
}

static void on_ae_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app_ae_toggle(app);
}

/* Both manual sliders are debounced onto the hardware: a drag produces ~16
 * writes/s instead of one per pixel of travel (the VCM and the CCI bus both
 * dislike bursts), and the write happens on the main loop. */
static gboolean manual_apply_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app->slider_timer = 0;
	if (app->pending_exposure >= 0) {
		const int lines = app->pending_exposure;

		app->pending_exposure = -1;
		app->ae.exposure = lines;
		if (!apply_exposure(app, lines) && !app->headless)
			set_status(app, "Exposure write failed \u2014 is the "
					"preview streaming?");
	}
	if (app->pending_focus >= 0) {
		const int pos = app->pending_focus;

		app->pending_focus = -1;
		lens_set(app, pos);
	}
	return G_SOURCE_REMOVE;
}

static void queue_manual_write(App *app)
{
	if (!app->slider_timer)
		app->slider_timer = g_timeout_add(kSliderDebounceMs,
						  manual_apply_cb, app);
}

static void on_exposure_changed(GtkRange *range, gpointer ud)
{
	auto *app = static_cast<App *>(ud);
	const int lines = int(std::lround(gtk_range_get_value(range)));

	if (app->exp_value)
		gtk_label_set_text(GTK_LABEL(app->exp_value),
				   std::to_string(lines).c_str());
	if (app->slider_sync || app->ae.enabled)
		return;   /* programmatic update, or AE is driving */
	app->pending_exposure = lines;
	queue_manual_write(app);
}

static void on_focus_changed(GtkRange *range, gpointer ud)
{
	auto *app = static_cast<App *>(ud);
	const int pos = int(std::lround(gtk_range_get_value(range)));

	if (app->focus_value)
		gtk_label_set_text(GTK_LABEL(app->focus_value),
				   std::to_string(pos).c_str());
	if (app->slider_sync || app->af.enabled)
		return;
	app->pending_focus = pos;
	queue_manual_write(app);
}

/* Cycle the manual preview rotation: AUTO (-1) -> 0 -> 90 -> 180 -> 270 -> AUTO.
 * Used by the on-screen rotate button and the 'o' debug key.  A manual
 * override is useful in laptop mode (pogo keyboard attached), where mutter
 * locks auto-rotate off and the user still wants to view the preview
 * upright while holding the pad rotated.  rot is applied CW-positive by
 * gtk_snapshot_rotate (see ORIENTATION MODEL). */
static void cycle_rotation(App *app)
{
	static const int kOverrides[] = { -1, 0, 90, 180, 270 };

	app->rot_override_idx = (app->rot_override_idx + 1) % 5;
	app->rot_override = kOverrides[app->rot_override_idx];
	std::string msg;
	if (app->rot_override < 0)
		msg = "rotate: auto";
	else
		msg = "rotate: " + std::to_string(app->rot_override) + "\u00b0 cw";
	set_status(app, msg);
	gtk_widget_queue_draw(app->preview);
}

static gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint,
			       GdkModifierType, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	switch (keyval) {
	case GDK_KEY_space:
		on_shot_clicked(nullptr, app);
		return true;
	case GDK_KEY_r:
		on_rec_clicked(nullptr, app);
		return true;
	case GDK_KEY_c:
		on_switch_clicked(nullptr, app);
		return true;
	case GDK_KEY_a:
		app_af_toggle(app);
		return true;
	case GDK_KEY_e:
		app_ae_toggle(app);
		return true;
	case GDK_KEY_o:
		cycle_rotation(app);
		return true;
	default:
		return false;
	}
}

static void on_gesture_pressed(GtkGestureClick *, int, double x, double y,
			       gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	preview_click(app, x, y);
}

static void setup_orientation_watch(App *app)
{
	GError *gerr = nullptr;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr,
					      &gerr);
	if (!bus) {
		g_clear_error(&gerr);
		return;
	}

	g_dbus_connection_call_sync(
		bus, "net.hadess.SensorProxy", "/net/hadess/SensorProxy",
		"net.hadess.SensorProxy", "ClaimAccelerometer", nullptr, nullptr,
		G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &gerr);
	g_clear_error(&gerr);

	g_dbus_connection_signal_subscribe(
		bus, "net.hadess.SensorProxy", "org.freedesktop.DBus.Properties",
		"PropertiesChanged", "/net/hadess/SensorProxy",
		"net.hadess.SensorProxy", G_DBUS_SIGNAL_FLAGS_NONE,
		orientation_changed, app, nullptr);
	/* bus ref intentionally kept for the process lifetime */
	update_orientation(app);
}

static void activate(GtkApplication *gapp, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app->win = gtk_application_window_new(GTK_APPLICATION(gapp));
	gtk_window_set_title(GTK_WINDOW(app->win), "ErhaiCam");
	gtk_window_set_default_size(GTK_WINDOW(app->win), 900, 700);

	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_string(
		css, "window { background: black; } "
		     "#status { background: rgba(0,0,0,0.55); "
		     "border-radius: 8px; padding: 4px 12px; color: white; } "
		     "#shutter { min-width: 64px; min-height: 64px; "
		     "padding: 0; border-radius: 9999px; background: white; "
		     "border: 4px solid #444; } "
		     "#shutter.flash { background: #ff3333; "
		     "border-color: #ff3333; } "
		     "#bar { margin-bottom: 12px; } "
		     "#bar button { min-width: 48px; min-height: 48px; } "
		     "#bar button.recording { background: #cc2222; "
		     "color: white; } "
		     "button.togbtn { border-radius: 9999px; min-width: 52px; "
		     "min-height: 52px; padding: 0; background: "
		     "rgba(0,0,0,0.6); color: white; font-weight: bold; } "
		     "button.togbtn.on { background: white; color: black; } "
		     "button.togbtn.off { background: rgba(0,0,0,0.6); "
		     "color: #bbbbbb; } "
		     "button.togbtn:disabled { color: #666666; } "
		     "scale.manual { min-height: 220px; min-width: 40px; } "
		     "label.manual { color: white; font-size: 0.85em; "
		     "background: rgba(0,0,0,0.55); border-radius: 6px; "
		     "padding: 1px 6px; }");
	gtk_style_context_add_provider_for_display(
		gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	GtkWidget *overlay = gtk_overlay_new();
	gtk_window_set_child(GTK_WINDOW(app->win), overlay);

	GtkWidget *preview = GTK_WIDGET(g_object_new(rot_preview_get_type(),
						     nullptr));
	reinterpret_cast<RotPreview *>(preview)->app = app;
	g_object_set_data(G_OBJECT(preview), "app", app);
	app->preview = preview;
	gtk_overlay_set_child(GTK_OVERLAY(overlay), preview);

	/* ---- top-left: AE (auto exposure) + its manual slider below it ----- */
	GtkWidget *left_dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_halign(left_dock, GTK_ALIGN_START);
	gtk_widget_set_valign(left_dock, GTK_ALIGN_START);
	gtk_widget_set_margin_start(left_dock, 16);
	gtk_widget_set_margin_top(left_dock, 16);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), left_dock);

	app->btn_ae = gtk_button_new_with_label("AE");
	gtk_widget_add_css_class(app->btn_ae, "togbtn");
	gtk_widget_add_css_class(app->btn_ae, "on");
	gtk_widget_set_tooltip_text(app->btn_ae,
				    "Automatic exposure (NPU). Turn it off "
				    "for the manual exposure slider.");
	g_signal_connect(app->btn_ae, "clicked", G_CALLBACK(on_ae_clicked), app);
	gtk_box_append(GTK_BOX(left_dock), app->btn_ae);

	app->exp_scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
						  kExposureMin, kExposureMax, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->exp_scale), FALSE);
	gtk_range_set_inverted(GTK_RANGE(app->exp_scale), TRUE);
	gtk_widget_add_css_class(app->exp_scale, "manual");
	gtk_widget_set_tooltip_text(app->exp_scale,
				    "Exposure (sensor lines): down = darker");
	g_signal_connect(app->exp_scale, "value-changed",
			 G_CALLBACK(on_exposure_changed), app);
	gtk_box_append(GTK_BOX(left_dock), app->exp_scale);

	app->exp_value = gtk_label_new(std::to_string(kExposureDefault).c_str());
	gtk_widget_add_css_class(app->exp_value, "manual");
	gtk_widget_set_halign(app->exp_value, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(left_dock), app->exp_value);

	/* ---- top-right: AF + its manual focus slider below it --------------- */
	GtkWidget *right_dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_halign(right_dock, GTK_ALIGN_END);
	gtk_widget_set_valign(right_dock, GTK_ALIGN_START);
	gtk_widget_set_margin_end(right_dock, 16);
	gtk_widget_set_margin_top(right_dock, 16);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), right_dock);

	app->btn_af = gtk_button_new_with_label("AF");
	gtk_widget_add_css_class(app->btn_af, "togbtn");
	gtk_widget_add_css_class(app->btn_af, "on");
	gtk_widget_set_tooltip_text(app->btn_af,
				    "Continuous autofocus (NPU). Turn it off "
				    "for the manual focus slider.");
	g_signal_connect(app->btn_af, "clicked", G_CALLBACK(on_af_clicked),
			 app);
	gtk_box_append(GTK_BOX(right_dock), app->btn_af);

	app->focus_scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
						   kFocusMin, kFocusMax, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->focus_scale), FALSE);
	gtk_range_set_inverted(GTK_RANGE(app->focus_scale), TRUE);
	gtk_widget_add_css_class(app->focus_scale, "manual");
	gtk_widget_set_tooltip_text(app->focus_scale,
				    "Focus (VCM): up = far, down = near");
	g_signal_connect(app->focus_scale, "value-changed",
			 G_CALLBACK(on_focus_changed), app);
	gtk_box_append(GTK_BOX(right_dock), app->focus_scale);

	app->focus_value = gtk_label_new("0");
	gtk_widget_add_css_class(app->focus_value, "manual");
	gtk_widget_set_halign(app->focus_value, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(right_dock), app->focus_value);

	app_sync_controls(app);

	GtkGesture *gesture = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
	g_signal_connect(gesture, "pressed", G_CALLBACK(on_gesture_pressed), app);
	gtk_widget_add_controller(preview, GTK_EVENT_CONTROLLER(gesture));

	app->status = gtk_label_new("");
	gtk_widget_set_name(app->status, "status");
	/* An empty label still draws its CSS padding/background — the black
	 * spot at the top of the preview.  Keep it hidden until used. */
	gtk_widget_set_visible(app->status, FALSE);
	gtk_widget_set_halign(app->status, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(app->status, GTK_ALIGN_START);
	gtk_widget_set_margin_top(app->status, 16);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->status);

	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_widget_set_name(bar, "bar");
	gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(bar, GTK_ALIGN_END);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar);

	app->btn_switch = gtk_button_new_from_icon_name("camera-switch-symbolic");
	g_signal_connect(app->btn_switch, "clicked",
			 G_CALLBACK(on_switch_clicked), app);
	gtk_box_append(GTK_BOX(bar), app->btn_switch);

	app->btn_shot = gtk_button_new();
	gtk_widget_set_name(app->btn_shot, "shutter");
	g_signal_connect(app->btn_shot, "clicked", G_CALLBACK(on_shot_clicked), app);
	gtk_box_append(GTK_BOX(bar), app->btn_shot);

	app->btn_rec = gtk_button_new_from_icon_name("media-record-symbolic");
	gtk_button_set_label(GTK_BUTTON(app->btn_rec), "Record");
	g_signal_connect(app->btn_rec, "clicked", G_CALLBACK(on_rec_clicked), app);
	gtk_box_append(GTK_BOX(bar), app->btn_rec);

	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), app);
	gtk_widget_add_controller(app->win, keys);

	setup_orientation_watch(app);
	setup_display_watch(app);
	preview_pipeline_start(app);
	gtk_window_present(GTK_WINDOW(app->win));
}

/* --------------------------------- main ---------------------------------- */

static gboolean headless_timeout_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	std::cerr << "erhai-cam: headless run timed out\n";
	g_main_loop_quit(app->loop);
	return G_SOURCE_REMOVE;
}

static gboolean rec_stop_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	app_record_stop(app);
	return G_SOURCE_REMOVE;
}

static gboolean headless_tick_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (++app->headless_ticks > 300) {   /* 30 s hard cap */
		headless_timeout_cb(app);
		return G_SOURCE_REMOVE;
	}

	if (app->shot) {
		if (!app->last_sample)
			return G_SOURCE_CONTINUE;
		std::string path, err;
		if (!app->out_path.empty()) {
			/* honour explicit path: save_photo writes default name,
			 * so capture, then move the file. */
			std::string def;
			if (!save_photo(app, &def, &err)) {
				std::cerr << "erhai-cam: " << err << "\n";
				g_main_loop_quit(app->loop);
				return G_SOURCE_REMOVE;
			}
			std::error_code ec;
			std::filesystem::rename(def, app->out_path, ec);
			path = ec ? def : app->out_path;
		} else {
			if (!save_photo(app, &path, &err)) {
				std::cerr << "erhai-cam: " << err << "\n";
				g_main_loop_quit(app->loop);
				return G_SOURCE_REMOVE;
			}
		}
		std::cout << "saved: " << path << "\n";
		g_main_loop_quit(app->loop);
		return G_SOURCE_REMOVE;
	}

	if (app->record_seconds > 0) {
		if (!app->rec_started) {
			if (!app->last_sample)
				return G_SOURCE_CONTINUE;
			app->rec_started = true;
			app_record_start(app);
			g_timeout_add_seconds(app->record_seconds,
					      rec_stop_cb, app);
		}
		return G_SOURCE_CONTINUE;
	}

	/* --once */
	if (app->quit_when_af_done) {
		std::cout << "focus=" << af_position(app) << "\n";
		g_main_loop_quit(app->loop);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

} // namespace

int main(int argc, char **argv)
{
	/* BUILD STAMP: every install gets a unique tag logged at startup so
	 * the journal always proves WHICH build a session is actually
	 * running (2026-09-07: repeated "no change" confusion). */
	std::cerr << "erhai-cam: BUILD " << __DATE__ " " << __TIME__
		  << " tag=shutter-ui-10-ae-npu\n";
	/* A dead analyzer helper must not kill the app with SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);
	std::string camera_override;
	bool use_gui = true;
	/* argv pass-through for g_application_run(): GApplication rejects
	 * unknown LOCAL options ("Unknown option --camera" -> silent exit
	 * before activate), so only forward argv[0]. */
	std::vector<char *> pass_argv{ argv[0] };

	for (int i = 1; i < argc; i++) {
		const std::string arg(argv[i]);
		if (arg == "--shot") {
			g_app = new App();
			g_app->shot = true;
			g_app->headless = true;
			use_gui = false;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				g_app->out_path = argv[++i];
		} else if (arg == "--record") {
			g_app = new App();
			g_app->record_seconds = (i + 1 < argc) ? std::atoi(argv[++i]) : 5;
			g_app->headless = true;
			use_gui = false;
		} else if (arg == "--once") {
			g_app = new App();
			g_app->once = true;
			g_app->headless = true;
			use_gui = false;
		} else if (arg == "--ae-probe") {
			/* headless AE soak: log every controller update */
			g_app = new App();
			g_app->headless = true;
			g_app->ae_probe = true;
			g_app->ae_probe_seconds =
				(i + 1 < argc) ? std::atoi(argv[++i]) : 30;
			use_gui = false;
		} else if (arg == "--no-ae") {
			if (!g_app)
				g_app = new App();
			g_app->ae.enabled = false;
		} else if (arg == "--no-af") {
			if (!g_app)
				g_app = new App();
			g_app->no_af = true;
			g_app->af.enabled = false;
		} else if (arg == "--camera" && i + 1 < argc) {
			camera_override = argv[++i];
		} else if (arg == "-h" || arg == "--help") {
			std::cout << "Usage: erhai-cam [--shot [PATH]] [--record SECONDS] "
				     "[--once] [--ae-probe SECONDS] [--no-af] "
				     "[--no-ae] [--camera NAME]\n";
			return 0;
		}
	}
	if (!g_app)
		g_app = new App();
	App *app = g_app;

	if (camera_override.empty()) {
		/* CLI/GApplication option parsing can eat --camera; an env
		 * override always works (used for headless orientation
		 * measurements). */
		if (const char *env = std::getenv("ERHAI_CAMERA"))
			camera_override = env;
	}

	if (!camera_override.empty()) {
		app->camera = camera_override;
		app->rear = (camera_override == kRearCamera);
	}

	gst_init(&argc, &argv);

	/* Lens: rear camera only. */
	if (auto path = Lens::find()) {
		try {
			app->lens = new Lens(*path);
			app->lens_ok = true;
			std::cout << "Lens device: " << *path << "\n";
		} catch (const std::exception &e) {
			std::cerr << "lens: " << e.what() << "\n";
		}
	} else {
		std::cerr << "no " << kLensName << " subdev; autofocus disabled\n";
	}
	if (!app->rear)
		app->lens_ok = false;

	/* Sensor: rear camera only (the exposure register lives on sc1320cs). */
	if (app->rear) {
		if (auto path = Sensor::find()) {
			try {
				app->sensor = new Sensor(*path);
				app->sensor_ok = true;
				int lines = 0;

				if (app->sensor->read_exposure_lines(&lines)) {
					app->ae.exposure = lines;
					std::cout << "Sensor exposure: " << lines
						  << " lines\n";
				}
				std::cout << "Sensor device: " << *path << "\n";
			} catch (const std::exception &e) {
				std::cerr << "sensor: " << e.what() << "\n";
			}
		}
		if (!app->sensor_ok)
			std::cerr << "no sc1320cs i2c client; exposure "
				     "control disabled\n";
	}

	/* The NPU analyzer (Hexagon HTP) for autofocus + auto exposure. */
	app->analyzer.start();
	app->analyzer_poll = g_timeout_add(100, [](gpointer d) -> gboolean {
		auto *a = static_cast<App *>(d);

		if (a->analyzer.poll_ready()) {
			std::cout << "analyzer: " << a->analyzer.backend() << " ("
				  << a->analyzer.detail() << ")\n";
			if (a->analyzer.state() == Analyzer::Dead) {
				std::cerr << "erhai-cam: " << a->analyzer.detail()
					  << "; focus/exposure run on the CPU\n";
				a->analyzer_poll = 0;
				return G_SOURCE_REMOVE;
			}
		}
		return G_SOURCE_CONTINUE;
	}, app);

	app->loop = g_main_loop_new(nullptr, false);

	if (use_gui) {
		GtkApplication *gtkapp = gtk_application_new("org.erhai.ErhaiCam",
							     G_APPLICATION_NON_UNIQUE);
		g_signal_connect(gtkapp, "activate", G_CALLBACK(activate), app);
		g_application_run(G_APPLICATION(gtkapp),
				  (int)pass_argv.size(), pass_argv.data());
		g_object_unref(gtkapp);
	} else {
		preview_pipeline_start(app);
		g_timeout_add(100, headless_tick_cb, app);
		g_timeout_add_seconds(30, headless_timeout_cb, app);
		if (app->ae_probe_seconds > 0)
			g_timeout_add_seconds(app->ae_probe_seconds,
					      headless_timeout_cb, app);
		g_main_loop_run(app->loop);
	}

	app->analyzer.stop();
	if (app->recording)
		app_record_stop(app);
	preview_pipeline_stop(app);
	return 0;
}

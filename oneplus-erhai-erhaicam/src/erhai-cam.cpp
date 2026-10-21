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
 * integration time, written straight to the sensor register 0x3e00 (the
 * kernel's V4L2_CID_EXPOSURE is a no-op) — since 2.8 on BOTH cameras, each
 * with its own register and limits (rear sc1320cs 6396 lines, front sc820cs
 * 3120); toggled off, a vertical slider takes over there too.  That ownership
 * is what keeps the software ISP out of the picture: with no exposure model
 * for either sensor it could only brighten in SOFTWARE, which lifts the black
 * level and yellows a dark scene ("the yellowish taint", 2026-10-21).  The AF
 * loop stays rear-only (the lens is).  The live preview is
 * counter-rotated T*90 deg ccw at mutter transform T (1..3, both cameras —
 * see the ORIENTATION MODEL comment) while photo capture saves the frame
 * UNROTATED (the raw chassis-fixed frame is the world record and stays
 * upright as a photo at every orientation); video is recorded as streamed.
 *
 * Video recording (2026-10-20): the hardware encoder (v4l2h264enc / Iris, whose
 * only raw format is NV12 — asking it for I420 silently fails the link and the
 * old pipeline fell back to software VP8) muxes H.264 + AAC into MP4, with an
 * automatic VP8/Opus WebM fallback.  The bottom-bar note button picks the
 * soundtrack: "No audio" (silent video, the default) or "Microphone" — the
 * PipeWire default source (the pad's Internal Mics), captured as a second
 * branch of the SAME pipeline so the muxer keeps the two streams in sync.
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

/* ---- exposure / the sensor register interface --------------------------
 * Integration time is 0x3e00..0x3e02 as a 3-byte big-endian burst (the
 * sensor auto-increments), holding lines << 5.  BOTH cameras use the same
 * register layout and the same encoding — only the ranges differ, because VTS
 * is fixed per sensor and the integration time must stay below the frame
 * length:
 *
 *   rear  sc1320cs   VTS 6404  ->  usable 0..6396, mode-table default 3196
 *   front sc820cs    VTS 3128  ->  usable 0..3120, mode-table default 3124
 *
 * The front therefore sits AT its ceiling in any normal room, which is the
 * whole reason the software ISP used to brighten it in SOFTWARE (and yellow
 * the picture doing it): libcamera has no exposure/gain model for either
 * sensor ("Failed to create camera sensor helper"), the kernel's
 * V4L2_CID_EXPOSURE / ANALOGUE_GAIN are no-ops on both, and the ISP's AGC can
 * then only express its target as DIGITAL gain.  The app owns BOTH registers
 * (2026-10-21, ErhaiCam 2.8) and the ISP's AGC is pinned off by default, so
 * brightness comes from real integration time instead.
 *
 * The soft ISP can invert ("negative") frames if the raw saturates near the
 * top, so AUTO exposure keeps itself below the sensor's AE ceiling and leaves
 * the top of the range to the manual slider. */
struct SensorSpec {
	const char *name;      /* i2c client name — the bus is never hardcoded */
	int addr;              /* its i2c address */
	int vts_default;       /* mode-table frame length (VTS, 0x320e/0f) */
	int vts_max;           /* how far the low-light ladder may lengthen it */
	int ae_reserve;        /* lines kept free below the frame's end */
	int default_lines;     /* the mode table's default exposure */
};
/* rear  sc1320cs: VTS 0x1904 = 6404 (driver SC1320CS_VTS_DEF) */
constexpr SensorSpec kRearSensor = { "sc1320cs", 0x36, 6404, 25616, 1596, 3196 };
/* front sc820cs: VTS 0x0c38 = 3128 (driver SC820CS_VTS_DEF) — and this sensor
 * is the reason the ladder exists: at 3128 lines its exposure ceiling is 3120,
 * so in a dim room it was simply out of light (measured luma 43, signal band 18
 * of 255) with NO ISP option able to help. */
constexpr SensorSpec kFrontSensor = { "sc820cs", 0x10, 3128, 12512, 0, 3124 };
/* VTS registers (both sensors, 16-bit big-endian pair) */
constexpr int kVtsRegHi = 0x320e;
constexpr int kVtsRegLo = 0x320f;
constexpr int kExposureShift = 5;
constexpr int kExposureMin = 1;
constexpr int kExposureMax = 6396;       /* rear VTS(6404) - 8: fader fallback */
constexpr int kExposureAeMax = 4800;     /* rear AE ceiling: AE fallback */
constexpr int kExposureDefault = kRearSensor.default_lines;
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
/* The manual AE/AF faders are stretched to the window and their tracks stop
 * this many pixels above the window's bottom edge: the note button sits in the
 * bottom-RIGHT corner (kNoteButtonBottomGap, the same 16 px the AF button uses
 * on the right edge) and is 52 px tall, so the fader must clear it — the gap is
 * 16 + 52 + 8 (box spacing) so BOTH faders stay the same length and mirror each
 * other (owner request 2026-10-20: "make the AF and AE fader longer, stop 50px
 * over bottom window edge", then "put the note button at the very right (same
 * margin as AF button) and make it round" — the button wins, the faders stay
 * symmetric). */
constexpr int kFaderBottomGap = 76;
constexpr int kNoteButtonBottomGap = 16;
constexpr int kNoteButtonSize = 52;   /* .togbtn min-width/height */

/* How long the picture waits for the microphone's first real audio before the
 * recording starts anyway (the pad's capture path needs ~0.15-0.25 s to wake;
 * see rec_audio_probe_cb / on_new_sample). */
constexpr auto kRecAudioWait = std::chrono::milliseconds(1500);

/* ---- the resolution menu -------------------------------------------------
 * ONE stream feeds everything (preview, video recording, photos, AF/AE), so
 * the size the user picks here is the size of the preview AND of the recorded
 * video (and of a snapshot).  The choice is stored PER CAMERA and the two
 * cameras do not offer the same list, because their sensors differ:
 *
 *   rear  = sc1320cs, 4160x3120 ; the software ISP scales to anything up to
 *           the sensor's own size, so 4152x3120 (its cropped native) is the
 *           largest entry — measured on the pad, every size below negotiates.
 *   front = sc820cs, 3264x2448 ; its BARE native width does NOT negotiate
 *           (measured: 3264x2448 fails caps negotiation), 3200x2400 is the
 *           largest usable size.
 *
 * Every offered size is verified to negotiate at 30 fps on this hardware; a
 * stored/forced size that does not work falls back to the other entries (see
 * preview_pipeline_start).  The DEFAULT is 1920x1080 (Full HD) for both
 * cameras (kDefaultW/kDefaultH below). */
struct ResOption {
	int w;
	int h;
	const char *label;   /* menu text, after the "WxH" pair */
};

const std::vector<ResOption> kRearResolutions = {
	{ 4152, 3120, "Max (4:3, sensor native)" },
	{ 3840, 2160, "4K UHD (16:9)" },
	{ 3200, 2400, "8 MP (4:3)" },
	{ 2560, 1920, "5 MP (4:3)" },
	{ 1920, 1080, "Full HD (16:9)" },
	{ 1280, 720, "HD (16:9)" },
	{ 640, 480, "SD (4:3)" },
};
const std::vector<ResOption> kFrontResolutions = {
	{ 3200, 2400, "8 MP (4:3)" },
	{ 2560, 1920, "5 MP (4:3)" },
	{ 1920, 1080, "Full HD (16:9)" },
	{ 1600, 1200, "2 MP (4:3)" },
	{ 1280, 960, "1.2 MP (4:3)" },
	{ 640, 480, "SD (4:3)" },
};
/* 1920x1080 ("Full HD") is the entry both lists share, i.e. the DEFAULT of the
 * app (used when nothing is stored yet) — owner decision 2026-10-20: Full HD is
 * the sane default for video, and the picker's own button always shows which
 * size is in use. */
constexpr int kDefaultW = 1920;
constexpr int kDefaultH = 1080;

/* ---- persisted settings --------------------------------------------------
 * The owner asked for BOTH cameras' chosen resolutions to survive a restart,
 * so they live in a small GKeyFile under the XDG config dir:
 *
 *     ~/.config/erhai/erhaicam.conf
 *     [resolution]
 *     rear=1920x1080
 *     front=1920x1080
 *
 * GKeyFile (not GSettings): no schema package/glib-compile-schemas step in the
 * deb, no D-Bus session needed, and the file is trivially inspectable — the app
 * has no other persisted state.  Every failure mode (missing file, bad value,
 * read-only HOME) falls back to the default and never blocks the UI. */
constexpr char kConfigSubdir[] = "erhai";
constexpr char kConfigName[] = "erhaicam.conf";
constexpr char kConfigGroup[] = "resolution";

std::string config_path()
{
	const char *xdg = std::getenv("XDG_CONFIG_HOME");
	const char *home = std::getenv("HOME");
	std::string base;

	if (xdg && *xdg)
		base = xdg;
	else if (home && *home)
		base = std::string(home) + "/.config";
	else
		return {};

	return base + "/" + kConfigSubdir + "/" + kConfigName;
}

/* "3200x2400" -> {3200, 2400}; anything else -> nullopt. */
std::optional<std::pair<int, int>> parse_res(const std::string &text)
{
	const auto x = text.find('x');

	if (x == std::string::npos || x == 0 || x + 1 >= text.size())
		return std::nullopt;
	try {
		const int w = std::stoi(text.substr(0, x));
		const int h = std::stoi(text.substr(x + 1));
		if (w <= 0 || h <= 0)
			return std::nullopt;
		return std::make_pair(w, h);
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

/* Index of the entry that EXACTLY matches w x h, else -1. */
int res_index_of(const std::vector<ResOption> &opts, int w, int h)
{
	for (size_t i = 0; i < opts.size(); i++)
		if (opts[i].w == w && opts[i].h == h)
			return int(i);
	return -1;
}

int default_res_index(const std::vector<ResOption> &opts)
{
	const int i = res_index_of(opts, kDefaultW, kDefaultH);

	return i >= 0 ? i : 0;
}

/* Read one camera's stored choice ("" / unknown values -> the default). */
int load_res_choice(const std::vector<ResOption> &opts, const char *key)
{
	const std::string path = config_path();

	if (path.empty())
		return default_res_index(opts);
	GKeyFile *kf = g_key_file_new();
	int idx = default_res_index(opts);

	if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE,
				      nullptr)) {
		gchar *value = g_key_file_get_string(kf, kConfigGroup, key,
						    nullptr);

		if (value) {
			if (const auto parsed = parse_res(value)) {
				if (const int i = res_index_of(opts, parsed->first,
							      parsed->second);
				    i >= 0)
					idx = i;
			}
			g_free(value);
		}
	}
	g_key_file_unref(kf);
	return idx;
}

/* Store ONE camera's choice, leaving the other camera's entry alone (best
 * effort; a read-only HOME is not worth interrupting the camera with). */
void save_res_choice(bool rear, const ResOption &opt)
{
	const std::string path = config_path();

	if (path.empty())
		return;
	GKeyFile *kf = g_key_file_new();
	std::error_code ec;
	const std::string value =
		std::to_string(opt.w) + "x" + std::to_string(opt.h);

	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path(), ec);
	g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr);
	g_key_file_set_string(kf, kConfigGroup, rear ? "rear" : "front",
			      value.c_str());
	g_key_file_set_comment(kf, nullptr, nullptr,
			       "ErhaiCam settings (video/preview/photo resolution "
			       "per camera)", nullptr);
	gchar *text = g_key_file_to_data(kf, nullptr, nullptr);

	if (text)
		g_file_set_contents(path.c_str(), text, -1, nullptr);
	g_free(text);
	g_key_file_unref(kf);
}

/* ---- the ISP controls (libcamera camera controls) -----------------------
 * libcamerasrc exposes EVERY libcamera control as a GObject property
 * (`gst-inspect-1.0 libcamerasrc`): ae-enable, ae-exposure-mode,
 * ae-metering-mode, ae-flicker-period, analogue-gain, exposure-time, gamma,
 * saturation, sharpness, awb-enable, awb-mode, colour-gains, brightness,
 * contrast …  They are the software ISP's levers, and the owner asked for them
 * MANUALLY (2026-10-20): in LOW light the ISP's `Adjust` algorithm compensates
 * IN SOFTWARE for the analogue gain it cannot give the sensor (the sc1320cs's
 * s_ctrl is a no-op), and that compensation (gamma + saturation boost) yellows
 * the picture; in a bright room the compensation is 1.0 and the image is fine.
 *
 * 2.8 — the ISP's luminance-dependent compensation is OFF BY DEFAULT, because
 * it IS the owner's "yellowish taint" (measured 2026-10-21): in a dark scene
 * the frame's black floor rose 4x (luma p1 10.5 -> 42.2 on his own two photos)
 * while the highlights barely moved — a fog, not a colour cast (the colour
 * temperature was unchanged, 4657 K vs 4686 K).  Pinning libcamera's `gamma`
 * and `saturation` stops the `Adjust` algorithm from re-curving the picture;
 * the app's own AE (which now owns BOTH cameras' exposure registers) provides
 * the brightness instead.  No `Ccm` exists for these sensors (uncalibrated
 * tuning), so nothing else in the pipeline can be pinned.
 *
 * So they can be forced from
 * The default is therefore the two PINS (kIspDefault) — they only take the
 * freedom away from `Adjust`, they cannot make a dark room darker.  What 2.8
 * deliberately does NOT do by default is switch the ISP's own AE off
 * (`lowlight`): with the front sensor's ceiling already at its maximum, that
 * would only darken a dark scene further while the fix for the tone is the
 * curve, not a second gain stage.  Every knob stays reachable.
 *
 *   ERHAI_CAM_ISP="key=value,key=value,…"      (a complete override)
 *   ERHAI_CAM_ISP=default|on|pins|gain         (the 2.8 default, kIspDefault)
 *   ERHAI_CAM_ISP=lowlight                     (the pins PLUS
 *                                               ae-enable=false: no second
 *                                               gain stage at all)
 *   ERHAI_CAM_ISP=none|off|raw|isp             (2.7 behaviour: touch nothing)
 * and from ~/.config/erhai/erhaicam.conf `[isp] key=value` lines, which are
 * APPENDED to the default (a config key therefore overrides it). */
constexpr char kIspDefault[] = "saturation=1.0,gamma=2.2";
constexpr char kIspLowLight[] =
	"ae-enable=false,saturation=1.0,gamma=2.2";

std::string isp_spec()
{
	if (const char *env = std::getenv("ERHAI_CAM_ISP")) {
		const std::string spec(env);

		if (spec == "lowlight")
			return kIspLowLight;
		if (spec == "default" || spec == "on" || spec == "pins" ||
		    spec == "gain")
			return kIspDefault;
		if (spec == "none" || spec == "off" || spec == "raw" ||
		    spec == "isp")
			return {};
		return spec;
	}
	const std::string path = config_path();
	std::string spec = kIspDefault;

	if (path.empty())
		return spec;
	GKeyFile *kf = g_key_file_new();

	if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE,
				      nullptr)) {
		gsize n = 0;
		gchar **keys = g_key_file_get_keys(kf, "isp", &n, nullptr);

		for (gsize i = 0; keys && i < n; i++) {
			gchar *v = g_key_file_get_string(kf, "isp", keys[i],
							nullptr);

			if (v) {
				if (!spec.empty())
					spec += ",";
				spec += std::string(keys[i]) + "=" + v;
				g_free(v);
			}
		}
		g_strfreev(keys);
	}
	g_key_file_unref(kf);
	return spec;
}

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

/* Manual/auto exposure writes the sensor's integration-time register
 * directly.  The kernel drivers accept V4L2_CID_EXPOSURE but their s_ctrl is
 * a no-op ("accepted but not yet written to hardware", drivers/media/i2c/
 * sc1320cs.c and sc820cs.c), so the control is useless and the app is the
 * register's owner.
 *
 * Both cameras are supported (2.8): the app resolves the i2c client of the
 * sensor the CURRENT camera streams from — "sc1320cs" for the rear,
 * "sc820cs" for the front — and carries that sensor's own limits.  Before
 * 2.8 only the rear was owned, which left the front camera entirely to the
 * software ISP's in-software gain (the yellow veil).
 *
 * The sensor's rails are only powered while it streams, so every write must
 * be guarded by app->camera_ok or the CCI transaction NAKs.  udev rule
 * 70-erhai-cam-i2c.rules grants the session user rw on the CCI adapter
 * (uaccess); the CCI masters enumerate in probe order and the bus numbers
 * swap between boots, so the bus is resolved from the i2c client's NAME —
 * never hardcoded. */
class Sensor {
public:
	static std::optional<std::string> find(const SensorSpec &spec)
	{
		const std::filesystem::path bus("/sys/bus/i2c/devices");
		std::error_code ec;

		for (const auto &entry :
		     std::filesystem::directory_iterator(bus, ec)) {
			if (read_text_file(entry.path() / "name") != spec.name)
				continue;
			const std::string dev = entry.path().filename();
			const auto dash = dev.find('-');
			if (dash == std::string::npos)
				continue;
			return "/dev/i2c-" + dev.substr(0, dash);
		}
		return std::nullopt;
	}

	explicit Sensor(const std::string &path, const SensorSpec &spec)
		: path_(path), spec_(spec), exposure_(spec.default_lines),
		  vts_(spec.vts_default)
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

	/* This sensor's own limits (the fader and the AE ask for them).  Both are
	 * DYNAMIC: the integration time can never exceed the frame length, so
	 * lengthening the frame (set_frame_lines) raises them. */
	int max_lines() const { return vts_ - 8; }
	int ae_max() const
	{
		return std::max(kExposureMin, max_lines() - spec_.ae_reserve);
	}
	const char *name() const { return spec_.name; }
	int frame_lines() const { return vts_; }
	int vts_default() const { return spec_.vts_default; }
	int vts_max() const { return spec_.vts_max; }

	/* LENGTHEN (or restore) the frame: VTS is the sensor's frame length and the
	 * ceiling of its integration time, so a longer frame is how a dark scene
	 * gets more light at a lower frame rate (0x320e/0f, 16-bit big-endian).
	 * Takes effect on the running stream (measured 2026-10-21: VTS 3128->6240
	 * on the front moved luma 43.4 -> 48.7 and the signal band 17.9 -> 24.3,
	 * i.e. real light with the contrast intact).  Exposure is clamped to the
	 * new frame by the caller. */
	bool set_frame_lines(int vts)
	{
		vts = std::clamp(vts, spec_.vts_default, spec_.vts_max);
		if (!write_reg(kVtsRegHi, vts >> 8) ||
		    !write_reg(kVtsRegLo, vts & 0xff))
			return false;
		vts_ = vts;
		return true;
	}

	bool set_exposure_lines(int lines)
	{
		lines = std::clamp(lines, kExposureMin, max_lines());
		const unsigned value = unsigned(lines) << kExposureShift;
		unsigned char msg[5] = {
			0x3e, 0x00,
			(unsigned char)((value >> 16) & 0xff),
			(unsigned char)((value >> 8) & 0xff),
			(unsigned char)(value & 0xff),
		};
		struct i2c_msg m = {};
		struct i2c_rdwr_ioctl_data xfer = { &m, 1 };

		m.addr = spec_.addr;
		m.flags = 0;
		m.len = sizeof(msg);
		m.buf = msg;
		/* I2C_RDWR, not I2C_SLAVE + write(): the kernel sensor driver
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

		msgs[0].addr = spec_.addr;
		msgs[0].flags = 0;
		msgs[0].len = sizeof(addr);
		msgs[0].buf = addr;
		msgs[1].addr = spec_.addr;
		msgs[1].flags = I2C_M_RD;
		msgs[1].len = sizeof(data);
		msgs[1].buf = data;

		if (ioctl(fd_, I2C_RDWR, &xfer) < 0)
			return false;

		const unsigned value = (unsigned(data[0]) << 16) |
				       (unsigned(data[1]) << 8) | data[2];
		const int decoded = int(value >> kExposureShift);

		if (decoded <= 0 || decoded > max_lines())
			return false;
		exposure_ = decoded;
		if (lines)
			*lines = decoded;
		return true;
	}

private:
	/* One 8-bit register (2-byte address + value), I2C_RDWR — the same
	 * transaction the exposure burst uses, so it works while the kernel
	 * driver holds the CCI bus. */
	bool write_reg(int reg, int value)
	{
		unsigned char msg[3] = {
			(unsigned char)((reg >> 8) & 0xff),
			(unsigned char)(reg & 0xff),
			(unsigned char)(value & 0xff),
		};
		struct i2c_msg m = {};
		struct i2c_rdwr_ioctl_data xfer = { &m, 1 };

		m.addr = spec_.addr;
		m.flags = 0;
		m.len = sizeof(msg);
		m.buf = msg;
		return ioctl(fd_, I2C_RDWR, &xfer) >= 0;
	}

	std::string path_;
	SensorSpec spec_;
	int fd_ = -1;
	int exposure_ = kExposureDefault;
	int vts_ = kRearSensor.vts_default;   /* current frame length */
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

/* The video soundtrack, chosen with the note button in the bottom bar. */
enum class AudioMode { None, Microphone };

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
 * the loop never leaves the streaming sensor's own AE ceiling (4800 of 6396
 * lines on the rear, 3120 of 3120 on the front) so the raw cannot saturate
 * into the soft ISP's inverting region.  Since 2.8 the loop runs on BOTH
 * cameras. */
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
	/* The window is being destroyed: every widget pointer below is gone, so
	 * the streaming thread / GStreamer bus callbacks must stop touching the
	 * UI (they all null-check their widget already). */
	bool ui_dead = false;
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
	/* video soundtrack picker (bottom bar, note icon + a popover) */
	GtkWidget *btn_audio = nullptr;
	GtkWidget *audio_popover = nullptr;
	GtkWidget *audio_none = nullptr;
	GtkWidget *audio_mic = nullptr;
	AudioMode audio_mode = AudioMode::None;
	bool audio_sync = false;    /* programmatic popover update in flight */
	/* resolution picker (bottom-left corner, display icon + a popover).
	 * ONE choice per camera, persisted in ~/.config/erhai/erhaicam.conf;
	 * the popover is REBUILT whenever the camera changes, because the two
	 * cameras offer different lists. */
	GtkWidget *btn_res = nullptr;
	GtkWidget *res_popover = nullptr;
	GtkWidget *res_box = nullptr;
	std::vector<GtkWidget *> res_checks;   /* current popover's buttons */
	int rear_res = 0;    /* index into kRearResolutions */
	int front_res = 0;   /* index into kFrontResolutions */
	bool res_sync = false;   /* programmatic check-button update in flight */
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
	/* the libcamerasrc element of the preview pipeline: the ISP controls
	 * (libcamera camera controls) live on it (see isp_spec) */
	GstElement *camsrc = nullptr;
	/* the app's grey-world white balance + the MANUAL warm/cool fader
	 * (see setup_wb / wb_probe_cb / wb_gains_from_state) */
	bool wb_grey = false;       /* automatic grey-world mode (env/config) */
	bool wb_manual = false;     /* the fader's value is in use */
	bool wb_active = false;     /* the WB toggle button (lit = applied) */
	GtkWidget *btn_wb = nullptr;
	int wb_warmth = 0;          /* -100 (cool) … +100 (warm) */
	GtkWidget *wb_scale = nullptr;
	GtkWidget *wb_value = nullptr;
	bool wb_slider_sync = false;
	guint wb_save_timer = 0;
	int wb_stride = 0;
	double wb_r = 1.0;
	double wb_b = 1.0;
	GstPad *wb_probe_pad = nullptr;
	gulong wb_probe = 0;
	GstVideoInfo wb_info{};
	bool wb_info_valid = false;
	unsigned long wb_frame = 0;
	GstElement *rpipe = nullptr;
	GstElement *rsrc = nullptr;
	GstElement *raudio = nullptr;   /* the mic source of the recording pipeline */
	bool recording = false;
	std::string rec_path;
	/* First recorded frame's PTS: the recording pipeline's zero (the camera
	 * timestamps come from the preview pipeline's time base, the mic's from
	 * its own — mixing them puts the video at the end of the movie). */
	GstClockTime rec_base = GST_CLOCK_TIME_NONE;
	/* The first picture frame must be the one CAPTURED when the microphone
	 * went live (rec_audio_shift), not the next one to arrive: without this
	 * the movie's picture starts up to a camera latency (~0.25 s) before the
	 * sound (measured 2026-10-20: the sound lagged by 0.27-0.32 s).  This is
	 * that frame's capture stamp, i.e. rec_audio_shift mapped into the
	 * camera's clock — see on_new_sample.  NONE = not known yet. */
	GstClockTime rec_anchor = GST_CLOCK_TIME_NONE;
	/* dead-air trim: the mic delivers EXACT zeros for its first ~0.15-0.3 s
	 * (see rec_audio_probe_cb) — those buffers are dropped, the track is
	 * re-based onto the first real one, and the video waits for it.
	 *
	 * The app opens the microphone ONLY when Record is pressed, on purpose:
	 * a capture stream that runs outside a recording (even one whose data is
	 * thrown away, as an earlier version did to hide the cold-start latency)
	 * is a microphone we do not need — owner decision 2026-10-20, "no audio
	 * recording before start is pressed -> security: we do not spy on
	 * users".  The cold-start gap is handled by trimming it instead. */
	bool rec_audio_live = false;
	GstClockTime rec_audio_shift = GST_CLOCK_TIME_NONE;
	int rec_audio_dropped = 0;   /* dead-air buffers thrown away */
	GstPad *rec_audio_pad = nullptr;
	gulong rec_audio_probe = 0;
	Steady::time_point rec_start = Steady::time_point::min();
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

	if (!msg->app->status || msg->app->ui_dead) {
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
			if (!app->status || app->ui_dead)
				return G_SOURCE_REMOVE;
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

/* --------------------------- resolution picker ---------------------------- */
/* The per-camera size menu (the button, its popover and the persistence) is
 * implemented further down, next to the soundtrack picker; these are declared
 * here because the preview pipeline and the recording lock use them. */
static const std::vector<ResOption> &res_options(const App *app);
static int res_index(const App *app);
static void res_menu_rebuild(App *app);
static void res_update_button(App *app);
static void res_set_locked(App *app, bool locked);

/* Defined with the photo probe further down, used per frame. */
static bool frame_rgb_mean(GstSample *sample, double *r, double *g, double *b);
/* the app's in-place white balance pass (defined with setup_wb) */
static void wb_detach(App *app);

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

/* ------------------------ the exposure register owner -------------------- */

/* The span the exposure fader spans for the camera in use (the two sensors
 * have different frame lengths, so the fader must change with the camera). */
static int exp_span(const App *app)
{
	return app->sensor_ok ? app->sensor->max_lines() : kExposureMax;
}

/* Resolve and own the integration-time register of the sensor the CURRENT
 * camera streams from.  ErhaiCam 2.8 owns BOTH sensors — the rear sc1320cs and
 * the front sc820cs — because the front was previously left entirely to the
 * software ISP, whose only remaining lever is DIGITAL gain (libcamera has no
 * exposure/gain model for either sensor, and both kernel controls are no-ops):
 * that software gain lifts the black level and, with the ISP's luminance-
 * dependent `Adjust` curve, turned a dark room into the owner's "yellowish
 * taint" (measured 2026-10-21).
 *
 * Called at startup and on EVERY camera switch: each sensor has its own i2c
 * client, address and limits, and the fd belongs to one camera at a time.  The
 * rails are powered only while a sensor streams, so the read-back is best
 * effort — a silent register keeps the mode-table default. */
static void sensor_attach(App *app)
{
	const SensorSpec &spec = app->rear ? kRearSensor : kFrontSensor;

	if (app->sensor) {
		delete app->sensor;
		app->sensor = nullptr;
	}
	app->sensor_ok = false;
	app->ae.exposure = spec.default_lines;   /* mode table default for now */
	if (auto path = Sensor::find(spec)) {
		try {
			app->sensor = new Sensor(*path, spec);
			app->sensor_ok = true;
			app->ae.exposure = app->sensor->ae_max();  /* its ceiling */
			std::cout << "Sensor device: " << *path << " (" << spec.name
				  << ", frame " << spec.vts_default << " lines (low "
				  << "light up to " << spec.vts_max << "), exposure 1.."
				  << app->sensor->max_lines() << ", AE up to "
				  << app->sensor->ae_max() << ")\n";
			int lines = 0;

			if (app->sensor->read_exposure_lines(&lines)) {
				app->ae.exposure = lines;
				std::cout << "Sensor exposure: " << lines
					  << " lines\n";
			}
		} catch (const std::exception &e) {
			std::cerr << "sensor: " << e.what() << "\n";
		}
	}
	if (!app->sensor_ok)
		std::cerr << "no " << spec.name << " i2c client; exposure control "
			     "disabled for the " << (app->rear ? "rear" : "front")
			  << " camera\n";
	/* The AE's tracked exposure belonged to the OLD sensor. */
	app->ae.stride = 0;
	app->ae.settle = kAeSettle;
	app->ae.measured = false;
	app->ae.at_limit = false;
}

/* --------------------- manual controls / AE-AF buttons ------------------- */

/* ONE place decides how the AF/AE buttons and the two manual sliders look:
 * each button is a toggle whose "off" state summons the matching vertical
 * slider underneath, and a slider is only usable while the camera it drives
 * is streaming (the VCM rails and the sensor register only exist then). */
static void app_sync_controls(App *app)
{
	if (app->btn_ae) {
		/* Both cameras expose a driver register the app owns (2.8). */
		gtk_widget_set_sensitive(app->btn_ae, app->sensor_ok);
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

	const bool exp_manual = app->sensor_ok && !app->ae.enabled;
	if (app->exp_scale) {
		/* The two sensors have different frame lengths: span THIS one. */
		gtk_range_set_range(GTK_RANGE(app->exp_scale), kExposureMin,
				    exp_span(app));
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

	if (!ae.enabled || !app->sensor_ok || !app->camera_ok)
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

	/* The ceiling is the STREAMING sensor's (rear 4800 of 6396, front 3120). */
	int ae_max = app->sensor->ae_max();

	/* --- THE LOW-LIGHT LADDER (2.9) -------------------------------------
	 * A frame's integration time can never exceed its frame length, and the
	 * mode table fixes that at 3128 lines (front) / 6404 (rear) — so once the
	 * exposure is at the ceiling the ONLY way to get more light is to
	 * LENGTHEN THE FRAME: 2x the frame length is 2x the integration time at
	 * half the frame rate.  Measured 2026-10-21 on the front camera in a dim
	 * room: VTS 3128 -> 6240 took the frame from luma 43.4 / band 17.9 to
	 * 48.7 / 24.3 (real light, contrast INTACT), where every ISP-side option
	 * changed nothing.  Lengthening when dark, and giving the frame rate back
	 * when not, is what makes a low-light picture usable instead of "bad".
	 * The highlight guard above (kRawExposureEpsilon) already stops brightening
	 * when a longer exposure would blow the highlights. */
	if (step > 0.0 && ae.exposure >= ae_max &&
	    app->sensor->frame_lines() < app->sensor->vts_max()) {
		const int vts = std::min(app->sensor->frame_lines() * 2,
					 app->sensor->vts_max());

		if (app->sensor->set_frame_lines(vts) &&
		    app->sensor->set_exposure_lines(app->sensor->max_lines())) {
			ae.exposure = app->sensor->max_lines();
			ae.settle = kAeSettle * 2;
			ae.measured = false;
			std::cerr << "erhai-cam: low light: frame " << vts
				  << " lines, exposure " << ae.exposure << "\n";
		}
		return;                       /* re-measure at the new setting */
	}
	/* Too bright and the long frame is clearly not needed any more: halve it
	 * back towards the mode table's length (the frame rate comes back). */
	if (step < 0.0 && app->sensor->frame_lines() > app->sensor->vts_default() &&
	    ae.exposure <= (app->sensor->vts_default() - 8) / 2) {
		const int vts = std::max(app->sensor->vts_default(),
					 app->sensor->frame_lines() / 2);

		if (app->sensor->set_frame_lines(vts)) {
			app->sensor->set_exposure_lines(ae.exposure);
			ae.settle = kAeSettle * 2;
			std::cerr << "erhai-cam: low light off: frame " << vts
				  << " lines, exposure " << ae.exposure << "\n";
		}
		ae_max = app->sensor->ae_max();
	}

	int next = ae.exposure;
	if (step != 0.0) {
		next = std::clamp(int(std::lround(ae.exposure * std::exp2(step))),
				  kExposureMin, ae_max);
		if (next == ae.exposure)   /* rounding at the very bottom */
			next = std::clamp(ae.exposure + (step > 0.0 ? 1 : -1),
					  kExposureMin, ae_max);
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

	const bool dark_end = ae.exposure >= ae_max && err > kAeDeadband;
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

/* Tear the recording pipeline down (used by both bus outcomes).  The two
 * source refs taken by gst_bin_get_by_name() are dropped with it. */
static void record_pipeline_clear(App *app)
{
	if (app->rec_audio_pad) {
		if (app->rec_audio_probe)
			gst_pad_remove_probe(app->rec_audio_pad,
					     app->rec_audio_probe);
		app->rec_audio_probe = 0;
		gst_object_unref(app->rec_audio_pad);
		app->rec_audio_pad = nullptr;
	}
	if (app->rsrc) {
		gst_object_unref(app->rsrc);
		app->rsrc = nullptr;
	}
	if (app->raudio) {
		gst_object_unref(app->raudio);
		app->raudio = nullptr;
	}
	if (app->rpipe) {
		gst_element_set_state(app->rpipe, GST_STATE_NULL);
		gst_object_unref(app->rpipe);
		app->rpipe = nullptr;
	}
}

static gboolean rec_bus_cb(GstBus *, GstMessage *msg, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
		record_pipeline_clear(app);
		app->recording = false;
		res_set_locked(app, false);
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
		if (app->rpipe)
			record_pipeline_clear(app);
		app->recording = false;
		res_set_locked(app, false);
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
	res_set_locked(app, false);
	if (app->btn_rec) {
		gtk_button_set_label(GTK_BUTTON(app->btn_rec), "Record");
		gtk_widget_remove_css_class(app->btn_rec, "recording");
	}
	if (app->rsrc)
		gst_app_src_end_of_stream(GST_APP_SRC(app->rsrc));
	/* The mic is a live source: it never ends by itself, and a muxer waits
	 * for EOS on EVERY sink pad, so the soundtrack must be ended explicitly
	 * or the MP4 is never finalized. */
	if (app->raudio)
		gst_element_send_event(app->raudio, gst_event_new_eos());
	/* rpipe is finalized by the EOS/error bus callback. */
}

/* Wait up to timeout_ms for the recording pipeline's EOS, pumping the default
 * main context (the bus watch lives on it) so the muxer can write its index —
 * only used when the app quits while a recording is still running. */
static void record_finish(App *app, int timeout_ms)
{
	for (int waited = 0; app->rpipe && waited < timeout_ms; waited += 20) {
		while (g_main_context_iteration(nullptr, FALSE))
			;   /* drain what is pending */
		g_usleep(20 * 1000);
	}
}

/* The recording pipeline's audio source.  ORDER MATTERS (2026-10-20):
 * `pulsesrc` first — the PULSE-protocol client of the same PipeWire server
 * gives an EXACT sample timeline (measured: declared duration == sample count,
 * 0 timeline gaps), while the native `pipewiresrc` client forwards PipeWire's
 * own chunking and its timestamps leave ~1 % of the audio as small gaps
 * (~5-30 ms, one every ~0.3 s) — audible as crackle/dropouts, worst right
 * after the stream starts while the mic node is still ramping (owner report:
 * "audio cracks in the first ~2 sec").  `pipewiresrc` stays as the second
 * choice, raw ALSA hw:0,2 (the SoundWire capture PCM, no session layer) as
 * the last.  The element is always NAMED "mic" so the stop path can send it
 * the EOS. */
static const char *audio_source_desc()
{
	static const struct {
		const char *factory;
		const char *desc;
	} sources[] = {
		{ "pulsesrc", "pulsesrc name=mic do-timestamp=true" },
		{ "pipewiresrc", "pipewiresrc name=mic do-timestamp=true" },
		{ "alsasrc", "alsasrc name=mic device=hw:0,2 do-timestamp=true" },
	};

	for (const auto &s : sources) {
		GstElementFactory *factory = gst_element_factory_find(s.factory);

		if (!factory)
			continue;
		gst_object_unref(factory);
		return s.desc;
	}
	return nullptr;
}

/* Throw the mic's DEAD AIR away and start the movie with the first real audio.
 *
 * The pad's capture path delivers ~0.15-0.25 s of EXACT zeros after a client
 * connects (the SoundWire/DSP capture graph plus the codec's ADC ramp; the
 * app opens the mic in app_record_start, i.e. when Record is pressed).  A take
 * therefore began with a hole of dead silence that ended in an abrupt onset -
 * the owner's "small problem maybe in the first 500 ms", made while he was
 * already speaking.  This probe drops the all-zero buffers, remembers the PTS
 * of the first real one and re-bases the whole soundtrack onto it, so the
 * audio starts at 0 together with the first picture frame (on_new_sample holds
 * the video back until rec_audio_live) — "throw away the first ~300 ms of
 * audio and video and save only the remainder", as the owner asked. */
static GstPadProbeReturn rec_audio_probe_cb(GstPad *, GstPadProbeInfo *info,
					    gpointer ud)
{
	auto *app = static_cast<App *>(ud);
	GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);

	if (!buf)
		return GST_PAD_PROBE_OK;

	if (!app->rec_audio_live) {
		GstMapInfo map;
		bool silent = true;

		if (!gst_buffer_map(buf, &map, GST_MAP_READ))
			return GST_PAD_PROBE_DROP;
		for (gsize i = 0; i < map.size; i++) {
			if (map.data[i]) {
				silent = false;
				break;
			}
		}
		if (silent) {
			gst_buffer_unmap(buf, &map);
			app->rec_audio_dropped++;
			return GST_PAD_PROBE_DROP;   /* dead air */
		}
		app->rec_audio_shift = GST_BUFFER_PTS(buf);
		app->rec_audio_live = true;
		gst_buffer_unmap(buf, &map);
		if (!app->headless)
			std::cerr << "erhai-cam: mic live after "
				  << app->rec_audio_dropped << " dead buffers\n";
	}

	if (GST_CLOCK_TIME_IS_VALID(app->rec_audio_shift) &&
	    GST_BUFFER_PTS_IS_VALID(buf) &&
	    GST_BUFFER_PTS(buf) >= app->rec_audio_shift) {
		GST_BUFFER_PTS(buf) -= app->rec_audio_shift;
		if (GST_BUFFER_DTS_IS_VALID(buf) &&
		    GST_BUFFER_DTS(buf) >= app->rec_audio_shift)
			GST_BUFFER_DTS(buf) -= app->rec_audio_shift;
	}
	return GST_PAD_PROBE_OK;
}

/* The app opens the microphone ONLY for an actual recording (see
 * app_record_start / record_pipeline_clear): no capture stream is ever left
 * running to hide the cold-start latency — owner decision 2026-10-20, "no audio
 * recording before start is pressed -> security: we do not spy on users".  An
 * earlier 2.0/2.1 attempt at a warm-up keep-alive (throwaway stream, data ->
 * fakesink, enabled with ERHAI_CAM_MIC_WARM=1) was REMOVED for that reason. */

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

	/* Container/encoder candidates, in preference order.  `format` is what the
	 * encoder eats: v4l2h264enc (the Iris hardware encoder) accepts ONLY NV12
	 * — the old I420 request made gst_parse_launch fail its link, so every
	 * recording silently fell through to the software VP8 branch (2026-10-20).
	 * `aenc`/`aparse` are the soundtrack for that container. */
	static const struct {
		const char *enc;
		const char *format;
		const char *parse;
		const char *mux;
		const char *ext;
		const char *aenc;
		const char *aparse;
	} candidates[] = {
		{ "v4l2h264enc", "NV12", "h264parse", "mp4mux", ".mp4",
		  "voaacenc", "aacparse" },
		{ "vp8enc", "I420", nullptr, "webmmux", ".webm",
		  "opusenc", nullptr },
	};

	/* The soundtrack source, when the mic is selected AND it exists. */
	const char *audio_src = app->audio_mode == AudioMode::Microphone ?
				audio_source_desc() : nullptr;

	/* Every recording starts its own video time base (see on_new_sample) and
	 * its own dead-air trim (see rec_audio_probe_cb). */
	app->rec_base = GST_CLOCK_TIME_NONE;
	app->rec_audio_shift = GST_CLOCK_TIME_NONE;
	app->rec_audio_dropped = 0;
	app->rec_start = Steady::now();

	for (const auto &c : candidates) {
		GstElementFactory *ef = gst_element_factory_find(c.enc);
		GstElementFactory *mf = gst_element_factory_find(c.mux);

		if (ef)
			gst_object_unref(ef);
		if (mf)
			gst_object_unref(mf);
		if (!ef || !mf)
			continue;

		GstElementFactory *af = c.aenc ?
			gst_element_factory_find(c.aenc) : nullptr;
		const bool have_aenc = af != nullptr;

		if (af)
			gst_object_unref(af);

		/* At most two passes per candidate: with the mic, then silent — a
		 * soundtrack that cannot come up must still leave a usable video. */
		for (int pass = (audio_src && have_aenc) ? 0 : 1; pass < 2; pass++) {
			const bool audio = audio_src && have_aenc && pass == 0;

			std::string name = timestamp_name("VID", c.ext);
			std::string desc =
				std::string("appsrc name=src do-timestamp=true format=time ! videoconvert ! "
					    "video/x-raw,format=") + c.format + " ! " +
				c.enc + (c.parse ? std::string(" ! ") + c.parse : "") +
				" ! queue ! mux. ";
			if (audio)
				desc += std::string(audio_src) +
					" ! audioconvert ! audioresample ! "
					"audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
					"queue ! " + c.aenc +
					(c.aparse ? std::string(" ! ") + c.aparse : "") +
					" ! queue ! mux. ";
			desc += std::string(c.mux) +
				" name=mux ! filesink location=\"" +
				(base / name).string() + "\"";

			GError *gerr = nullptr;
			GstElement *rpipe = gst_parse_launch(desc.c_str(), &gerr);
			if (gerr || !rpipe) {
				std::cerr << "record pipeline: "
					  << (gerr ? gerr->message : "?") << "\n";
				g_clear_error(&gerr);
				continue;
			}

			GstElement *src = gst_bin_get_by_name(GST_BIN(rpipe),
							      "src");
			GstElement *mic = audio ?
				gst_bin_get_by_name(GST_BIN(rpipe), "mic") :
				nullptr;

			g_object_set(src, "caps", caps, nullptr);
			gst_app_src_set_stream_type(GST_APP_SRC(src),
						    GST_APP_STREAM_TYPE_STREAM);

			app->rec_path = (base / name).string();
			gst_bus_add_watch(gst_element_get_bus(rpipe), rec_bus_cb,
					  app);
			if (gst_element_set_state(rpipe, GST_STATE_PLAYING) ==
			    GST_STATE_CHANGE_FAILURE) {
				if (mic)
					gst_object_unref(mic);
				gst_object_unref(src);
				gst_object_unref(rpipe);
				continue;
			}

			app->rpipe = rpipe;
			app->rsrc = src;
			app->raudio = mic;
			app->recording = true;
			/* The resolution is fixed for the whole take. */
			res_set_locked(app, true);
			/* The mic's dead air is trimmed at the source (see
			 * rec_audio_probe_cb); with no soundtrack there is
			 * nothing to wait for. */
			if (mic) {
				GstPad *pad = gst_element_get_static_pad(mic,
									"src");

				if (pad) {
					app->rec_audio_pad = pad;
					app->rec_audio_probe = gst_pad_add_probe(
						pad,
						GST_PAD_PROBE_TYPE_BUFFER,
						rec_audio_probe_cb, app,
						nullptr);
				}
			} else {
				app->rec_audio_live = true;
			}
			if (app->btn_rec) {
				gtk_button_set_label(GTK_BUTTON(app->btn_rec),
						     "Stop");
				gtk_widget_add_css_class(app->btn_rec,
							 "recording");
			}
			set_status(app, audio ? "Recording with sound\u2026" :
				(audio_src ? "Recording \u2014 no sound "
					     "(microphone unavailable)"
					   : "Recording\u2026"));
			gst_caps_unref(caps);
			return;
		}
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
		/* A stream start reloads the sensor's mode table, so its frame
		 * length (and with it the exposure ceiling the low-light ladder
		 * raised) is back at the default: put both back in step, and
		 * re-assert the integration time (the AE re-escalates by itself
		 * if the scene is still dark; with AE off this is the manual
		 * value, as before). */
		if (app->sensor_ok && app->sensor) {
			app->sensor->set_frame_lines(app->sensor->vts_default());
			app->sensor->set_exposure_lines(app->ae.exposure);
		} else if (!app->ae.enabled) {
			apply_exposure(app, app->ae.exposure);
		}
		if (!app->rear && !app->headless)
			set_status(app, "Front camera");
		app_sync_controls_async(app);
	}

	/* Hold the picture back until the microphone is actually delivering, so
	 * both branches start at movie 0 and the take is not missing its first
	 * ~0.2 s of sound behind a hole of dead silence (owner report
	 * 2026-10-20; rec_audio_live is set by rec_audio_probe_cb).  The wait is
	 * capped: a mic that never comes up must not block the whole recording.
	 * With the soundtrack OFF there is no audio branch and the flag is set
	 * at record start. */
	if (app->recording && app->rsrc &&
	    (app->rec_audio_live ||
	     Steady::now() - app->rec_start > kRecAudioWait)) {
		app->rec_audio_live = true;   /* the mic never came up */
		GstBuffer *buffer = gst_buffer_ref(gst_sample_get_buffer(sample));

		/* Re-base the frame onto the recording pipeline's own zero.
		 *
		 * The camera's timestamps belong to the PREVIEW pipeline's time
		 * base (they grow with how long the app has been streaming),
		 * while the recording pipeline's other branch — the
		 * microphone — starts at its own zero.  Left alone, the muxer
		 * keeps the difference and the video lands that far into the
		 * movie: the player shows NOTHING (and reports a wrong
		 * duration) until the video's own start, so a recording made
		 * 12 s after launch came out as a ~12 s black lead-in plus
		 * 3.7 s of picture (owner report 2026-10-20).  Subtract the
		 * first KEPT frame's PTS (and DTS) from every frame, so both
		 * branches start at 0.
		 *
		 * WHICH frame is the first one kept matters: the mic went live
		 * at `rec_audio_shift` (pipeline clock) but the frames reaching
		 * us are already a camera latency old, so just taking the next
		 * arrival began the picture ~0.25 s BEFORE the sound and the
		 * sound therefore lagged it.  Map the mic's live instant into
		 * the camera's clock once (`rec_anchor`, using this frame's
		 * capture stamp `pts` and the instant it reached us, `now` —
		 * their difference is the constant offset between the two
		 * clocks) and drop every frame captured before it. */
		const GstClockTime pts = GST_BUFFER_PTS(buffer);
		const GstClockTime dts = GST_BUFFER_DTS(buffer);
		const GstClockTime now =
			gst_element_get_current_running_time(app->rsrc);

		if (GST_CLOCK_TIME_IS_VALID(pts)) {
			if (!GST_CLOCK_TIME_IS_VALID(app->rec_anchor) &&
			    GST_CLOCK_TIME_IS_VALID(app->rec_audio_shift)) {
				const GstClockTime shift =
					app->rec_audio_shift;

				if (GST_CLOCK_TIME_IS_VALID(now) &&
				    GST_CLOCK_TIME_IS_VALID(shift) &&
				    pts > now)
					app->rec_anchor = (pts - now) + shift;
				/* sanity: a plausible anchor sits near this frame
				 * (guards against a torn read of the probe's
				 * value — the audio thread writes it) */
				if (GST_CLOCK_TIME_IS_VALID(app->rec_anchor) &&
				    (app->rec_anchor > pts + 2 * GST_SECOND ||
				     pts > app->rec_anchor + 60 * GST_SECOND)) {
					std::cerr << "erhai-cam: unusable mic anchor; "
						     "starting at the next frame\n";
					app->rec_anchor = GST_CLOCK_TIME_NONE;
				}
			}
			if (GST_CLOCK_TIME_IS_VALID(app->rec_anchor) &&
			    pts < app->rec_anchor) {
				/* captured before the sound exists */
				gst_buffer_unref(buffer);
				buffer = nullptr;
			} else if (!GST_CLOCK_TIME_IS_VALID(app->rec_base)) {
				app->rec_base = pts;
			}
		}
		if (std::getenv("ERHAI_CAM_REC_DEBUG"))
			std::cerr << "erhai-cam: rec-dbg pts=" << pts << " now="
				  << now << " shift=" << app->rec_audio_shift
				  << " anchor=" << app->rec_anchor << " base="
				  << app->rec_base << " kept=" << (buffer != nullptr)
				  << " rebased="
				  << (buffer ? gint64(pts - app->rec_base) : -1)
				  << "\n";
		if (buffer && GST_CLOCK_TIME_IS_VALID(pts)) {
			GST_BUFFER_PTS(buffer) = pts - app->rec_base;
			if (GST_CLOCK_TIME_IS_VALID(dts)) {
				if (dts >= app->rec_base)
					GST_BUFFER_DTS(buffer) = dts -
								app->rec_base;
				else
					GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(
								buffer);
			}
		}
		if (buffer)
			gst_app_src_push_buffer(GST_APP_SRC(app->rsrc), buffer);
	}

	af_step(app, sample);
	ae_step(app, sample);
	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

static void preview_pipeline_stop(App *app)
{
	wb_detach(app);
	if (app->camsrc) {
		gst_object_unref(app->camsrc);
		app->camsrc = nullptr;
	}
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

/* Apply the forced ISP controls to the camera source (a no-op without them).
 * gst_util_set_object_arg() parses the string by the property's own type, so
 * booleans, enums, floats and value arrays ("<0.97, 1.05>") all work. */
static void apply_isp_controls(App *app)
{
	if (!app->camsrc)
		return;
	const std::string spec = isp_spec();

	if (spec.empty())
		return;
	size_t pos = 0;

	std::cerr << "erhai-cam: ISP controls:";
	while (pos <= spec.size()) {
		const size_t comma = spec.find(',', pos);
		std::string item = spec.substr(pos, comma - pos);

		while (!item.empty() && item.front() == ' ')
			item.erase(item.begin());
		const size_t eq = item.find('=');

		if (eq != std::string::npos) {
			const std::string key = item.substr(0, eq);
			const std::string val = item.substr(eq + 1);

			if (!key.empty() && !val.empty() &&
			    g_object_class_find_property(
				    G_OBJECT_GET_CLASS(app->camsrc),
				    key.c_str())) {
				gst_util_set_object_arg(G_OBJECT(app->camsrc),
							key.c_str(),
							val.c_str());
				std::cerr << " " << key << "=" << val;
			} else {
				std::cerr << " (unknown: " << key << ")";
			}
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	std::cerr << "\n";
}

/* ---- the app's own white balance ----------------------------------------
 * The pad has NO white balance by default: libcamerasrc's `awb-enable` is
 * FALSE, and the software ISP's AWB (measured 2026-10-20) barely moves the
 * colour gains, so the picture simply carries the scene's colour temperature —
 * daylight looks right, warm indoor light looks yellow.  Since this app already
 * owns exposure, it can own white balance too: a damped grey-world controller
 * (same spirit as the AF/AE loops) measures the frame's mean R,G,B and drives
 * libcamera's `colour-gains` (R and B relative to green) so the average of the
 * picture is neutral.
 *
 *   ERHAI_CAM_WB=grey   ~/.config/erhai/erhaicam.conf [whitebalance] mode=grey
 *   ERHAI_CAM_WB=off    fixed 1.0/1.0 (no correction, but stable)
 *   ERHAI_CAM_WB=isp    leave the ISP alone (today's default behaviour)
 * The damping (25 % per update, 6 frames apart, ratio clamped to 0.5..2.0)
 * keeps a scene that is genuinely one colour from swinging the whole stream. */
constexpr int kWbStride = 30;             /* frames between UPDATES (~2.5 s) */
constexpr double kWbMinLuma = 25.0;       /* too dark to trust the estimate */
constexpr double kWbStep = 1.03;          /* max ±3 % per update (latency!) */
constexpr double kWbMinGain = 0.6;
constexpr double kWbMaxGain = 1.6;
/* The MANUAL warm/cool fader (owner request 2026-10-20: "a yellowish almost
 * transparent overlay was put over the original image — can we control it?
 * maybe with a slider next to AE"): ±100 on the slider = ±30 % on R, ∓30 % on B,
 * i.e. a real white-balance control the ISP cannot refuse (the app applies it
 * to the frames itself, see wb_probe_cb). */
constexpr int kWbWarmthMin = -100;
constexpr int kWbWarmthMax = 100;
constexpr double kWbWarmthPerStep = 0.003;

static void wb_gains_from_state(const App *app, double *gr, double *gb)
{
	if (!app->wb_active && !app->wb_grey) {   /* WB toggle off: bypassed */
		*gr = 1.0;
		*gb = 1.0;
		return;
	}
	if (app->wb_grey) {          /* the app's slow grey-world loop */
		*gr = app->wb_r;
		*gb = app->wb_b;
		return;
	}
	{                            /* the warm/cool fader */
		const double w = app->wb_warmth;

		*gr = std::clamp(1.0 + kWbWarmthPerStep * w, kWbMinGain,
				 kWbMaxGain);
		*gb = std::clamp(1.0 - kWbWarmthPerStep * w, kWbMinGain,
				 kWbMaxGain);
	}
}

/* defined with the photo probe below; needed by wb_step */
static bool frame_rgb_mean(GstSample *sample, double *r, double *g, double *b);

std::string wb_mode()
{
	if (const char *env = std::getenv("ERHAI_CAM_WB"))
		return env;
	const std::string path = config_path();

	if (path.empty())
		return {};
	GKeyFile *kf = g_key_file_new();
	std::string mode;

	if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE,
				      nullptr)) {
		gchar *v = g_key_file_get_string(kf, "whitebalance", "mode",
						nullptr);

		if (v) {
			mode = v;
			g_free(v);
		}
	}
	g_key_file_unref(kf);
	return mode;
}

/* The gains the ISP cannot apply (measured 2026-10-20: libcamera's ColourGains
 * reaches the debayer on this path as a NO-OP — an extreme <0.6,1.6> changes
 * the image by nothing), so the app scales the R and B channels of every frame
 * ITSELF, in place, right before the tee: one pass over the BGRA pixels, the
 * same way it already owns exposure.  The controller is an integral one: it
 * measures the strided R,G,B of the frame it is about to correct and multiplies
 * the running gains by a damped residual, so it converges to a neutral picture
 * (and back, if the light changes). */
static GstPadProbeReturn wb_probe_cb(GstPad *pad, GstPadProbeInfo *info,
				     gpointer ud)
{
	auto *app = static_cast<App *>(ud);
	GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);

	if ((!app->wb_active && !app->wb_grey) || !buf)
		return GST_PAD_PROBE_OK;

	if (!app->wb_info_valid) {
		GstCaps *caps = gst_pad_get_current_caps(pad);

		if (!caps || !gst_video_info_from_caps(&app->wb_info, caps)) {
			if (caps)
				gst_caps_unref(caps);
			return GST_PAD_PROBE_OK;
		}
		gst_caps_unref(caps);
		if (GST_VIDEO_INFO_FORMAT(&app->wb_info) != GST_VIDEO_FORMAT_BGRA)
			return GST_PAD_PROBE_OK;
		app->wb_info_valid = true;
	}

	GstVideoFrame frame;

	if (!gst_video_frame_map(&frame, &app->wb_info, buf,
				 GST_MAP_READWRITE))
		return GST_PAD_PROBE_OK;

	const int w = GST_VIDEO_INFO_WIDTH(&app->wb_info);
	const int h = GST_VIDEO_INFO_HEIGHT(&app->wb_info);
	const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
	uint8_t *base = static_cast<uint8_t *>(
		GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
	double sr = 0, sg = 0, sb = 0;
	long n = 0;

	/* measure on a stride-sampled subset of the UNCORRECTED frame */
	for (int y = 0; y < h; y += 8) {
		const uint8_t *row = base + size_t(y) * stride;

		for (int x = 0; x < w; x += 8) {
			const uint8_t *p = row + size_t(x) * 4;

			sb += p[0];
			sg += p[1];
			sr += p[2];
			n++;
		}
	}

	if (app->wb_grey && n && sg / n >= kWbMinLuma) {
		if (++app->wb_stride >= kWbStride) {
			app->wb_stride = 0;
			const double r = sr / n, g = sg / n, b = sb / n;

			/* multiply the running gains by a small, rate-limited
			 * residual correction: the ISP pipeline lags a frame, so
			 * a fast loop oscillates (measured: every-frame updates
			 * drove both gains into their clamps and the picture
			 * blue). */
			const double step_r = std::clamp(
				g / std::max(r, 1.0), 1.0 / kWbStep, kWbStep);
			const double step_b = std::clamp(
				g / std::max(b, 1.0), 1.0 / kWbStep, kWbStep);

			app->wb_r = std::clamp(app->wb_r * step_r, kWbMinGain,
					       kWbMaxGain);
			app->wb_b = std::clamp(app->wb_b * step_b, kWbMinGain,
					       kWbMaxGain);
			if (std::getenv("ERHAI_CAM_WB_DEBUG"))
				std::cerr << "erhai-cam: wb frame=" << app->wb_frame
					  << " measured=" << int(r) << "," << int(g)
					  << "," << int(b) << " gains R=" << app->wb_r
					  << " B=" << app->wb_b << "\n";
		}
	}

	/* apply the running gains to THIS frame (cheap: 2 multiplies + clamp) */
	double gr = 1.0, gb = 1.0;

	wb_gains_from_state(app, &gr, &gb);
	if (gr != 1.0 || gb != 1.0) {
		for (int y = 0; y < h; y++) {
			uint8_t *row = base + size_t(y) * stride;

			for (int x = 0; x < w; x++) {
				uint8_t *p = row + size_t(x) * 4;

				p[0] = (uint8_t)std::clamp(p[0] * gb, 0.0, 255.0);
				p[2] = (uint8_t)std::clamp(p[2] * gr, 0.0, 255.0);
			}
		}
	}
	app->wb_frame++;
	gst_video_frame_unmap(&frame);
	return GST_PAD_PROBE_OK;
}

/* The ISP cannot do the white balance, so attach the app's own pass. */
static void wb_attach(App *app)
{
	if ((!app->wb_active && !app->wb_grey) || !app->pipe || app->wb_probe_pad)
		return;

	GstElement *tee = gst_bin_get_by_name(GST_BIN(app->pipe), "t");

	if (!tee)
		return;
	GstPad *sink = gst_element_get_static_pad(tee, "sink");

	gst_object_unref(tee);
	if (!sink)
		return;
	app->wb_probe_pad = sink;
	app->wb_probe = gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER,
					  wb_probe_cb, app, nullptr);
}

static void wb_detach(App *app)
{
	if (app->wb_probe_pad && app->wb_probe)
		gst_pad_remove_probe(app->wb_probe_pad, app->wb_probe);
	app->wb_probe = 0;
	if (app->wb_probe_pad) {
		gst_object_unref(app->wb_probe_pad);
		app->wb_probe_pad = nullptr;
	}
	app->wb_info_valid = false;
}

static void setup_wb(App *app)
{
	const std::string mode = wb_mode();

	if (mode == "grey" || mode == "auto") {
		app->wb_grey = true;
		std::cerr << "erhai-cam: white balance: grey-world (app)\n";
	} else if (mode == "off" || mode == "fixed") {
		app->wb_r = 1.0;
		app->wb_b = 1.0;
		std::cerr << "erhai-cam: white balance: fixed 1.0/1.0\n";
	} else if (app->wb_manual || mode == "manual") {
		app->wb_manual = true;
		if (mode == "manual")
			app->wb_active = true;
		std::cerr << "erhai-cam: white balance: manual warmth="
			  << app->wb_warmth << " active="
			  << (app->wb_active ? 1 : 0) << "\n";
	} else if (!mode.empty() && mode != "isp") {
		std::cerr << "erhai-cam: white balance: unknown mode '" << mode
			  << "'\n";
	} else {
		std::cerr << "erhai-cam: white balance: ISP (unchanged)\n";
	}
	wb_attach(app);
}

/* Load the stored warm/cool value (mode=manual in the config, or warmth=). */
static void load_wb_config(App *app)
{
	const std::string path = config_path();

	if (path.empty())
		return;
	GKeyFile *kf = g_key_file_new();

	if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE,
				      nullptr)) {
		/* A stored warmth is meant to be USED unless the config
		 * explicitly says the WB toggle is off. */
		GError *err = nullptr;
		const int warmth = g_key_file_get_integer(kf, "whitebalance",
							  "warmth", &err);

		if (!err) {
			app->wb_warmth = std::clamp(warmth, kWbWarmthMin,
						    kWbWarmthMax);
			app->wb_manual = true;
			app->wb_active = true;
		} else {
			g_error_free(err);
		}
		err = nullptr;
		const gboolean active = g_key_file_get_boolean(
			kf, "whitebalance", "active", &err);

		if (!err)
			app->wb_active = active;
		else
			g_error_free(err);
	}
	g_key_file_unref(kf);
}

static void save_wb_config(const App *app)
{
	const std::string path = config_path();

	if (path.empty())
		return;
	GKeyFile *kf = g_key_file_new();
	std::error_code ec;

	std::filesystem::create_directories(
		std::filesystem::path(path).parent_path(), ec);
	g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr);
	g_key_file_set_integer(kf, "whitebalance", "warmth", app->wb_warmth);
	g_key_file_set_boolean(kf, "whitebalance", "active", app->wb_active);
	g_key_file_set_comment(kf, "whitebalance", nullptr,
			       "White balance: the app applies these gains itself "
			       "(the ISP ignores them)", nullptr);
	gchar *text = g_key_file_to_data(kf, nullptr, nullptr);

	if (text)
		g_file_set_contents(path.c_str(), text, -1, nullptr);
	g_free(text);
	g_key_file_unref(kf);
}

/* The WB toggle next to AE: lit = the white balance is applied, dim = the
 * picture is left exactly as the ISP delivers it.  The fader follows it (and
 * the auto grey-world mode takes the button over). */
static void wb_sync_button(App *app)
{
	if (!app->btn_wb)
		return;
	const bool on = app->wb_active || app->wb_grey;

	if (on) {
		gtk_widget_add_css_class(app->btn_wb, "on");
		gtk_widget_remove_css_class(app->btn_wb, "off");
	} else {
		gtk_widget_add_css_class(app->btn_wb, "off");
		gtk_widget_remove_css_class(app->btn_wb, "on");
	}
	gtk_widget_set_sensitive(app->btn_wb, !app->wb_grey);
	gtk_widget_set_tooltip_text(app->btn_wb, on ?
		"White balance: ON — the app corrects the colour of the "
		"picture (this camera's own white balance does nothing). "
		"Tap to leave the colours untouched." :
		"White balance: OFF — colours are whatever the camera gives. "
		"Tap to switch the correction on.");
	/* the fader AND its value readout follow the toggle the same way the
	 * AE/AF faders do: shown only while the WB correction is on */
	if (app->wb_scale) {
		gtk_widget_set_visible(app->wb_scale, on);
		gtk_widget_set_sensitive(app->wb_scale, on);
	}
	if (app->wb_value) {
		gtk_widget_set_visible(app->wb_value, on);
		gtk_widget_set_sensitive(app->wb_value, on);
	}
}

static void save_wb_config(const App *app);
static void wb_sync_button(App *app);

/* Push the current state into the widgets WITHOUT changing it (used at
 * startup, where the config's on/off must survive). */
static void wb_sync_ui(App *app)
{
	if (app->wb_scale && !app->wb_slider_sync) {
		app->wb_slider_sync = true;
		gtk_range_set_value(GTK_RANGE(app->wb_scale), app->wb_warmth);
		app->wb_slider_sync = false;
	}
	if (app->wb_value) {
		const std::string text =
			(app->wb_warmth > 0 ? "+" : "") +
			std::to_string(app->wb_warmth);

		gtk_label_set_text(GTK_LABEL(app->wb_value), text.c_str());
	}
	wb_sync_button(app);
}

/* The warm/cool fader: + = warmer (R up, B down), - = cooler.  Moving the
 * fader also switches the WB pass ON (dragging it must do something), and the
 * value is only written to the config once the drag settles — GTK animates a
 * slider, so every animation step emits value-changed and the last of those is
 * not necessarily the final value. */
static gboolean wb_save_later_cb(gpointer data)
{
	auto *app = static_cast<App *>(data);

	app->wb_save_timer = 0;
	save_wb_config(app);
	return G_SOURCE_REMOVE;
}

static void wb_set_warmth(App *app, int warmth, bool announce)
{
	app->wb_warmth = std::clamp(warmth, kWbWarmthMin, kWbWarmthMax);
	if (!app->wb_grey) {
		app->wb_manual = true;
		app->wb_active = true;
		wb_attach(app);
	}
	wb_sync_ui(app);
	if (app->wb_save_timer)
		g_source_remove(app->wb_save_timer);
	app->wb_save_timer = g_timeout_add(600, wb_save_later_cb, app);
	if (announce && !app->headless)
		set_status(app, app->wb_warmth == 0 ?
			"White balance: neutral (0)" :
			std::string("White balance: ") +
				(app->wb_warmth > 0 ? "warm +" : "cool ") +
				std::to_string(app->wb_warmth));
}

static void on_wb_toggled(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->wb_grey)   /* the auto loop owns the gains */
		return;
	app->wb_active = !app->wb_active;
	if (app->wb_active)
		app->wb_manual = true;
	wb_sync_button(app);
	if (app->wb_save_timer)
		g_source_remove(app->wb_save_timer);
	app->wb_save_timer = g_timeout_add(600, wb_save_later_cb, app);
	if (!app->headless)
		set_status(app, app->wb_active ?
			"White balance: on" : "White balance: off (untouched)");
}

static void on_wb_changed(GtkRange *range, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->wb_slider_sync)
		return;
	if (app->wb_grey) {
		/* the auto loop owns the gains; keep the fader where it is */
		return;
	}
	wb_set_warmth(app, (int)gtk_range_get_value(range), true);
}

static bool preview_pipeline_start(App *app)
{
	static GstAppSinkCallbacks cbs = { nullptr, nullptr, on_new_sample,
					   nullptr, nullptr };

	/* The camera's own list, with the CHOSEN entry first: a stored size
	 * that no longer negotiates still leaves a working preview instead of
	 * a black screen. */
	const auto &opts = res_options(app);
	const int chosen = res_index(app);
	std::vector<std::pair<int, int>> candidates;

	candidates.push_back({ opts[chosen].w, opts[chosen].h });
	for (size_t i = 0; i < opts.size(); i++)
		if (int(i) != chosen)
			candidates.push_back({ opts[i].w, opts[i].h });

	for (const auto &[w, h] : candidates) {
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
		/* The size is asked of LIBCAMERA, i.e. the software ISP scales —
		 * putting it after videoconvert instead made videoconvert scale
		 * the ISP's 4096x2160 (rear) / 3200x2400 (front) output, which is
		 * pure extra CPU for the same frame rate (measured: 11.5 fps
		 * rear / 18 fps front at EVERY size, because the sensor runs one
		 * fixed mode). */
		std::string desc =
			"libcamerasrc name=cam camera-name=\"" + app->camera +
			"\" ! video/x-raw,width=" +
			std::to_string(w) + ",height=" + std::to_string(h) +
			" ! queue ! videoconvert ! "
			"video/x-raw,format=BGRA ! tee name=t "
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
				app->camsrc = gst_bin_get_by_name(GST_BIN(pipe),
								  "cam");
				app->stream_w = w;
				app->stream_h = h;
				/* the ISP controls, if any are forced */
				apply_isp_controls(app);
				/* then the app's own white balance, if enabled */
				setup_wb(app);
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
	/* The OTHER camera has a different sensor register (and their own
	 * limits): hand exposure over to it, then refit the UI to it.  The lens
	 * and the AF loop stay rear-only. */
	sensor_attach(app);
	app->lens_ok = app->rear && app->lens;
	app->af.mode = AfMode::Idle;
	app_sync_controls(app);
	/* The two cameras offer different sizes: show THIS camera's menu and
	 * its own remembered choice (owner requirement). */
	res_menu_rebuild(app);
	res_update_button(app);
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

/* ------------------------- video soundtrack picker ----------------------- */

/* The note button in the bottom bar chooses what the video records:
 * no audio (silent, the default) or the microphone.  The choice is read when a
 * recording STARTS, so changing it mid-recording applies to the next one. */
static void app_set_audio_mode(App *app, AudioMode mode, bool announce)
{
	app->audio_mode = mode;
	const bool mic = mode == AudioMode::Microphone;

	/* The popover's two check buttons are radio-like: keep them in step with
	 * the mode however it was set (the guard stops the toggled handlers from
	 * re-entering this function). */
	if (app->audio_none || app->audio_mic) {
		app->audio_sync = true;
		if (app->audio_none)
			gtk_check_button_set_active(
				GTK_CHECK_BUTTON(app->audio_none), !mic);
		if (app->audio_mic)
			gtk_check_button_set_active(
				GTK_CHECK_BUTTON(app->audio_mic), mic);
		app->audio_sync = false;
	}

	if (app->btn_audio) {
		/* Same visual language as the AF/AE toggles: lit = on. */
		if (mic) {
			gtk_widget_add_css_class(app->btn_audio, "on");
			gtk_widget_remove_css_class(app->btn_audio, "off");
		} else {
			gtk_widget_add_css_class(app->btn_audio, "off");
			gtk_widget_remove_css_class(app->btn_audio, "on");
		}
		gtk_widget_set_tooltip_text(app->btn_audio, mic ?
			"Sound: microphone \u2014 tap to change" :
			"Sound: no audio \u2014 tap to change");
	}

	if (!announce || app->headless)
		return;
	if (app->recording)
		set_status(app, mic ? "Sound: microphone (next recording)" :
				      "Sound: off (next recording)");
	else
		set_status(app, mic ? "Video sound: microphone" :
				      "Video sound: no audio");
}

static void on_audio_none_toggled(GtkCheckButton *button, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->audio_sync || !gtk_check_button_get_active(button))
		return;
	app_set_audio_mode(app, AudioMode::None, true);
	gtk_popover_popdown(GTK_POPOVER(app->audio_popover));
}

static void on_audio_mic_toggled(GtkCheckButton *button, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->audio_sync || !gtk_check_button_get_active(button))
		return;
	app_set_audio_mode(app, AudioMode::Microphone, true);
	gtk_popover_popdown(GTK_POPOVER(app->audio_popover));
}

static void on_audio_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->audio_popover)
		gtk_popover_popup(GTK_POPOVER(app->audio_popover));
}

/* Build the note button and its two-choice popover.  It is parented to the
 * button (GTK4 attaches a popover to its parent widget) and pops UP from the
 * bottom bar. */
static GtkWidget *build_audio_button(App *app)
{
	app->btn_audio = gtk_button_new_from_icon_name(
		"audio-x-generic-symbolic");
	gtk_widget_add_css_class(app->btn_audio, "togbtn");
	gtk_widget_add_css_class(app->btn_audio, "off");
	g_signal_connect(app->btn_audio, "clicked", G_CALLBACK(on_audio_clicked),
			 app);

	app->audio_popover = gtk_popover_new();
	gtk_popover_set_position(GTK_POPOVER(app->audio_popover), GTK_POS_TOP);
	gtk_popover_set_has_arrow(GTK_POPOVER(app->audio_popover), TRUE);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 10);
	gtk_widget_set_margin_bottom(box, 10);

	app->audio_none = gtk_check_button_new_with_label("No audio");
	app->audio_mic = gtk_check_button_new_with_label("Microphone");
	gtk_check_button_set_group(GTK_CHECK_BUTTON(app->audio_mic),
				   GTK_CHECK_BUTTON(app->audio_none));
	gtk_widget_set_tooltip_text(app->audio_none,
				    "Record silent video");
	gtk_widget_set_tooltip_text(app->audio_mic,
				    "Record the internal microphones");
	g_signal_connect(app->audio_none, "toggled",
			 G_CALLBACK(on_audio_none_toggled), app);
	g_signal_connect(app->audio_mic, "toggled",
			 G_CALLBACK(on_audio_mic_toggled), app);
	gtk_box_append(GTK_BOX(box), app->audio_none);
	gtk_box_append(GTK_BOX(box), app->audio_mic);

	gtk_popover_set_child(GTK_POPOVER(app->audio_popover), box);
	gtk_widget_set_parent(app->audio_popover, app->btn_audio);
	return app->btn_audio;
}

/* --------------------------- resolution picker --------------------------- */

/* The button is in the bottom-LEFT corner (mirroring the soundtrack picker on
 * the right) and shows the size in use, so the state is visible on a touch
 * device where a tooltip is not.  Tapping it opens a radio list of the sizes
 * the CAMERA IN USE offers; switching cameras rebuilds that list.  The choice
 * is stored per camera (see save_res_choice) and applies immediately by
 * re-opening the stream — so it also becomes the size of the recorded video
 * and of a snapshot (one stream feeds all three).  While a recording runs the
 * picker is refused: a new stream would end the take. */

static const std::vector<ResOption> &res_options(const App *app)
{
	return app->rear ? kRearResolutions : kFrontResolutions;
}

static int res_index(const App *app)
{
	const auto &opts = res_options(app);
	const int idx = app->rear ? app->rear_res : app->front_res;

	if (idx < 0 || idx >= int(opts.size()))
		return default_res_index(opts);
	return idx;
}

/* "3200×2400" — what the button shows. */
static std::string res_short(const ResOption &opt)
{
	return std::to_string(opt.w) + "\u00d7" + std::to_string(opt.h);
}

/* "3200×2400 — 8 MP (4:3)" — what the menu shows. */
static std::string res_text(const ResOption &opt)
{
	return res_short(opt) + " \u2014 " + opt.label;
}

/* Show the size in use on the button and in its tooltip. */
static void res_update_button(App *app)
{
	if (!app->btn_res)
		return;
	const ResOption &opt = res_options(app)[res_index(app)];

	gtk_button_set_label(GTK_BUTTON(app->btn_res), res_short(opt).c_str());
	gtk_widget_set_tooltip_text(app->btn_res,
		("Video / preview resolution: " + res_text(opt) +
		 " \u2014 tap to change (photos too)").c_str());
}

/* A recording taps the LIVE stream, so the picker is locked (greyed) while a
 * take runs — a new size would need a new pipeline and end the recording. */
static void res_set_locked(App *app, bool locked)
{
	if (app->btn_res)
		gtk_widget_set_sensitive(app->btn_res, !locked);
}

/* Tick the entry that is in use (the option list itself does not change).
 * Guarded like slider_sync/audio_sync so the toggled handlers stay quiet. */
static void res_menu_sync(App *app)
{
	if (app->res_checks.empty())
		return;
	app->res_sync = true;
	const int cur = res_index(app);

	for (size_t i = 0; i < app->res_checks.size(); i++)
		if (app->res_checks[i])
			gtk_check_button_set_active(
				GTK_CHECK_BUTTON(app->res_checks[i]),
				int(i) == cur);
	app->res_sync = false;
}

static void app_set_resolution(App *app, int idx, bool announce);

static void on_res_toggled(GtkCheckButton *button, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->res_sync || !gtk_check_button_get_active(button))
		return;
	app_set_resolution(
		app, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
						      "res-index")),
		true);
	if (app->res_popover)
		gtk_popover_popdown(GTK_POPOVER(app->res_popover));
}

/* Build the radio list for the camera in use.  Called at startup and on every
 * camera switch — the front and the rear camera do NOT offer the same sizes,
 * so the menu must follow the camera (owner requirement). */
static void res_menu_rebuild(App *app)
{
	if (!app->res_box)
		return;
	app->res_checks.clear();
	while (GtkWidget *child = gtk_widget_get_first_child(app->res_box))
		gtk_box_remove(GTK_BOX(app->res_box), child);

	const auto &opts = res_options(app);
	GtkWidget *first = nullptr;

	for (size_t i = 0; i < opts.size(); i++) {
		const std::string label = res_text(opts[i]);
		GtkWidget *cb = gtk_check_button_new_with_label(label.c_str());

		g_object_set_data(G_OBJECT(cb), "res-index",
				  GINT_TO_POINTER(int(i)));
		if (!first) {
			first = cb;
		} else {
			gtk_check_button_set_group(GTK_CHECK_BUTTON(cb),
						   GTK_CHECK_BUTTON(first));
		}
		g_signal_connect(cb, "toggled", G_CALLBACK(on_res_toggled),
				 app);
		const std::string tip =
			(app->rear ? "Back" : "Front") +
			std::string(" camera: video, preview and photos at ") +
			label;

		gtk_widget_set_tooltip_text(cb, tip.c_str());
		gtk_box_append(GTK_BOX(app->res_box), cb);
		app->res_checks.push_back(cb);
	}

	res_menu_sync(app);
}

/* Choose a size for the camera in use: remember it, show it, and re-open the
 * stream at it (a size that cannot be negotiated falls through the other
 * entries inside preview_pipeline_start). */
static void app_set_resolution(App *app, int idx, bool announce)
{
	const auto &opts = res_options(app);

	if (idx < 0 || idx >= int(opts.size()))
		return;

	/* A recording taps the LIVE stream, so a size change would have to end
	 * the take — refuse it and put the tick back where it was. */
	if (app->recording) {
		res_menu_sync(app);
		set_status(app, "Stop the recording to change the resolution");
		return;
	}

	(app->rear ? app->rear_res : app->front_res) = idx;
	const ResOption &opt = opts[idx];

	save_res_choice(app->rear, opt);
	res_menu_sync(app);
	res_update_button(app);

	const bool changed = app->stream_w != opt.w || app->stream_h != opt.h;

	if (changed && app->camera_ok) {
		preview_pipeline_stop(app);
		gtk_widget_queue_draw(app->preview);
		if (!preview_pipeline_start(app)) {
			set_status(app, "Cannot stream at " + res_short(opt));
			return;
		}
	}
	if (announce && !app->headless)
		set_status(app, "Resolution: " + res_text(opt) +
				" (video and photos)");
}

/* The pill button: the size in use, tapping opens the radio list above it. */
static gboolean on_res_clicked(GtkButton *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->res_popover)
		gtk_popover_popup(GTK_POPOVER(app->res_popover));
	return TRUE;
}

static GtkWidget *build_res_button(App *app)
{
	app->btn_res = gtk_button_new_with_label("3200\u00d72400");
	gtk_button_set_icon_name(GTK_BUTTON(app->btn_res),
				 "video-display-symbolic");
	gtk_widget_add_css_class(app->btn_res, "resbtn");
	g_signal_connect(app->btn_res, "clicked",
			 G_CALLBACK(on_res_clicked), app);

	app->res_popover = gtk_popover_new();
	gtk_popover_set_position(GTK_POPOVER(app->res_popover), GTK_POS_TOP);
	gtk_popover_set_has_arrow(GTK_POPOVER(app->res_popover), TRUE);

	app->res_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(app->res_box, 12);
	gtk_widget_set_margin_end(app->res_box, 12);
	gtk_widget_set_margin_top(app->res_box, 10);
	gtk_widget_set_margin_bottom(app->res_box, 10);
	gtk_popover_set_child(GTK_POPOVER(app->res_popover), app->res_box);
	gtk_widget_set_parent(app->res_popover, app->btn_res);

	res_menu_rebuild(app);
	res_update_button(app);
	return app->btn_res;
}

/* ERHAI_CAM_PHOTO_PROBE="<s>,<s>,…" (GUI only): take a photo at each of those
 * seconds through the SAME path the shutter button uses and log the file AND
 * the colour of the frame that went into it (mean R/G/B + R/B) — the
 * instrument for "take a photo now, wait 5 s, take another" colour reports.
 * ERHAI_CAM_PHOTO_PROBE_SWITCH=<s> switches the camera once, so the front can
 * be probed without a finger (it does not start on a cold app).
 * Both are inert without their env var. */
static bool frame_rgb_mean(GstSample *sample, double *r, double *g, double *b)
{
	GstVideoInfo info;
	GstVideoFrame frame;
	GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : nullptr;

	if (!buffer || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
	    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA ||
	    !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
		return false;

	const int w = GST_VIDEO_INFO_WIDTH(&info);
	const int h = GST_VIDEO_INFO_HEIGHT(&info);
	const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
	const uint8_t *base = static_cast<const uint8_t *>(
		GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
	double sr = 0, sg = 0, sb = 0;
	int n = 0;

	/* stride-sample: every 16th pixel in both directions is plenty here */
	for (int y = 0; y < h; y += 16) {
		const uint8_t *row = base + size_t(y) * stride;

		for (int x = 0; x < w; x += 16) {
			const uint8_t *p = row + size_t(x) * 4;

			sb += p[0];
			sg += p[1];
			sr += p[2];
			n++;
		}
	}
	gst_video_frame_unmap(&frame);
	if (!n)
		return false;
	*r = sr / n;
	*g = sg / n;
	*b = sb / n;
	return true;
}

static gboolean photo_probe_cb(gpointer data)
{
	auto *job = static_cast<std::pair<App *, int> *>(data);
	App *app = job->first;
	const int at = job->second;
	std::string path, err;

	std::cerr << "erhai-cam: photo-probe t=" << at
		  << " camera=" << (app->rear ? "rear" : "front")
		  << " stream=" << app->stream_w << "x" << app->stream_h;

	GstSample *sample = nullptr;

	{
		std::lock_guard lock(app->frame_lock);

		if (app->last_sample)
			sample = gst_sample_ref(app->last_sample);
	}
	if (sample) {
		double r = 0, g = 0, b = 0;

		if (frame_rgb_mean(sample, &r, &g, &b))
			std::cerr << " frame R,G,B=" << int(r) << "," << int(g)
				  << "," << int(b) << " R/B="
				  << (b > 0 ? r / b : 0.0);
		gst_sample_unref(sample);
	}
	if (save_photo(app, &path, &err))
		std::cerr << " saved=" << path << "\n";
	else
		std::cerr << " failed=" << err << "\n";
	delete job;
	return G_SOURCE_REMOVE;
}

static gboolean photo_probe_switch_cb(gpointer data)
{
	auto *app = static_cast<App *>(data);

	app_switch_camera(app);
	std::cerr << "erhai-cam: photo-probe switch -> "
		  << (app->rear ? "rear" : "front") << "\n";
	return G_SOURCE_REMOVE;
}

/* ERHAI_CAM_PREVIEW_DUMP=<file.png> (GUI): render the preview WIDGET the way the
 * compositor would (GtkWidgetPaintable -> GskRenderNode -> texture -> PNG) so
 * the colours the user actually SEES can be measured; the captured frames are
 * not the same thing as the rendered preview.  Inert without the env var. */
static gboolean preview_dump_cb(gpointer data)
{
	auto *job = static_cast<std::pair<App *, std::string> *>(data);
	App *app = job->first;
	const std::string path = job->second;

	if (app->ui_dead || !app->preview) {
		std::cerr << "erhai-cam: preview-dump: no preview widget\n";
	} else {
		const int w = gtk_widget_get_width(app->preview);
		const int h = gtk_widget_get_height(app->preview);

		std::cerr << "erhai-cam: preview-dump widget=" << w << "x" << h;
		if (w > 0 && h > 0) {
			GdkPaintable *p = gtk_widget_paintable_new(app->preview);
			GtkSnapshot *snap = gtk_snapshot_new();

			gdk_paintable_snapshot(p, snap, (double)w, (double)h);
			GskRenderNode *node = gtk_snapshot_free_to_node(snap);
			GtkNative *native = gtk_widget_get_native(app->preview);
			/* the window's own renderer is realized; a fresh cairo one
			 * would need a surface and refuses to render textures */
			GskRenderer *r = native ? gtk_native_get_renderer(native) :
						 nullptr;
			GError *err = nullptr;
			GdkTexture *tex = nullptr;

			if (r && node)
				tex = gsk_renderer_render_texture(r, node, nullptr);
			if (tex) {
				if (gdk_texture_save_to_png(tex, path.c_str()))
					std::cerr << " saved=" << path;
				else
					std::cerr << " save failed";
				g_object_unref(tex);
			} else {
				std::cerr << " render failed: "
					  << (err ? err->message : "?")
					  << " (renderer=" << (r ? "native" : "none")
					  << " node=" << (node ? "yes" : "no") << ")";
			}
			if (err)
				g_error_free(err);
			if (node)
				gsk_render_node_unref(node);
			g_object_unref(p);
		}
		std::cerr << "\n";
	}
	delete job;
	return G_SOURCE_REMOVE;
}

static void setup_photo_probe(App *app)
{
	const char *spec = std::getenv("ERHAI_CAM_PHOTO_PROBE");

	if (spec) {
		const std::string times(spec);
		size_t pos = 0;

		while (pos <= times.size()) {
			const size_t comma = times.find(',', pos);

			if (const int at = std::atoi(
				    times.substr(pos, comma - pos).c_str())) {
				g_timeout_add_seconds(at, photo_probe_cb,
						      new std::pair<App *, int>(
							      app, at));
			}
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
	}
	if (const char *sw = std::getenv("ERHAI_CAM_PHOTO_PROBE_SWITCH")) {
		if (const int at = std::atoi(sw))
			g_timeout_add_seconds(at, photo_probe_switch_cb, app);
	}
	if (const char *dump = std::getenv("ERHAI_CAM_PREVIEW_DUMP")) {
		const std::string spec2(dump);
		size_t pos = 0;

		while (pos <= spec2.size()) {          /* "<file>,<file>,…" */
			const size_t comma = spec2.find(',', pos);
			const std::string file =
				spec2.substr(pos, comma - pos);
			const bool switch_cam =
				!file.empty() && file[0] == '+'; /* '+': front */

			if (!file.empty() && file[0] != '-') {
				const std::string want =
					file[0] == '+' ? file.substr(1) : file;

				/* the front does not start cold: switch first */
				if (switch_cam)
					g_timeout_add_seconds(
						2, photo_probe_switch_cb, app);
				g_timeout_add_seconds(
					switch_cam ? 9 : 6, preview_dump_cb,
					new std::pair<App *, std::string>(app,
									 want));
			}
			if (comma == std::string::npos)
				break;
			pos = comma + 1;
		}
	}
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
		if (app->sensor_ok) {
			ae.exposure = app->sensor->exposure_lines();
			ae.measured = false;
			set_status(app, "Auto exposure on");
		} else {
			set_status(app, "Auto exposure unavailable: no sensor "
					"register");
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

/* The window is being closed: the widgets below are about to be destroyed, so
 * forget them (every GTK-touching callback in this file null-checks its widget)
 * and stop the status timer from writing into a freed label. */
static gboolean on_window_close(GtkWindow *, gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->status_timer) {
		g_source_remove(app->status_timer);
		app->status_timer = 0;
	}
	app->ui_dead = true;
	app->status = nullptr;
	app->btn_switch = nullptr;
	app->btn_shot = nullptr;
	app->btn_rec = nullptr;
	app->btn_af = nullptr;
	app->btn_ae = nullptr;
	app->btn_audio = nullptr;
	app->audio_popover = nullptr;
	app->audio_none = nullptr;
	app->audio_mic = nullptr;
	app->btn_res = nullptr;
	app->res_popover = nullptr;
	app->res_box = nullptr;
	app->res_checks.clear();
	app->focus_scale = nullptr;
	app->focus_value = nullptr;
	app->exp_scale = nullptr;
	app->exp_value = nullptr;
	app->preview = nullptr;
	return FALSE;   /* let the close happen */
}

/* ERHAI_CAM_GEOM=1: after the first layout, print the window size and the two
 * manual faders' boxes MEASURED RELATIVE TO THE WINDOW, so the owner's "the
 * fader must stop kFaderBottomGap above the bottom edge" can be checked with
 * numbers instead of by eye (bottom-gap is exactly that distance).
 * ERHAI_CAM_UI_TEST=1 additionally drives the soundtrack picker through its
 * real signal path (check button -> toggled -> app_set_audio_mode) and logs
 * the resulting state, plus a popover popup/popdown, so the new UI can be
 * verified without a finger on the glass.  Both are inert without the env. */
static gboolean debug_layout_cb(gpointer ud)
{
	auto *app = static_cast<App *>(ud);

	if (app->ui_dead || !app->win)
		return G_SOURCE_REMOVE;
	const int win_w = gtk_widget_get_width(app->win);
	const int win_h = gtk_widget_get_height(app->win);

	std::cerr << "erhai-cam: layout window " << win_w << "x" << win_h << "\n";
	const struct {
		const char *name;
		GtkWidget *widget;
	} faders[] = {
		{ "exposure", app->exp_scale },
		{ "white-balance", app->wb_scale },
		{ "focus", app->focus_scale },
	};

	for (const auto &f : faders) {
		graphene_rect_t rect;

		if (!f.widget ||
		    !gtk_widget_compute_bounds(f.widget, app->win, &rect)) {
			std::cerr << "erhai-cam: layout fader-" << f.name
				  << " not laid out\n";
			continue;
		}
		std::cerr << "erhai-cam: layout fader-" << f.name
			  << " top=" << int(rect.origin.y)
			  << " height=" << int(rect.size.height)
			  << " bottom-gap=" << win_h -
			     int(rect.origin.y + rect.size.height)
			  << " (want " << kFaderBottomGap << ")\n";
	}

	if (app->btn_audio) {
		graphene_rect_t r;

		if (gtk_widget_compute_bounds(app->btn_audio, app->win, &r))
			std::cerr << "erhai-cam: layout note-button "
				  << int(r.size.width) << "x"
				  << int(r.size.height) << " right-margin="
				  << win_w - int(r.origin.x + r.size.width)
				  << " bottom-margin="
				  << win_h - int(r.origin.y + r.size.height)
				  << " togbtn="
				  << gtk_widget_has_css_class(app->btn_audio,
							      "togbtn")
				  << "\n";
	}

	if (app->btn_res) {
		graphene_rect_t r;

		if (gtk_widget_compute_bounds(app->btn_res, app->win, &r)) {
			const char *label = gtk_button_get_label(
				GTK_BUTTON(app->btn_res));

			std::cerr << "erhai-cam: layout res-button "
				  << int(r.size.width) << "x"
				  << int(r.size.height) << " left-margin="
				  << int(r.origin.x) << " bottom-margin="
				  << win_h - int(r.origin.y + r.size.height)
				  << " resbtn="
				  << gtk_widget_has_css_class(app->btn_res,
							      "resbtn")
				  << " label=" << (label ? label : "?")
				  << " entries=" << app->res_checks.size()
				  << " sensitive="
				  << gtk_widget_get_sensitive(app->btn_res)
				  << "\n";
		}
	}

	const char *test = std::getenv("ERHAI_CAM_UI_TEST");

	if (!test || std::string(test) != "1")
		return G_SOURCE_REMOVE;
	auto dump = [app](const char *when) {
		std::cerr << "erhai-cam: ui-test " << when
			  << " mode=" << (app->audio_mode ==
					  AudioMode::Microphone ? "microphone"
								: "none")
			  << " check-none=" << (app->audio_none ?
			     gtk_check_button_get_active(
				     GTK_CHECK_BUTTON(app->audio_none)) : -1)
			  << " check-mic=" << (app->audio_mic ?
			     gtk_check_button_get_active(
				     GTK_CHECK_BUTTON(app->audio_mic)) : -1)
			  << " btn-on=" << gtk_widget_has_css_class(
				     app->btn_audio, "on")
			  << " btn-off=" << gtk_widget_has_css_class(
				     app->btn_audio, "off")
			  << " icon=" << (app->btn_audio ?
			     gtk_button_get_icon_name(
				     GTK_BUTTON(app->btn_audio)) : "?")
			  << "\n";
	};

	dump("start");
	if (app->audio_popover)   /* the popup path used by a tap */
		gtk_popover_popup(GTK_POPOVER(app->audio_popover));
	if (app->audio_mic)   /* choose Microphone */
		gtk_check_button_set_active(GTK_CHECK_BUTTON(app->audio_mic),
					    TRUE);
	dump("after-mic");
	if (app->audio_none)   /* and back to No audio */
		gtk_check_button_set_active(GTK_CHECK_BUTTON(app->audio_none),
					    TRUE);
	dump("after-none");
	if (app->audio_popover)
		gtk_popover_popdown(GTK_POPOVER(app->audio_popover));

	/* The resolution picker, driven the same way (check button -> toggled
	 * -> app_set_resolution): pick another size, then switch camera and
	 * check that the menu AND the choice follow the camera, then read the
	 * stored file back the way startup does. */
	if (app->btn_res) {
		auto res_dump = [app](const char *when) {
			const char *label = gtk_button_get_label(
				GTK_BUTTON(app->btn_res));

			std::cerr << "erhai-cam: ui-test res-" << when
				  << " camera=" << (app->rear ? "rear" : "front")
				  << " choice=" << (app->rear ? app->rear_res
							      : app->front_res)
				  << " label=" << (label ? label : "?")
				  << " entries=" << app->res_checks.size()
				  << " stream=" << app->stream_w << "x"
				  << app->stream_h << "\n";
		};

		res_dump("start");
		if (app->res_popover)
			gtk_popover_popup(GTK_POPOVER(app->res_popover));
		const int want =
			(app->rear ? app->rear_res : app->front_res) == 0 ? 1 : 0;

		if (want < int(app->res_checks.size()))
			gtk_check_button_set_active(
				GTK_CHECK_BUTTON(app->res_checks[want]), TRUE);
		res_dump("after-pick");
		/* A recording taps the live stream, so the picker must lock while
		 * one runs and unlock when it is over (checked here, while the
		 * REAR camera is the one streaming). */
		if (app->appsink) {
			app_record_start(app);
			std::cerr << "erhai-cam: ui-test res-recording sensitive="
				  << gtk_widget_get_sensitive(app->btn_res)
				  << " recording=" << app->recording << "\n";
			app_record_stop(app);
			std::cerr << "erhai-cam: ui-test res-stopped sensitive="
				  << gtk_widget_get_sensitive(app->btn_res)
				  << " recording=" << app->recording << "\n";
		}
		if (app->res_popover)
			gtk_popover_popdown(GTK_POPOVER(app->res_popover));

		app_switch_camera(app);   /* the OTHER camera's menu */
		res_dump("after-switch");
		const int want2 =
			(app->rear ? app->rear_res : app->front_res) == 0 ? 1 : 0;

		if (want2 < int(app->res_checks.size()))
			gtk_check_button_set_active(
				GTK_CHECK_BUTTON(app->res_checks[want2]), TRUE);
		res_dump("after-pick2");
		if (app->res_popover)
			gtk_popover_popdown(GTK_POPOVER(app->res_popover));

		const ResOption &r = kRearResolutions[
			load_res_choice(kRearResolutions, "rear")];
		const ResOption &f = kFrontResolutions[
			load_res_choice(kFrontResolutions, "front")];

		std::cerr << "erhai-cam: ui-test res-config " << config_path()
			  << " rear=" << r.w << "x" << r.h
			  << " front=" << f.w << "x" << f.h << "\n";
	}

	/* The white-balance fader, through its real signal path (range ->
	 * value-changed -> wb_set_warmth -> the per-frame gains). */
		if (app->wb_scale) {
		auto wb_probe_dump = [app](const char *when) {
			double gr = 1.0, gb = 1.0;

			wb_gains_from_state(app, &gr, &gb);
			std::cerr << "erhai-cam: ui-test wb-" << when
				  << " active=" << app->wb_active
				  << " warmth=" << app->wb_warmth
				  << " btn-on=" << (app->btn_wb ?
				     gtk_widget_has_css_class(app->btn_wb,
							      "on") : -1)
				  << " label="
				  << (gtk_label_get_text(
					      GTK_LABEL(app->wb_value)))
				  << " gains R=" << gr << " B=" << gb
				  << " probe=" << (app->wb_probe ? 1 : 0)
				  << " slider-sensitive="
				  << gtk_widget_get_sensitive(app->wb_scale)
				  << " slider-visible="
				  << gtk_widget_get_visible(app->wb_scale)
				  << " value-visible="
				  << gtk_widget_get_visible(app->wb_value)
				  << "\n";
		};

		wb_probe_dump("start");
		if (app->btn_wb) {          /* the WB toggle */
			g_signal_emit_by_name(app->btn_wb, "clicked");
			wb_probe_dump("after-toggle");
		}
		gtk_range_set_value(GTK_RANGE(app->wb_scale), 40);
		wb_probe_dump("after-warm");
		gtk_range_set_value(GTK_RANGE(app->wb_scale), -40);
		wb_probe_dump("after-cool");
		gtk_range_set_value(GTK_RANGE(app->wb_scale), 0);
		wb_probe_dump("after-zero");
		if (app->btn_wb) {          /* and off again */
			g_signal_emit_by_name(app->btn_wb, "clicked");
			wb_probe_dump("after-off");
		}
	}
	return G_SOURCE_REMOVE;
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
		     "button.resbtn { border-radius: 9999px; min-height: 52px; "
		     "padding: 0 14px; background: rgba(0,0,0,0.6); "
		     "color: white; font-size: 0.9em; } "
		     "button.resbtn:disabled { color: #888888; } "
		     "scale.manual { min-height: 120px; min-width: 40px; } "
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

	/* ---- top-left: AE (auto exposure) + its manual fader below it ------ */
	/* The dock fills the window height and stops kFaderBottomGap above the
	 * bottom edge; the FADER is its last child, so that gap is the fader's
	 * own bottom (the value readout sits under the button instead) and the
	 * track is as long as the window allows. */
	GtkWidget *left_dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_halign(left_dock, GTK_ALIGN_START);
	gtk_widget_set_valign(left_dock, GTK_ALIGN_FILL);
	gtk_widget_set_margin_start(left_dock, 16);
	gtk_widget_set_margin_top(left_dock, 16);
	gtk_widget_set_margin_bottom(left_dock, kFaderBottomGap);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), left_dock);

	app->btn_ae = gtk_button_new_with_label("AE");
	gtk_widget_add_css_class(app->btn_ae, "togbtn");
	gtk_widget_add_css_class(app->btn_ae, "on");
	gtk_widget_set_tooltip_text(app->btn_ae,
				    "Automatic exposure (NPU). Turn it off "
				    "for the manual exposure slider.");
	g_signal_connect(app->btn_ae, "clicked", G_CALLBACK(on_ae_clicked), app);

	/* The heads: the AE toggle and the WB caption, above their faders. */
	GtkWidget *heads = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append(GTK_BOX(heads), app->btn_ae);

	/* The WB toggle next to AE (lit = the app's white balance is applied),
	 * so the fader's value can be switched on and off. */
	app->btn_wb = gtk_button_new_with_label("WB");
	gtk_widget_add_css_class(app->btn_wb, "togbtn");
	gtk_widget_add_css_class(app->btn_wb, "off");
	g_signal_connect(app->btn_wb, "clicked", G_CALLBACK(on_wb_toggled),
			 app);
	gtk_box_append(GTK_BOX(heads), app->btn_wb);
	gtk_box_append(GTK_BOX(left_dock), heads);

	/* Two faders side by side: exposure and white balance. */
	GtkWidget *faders = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_vexpand(faders, TRUE);
	gtk_box_append(GTK_BOX(left_dock), faders);

	app->exp_value = gtk_label_new(std::to_string(kExposureDefault).c_str());
	gtk_widget_add_css_class(app->exp_value, "manual");
	gtk_widget_set_halign(app->exp_value, GTK_ALIGN_CENTER);

	app->exp_scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
						  kExposureMin, kExposureMax, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->exp_scale), FALSE);
	gtk_range_set_inverted(GTK_RANGE(app->exp_scale), TRUE);
	gtk_widget_add_css_class(app->exp_scale, "manual");
	gtk_widget_set_vexpand(app->exp_scale, TRUE);
	gtk_widget_set_tooltip_text(app->exp_scale,
				    "Exposure (sensor lines): down = darker");
	g_signal_connect(app->exp_scale, "value-changed",
			 G_CALLBACK(on_exposure_changed), app);

	GtkWidget *exp_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(exp_col), app->exp_value);
	gtk_box_append(GTK_BOX(exp_col), app->exp_scale);
	gtk_box_append(GTK_BOX(faders), exp_col);

	app->wb_value = gtk_label_new("0");
	gtk_widget_add_css_class(app->wb_value, "manual");
	gtk_widget_set_halign(app->wb_value, GTK_ALIGN_CENTER);

	app->wb_scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
						 kWbWarmthMin, kWbWarmthMax, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->wb_scale), FALSE);
	gtk_range_set_inverted(GTK_RANGE(app->wb_scale), TRUE);
	gtk_widget_add_css_class(app->wb_scale, "manual");
	gtk_widget_set_vexpand(app->wb_scale, TRUE);
	gtk_widget_set_tooltip_text(app->wb_scale,
		"White balance: up = warmer (more yellow), "
		"down = cooler (more blue). 0 = untouched.");
	g_signal_connect(app->wb_scale, "value-changed",
			 G_CALLBACK(on_wb_changed), app);

	GtkWidget *wb_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(wb_col), app->wb_value);
	gtk_box_append(GTK_BOX(wb_col), app->wb_scale);
	gtk_box_append(GTK_BOX(faders), wb_col);

	/* bring the stored state into the fader/label/button WITHOUT changing it
	 * (a stored warmth with active=false must stay switched off) */
	wb_sync_ui(app);

	/* ---- top-right: AF + its manual fader below it --------------------- */
	GtkWidget *right_dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_halign(right_dock, GTK_ALIGN_END);
	gtk_widget_set_valign(right_dock, GTK_ALIGN_FILL);
	gtk_widget_set_margin_end(right_dock, 16);
	gtk_widget_set_margin_top(right_dock, 16);
	gtk_widget_set_margin_bottom(right_dock, kFaderBottomGap);
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

	app->focus_value = gtk_label_new("0");
	gtk_widget_add_css_class(app->focus_value, "manual");
	gtk_widget_set_halign(app->focus_value, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(right_dock), app->focus_value);

	app->focus_scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL,
						   kFocusMin, kFocusMax, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->focus_scale), FALSE);
	gtk_range_set_inverted(GTK_RANGE(app->focus_scale), TRUE);
	gtk_widget_add_css_class(app->focus_scale, "manual");
	gtk_widget_set_vexpand(app->focus_scale, TRUE);
	gtk_widget_set_tooltip_text(app->focus_scale,
				    "Focus (VCM): up = far, down = near");
	g_signal_connect(app->focus_scale, "value-changed",
			 G_CALLBACK(on_focus_changed), app);
	gtk_box_append(GTK_BOX(right_dock), app->focus_scale);

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

	/* ---- bottom-right: the soundtrack picker (note icon, round) --------- */
	/* Owner request 2026-10-20: "at the very right (same margin as AF button)
	 * and make it round".  Like the AF/AE toggles it is a 52 px circle with
	 * the togbtn styling — outside #bar, so those sizes apply.  The right
	 * fader clears it (kFaderBottomGap). */
	GtkWidget *audio_btn = build_audio_button(app);
	gtk_widget_set_halign(audio_btn, GTK_ALIGN_END);
	gtk_widget_set_valign(audio_btn, GTK_ALIGN_END);
	gtk_widget_set_margin_end(audio_btn, 16);       /* == AF dock margin */
	gtk_widget_set_margin_bottom(audio_btn, kNoteButtonBottomGap);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), audio_btn);
	/* Take the CLI/env choice, without a startup status line. */
	app_set_audio_mode(app, app->audio_mode, false);

	/* ---- bottom-left: the resolution picker (pill, size in use) -------- */
	/* Owner request: "a button in the left bottom corner ... tapping opens
	 * a menu to choose the resolution", per camera, and both choices must
	 * persist.  It mirrors the soundtrack picker in the opposite corner and
	 * keeps the SAME 16 px margins, so the left fader (kFaderBottomGap,
	 * i.e. 16 + 52 + 8) still clears it. */
	GtkWidget *res_btn = build_res_button(app);
	gtk_widget_set_halign(res_btn, GTK_ALIGN_START);
	gtk_widget_set_valign(res_btn, GTK_ALIGN_END);
	gtk_widget_set_margin_start(res_btn, 16);
	gtk_widget_set_margin_bottom(res_btn, kNoteButtonBottomGap);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), res_btn);

	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), app);
	gtk_widget_add_controller(app->win, keys);

	/* Closing the window destroys every widget below while the streaming
	 * thread and the GStreamer bus may still call back — drop the pointers
	 * (all those callbacks null-check them) and stop the UI work. */
	g_signal_connect(app->win, "close-request",
			 G_CALLBACK(on_window_close), app);

	setup_orientation_watch(app);
	setup_display_watch(app);
	preview_pipeline_start(app);
	gtk_window_present(GTK_WINDOW(app->win));
	if (std::getenv("ERHAI_CAM_GEOM"))
		g_timeout_add(1500, debug_layout_cb, app);
	setup_photo_probe(app);
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
		  << " tag=shutter-ui-14-lowlight\n";
	/* A dead analyzer helper must not kill the app with SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);
	std::string camera_override;
	std::string res_override;
	bool use_gui = true;
	/* argv pass-through for g_application_run(): GApplication rejects
	 * unknown LOCAL options ("Unknown option --camera" -> silent exit
	 * before activate), so only forward argv[0]. */
	std::vector<char *> pass_argv{ argv[0] };

	for (int i = 1; i < argc; i++) {
		const std::string arg(argv[i]);
		/* Every flag ADDS to the App: the modes below used to build a fresh
		 * one, which silently dropped whatever an earlier flag had set
		 * (e.g. --no-af --record 5 lost --no-af).  Flag order must not
		 * matter (2026-10-20: --audio mic --record N needs the same). */
		if (arg == "--shot") {
			if (!g_app)
				g_app = new App();
			g_app->shot = true;
			g_app->headless = true;
			use_gui = false;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				g_app->out_path = argv[++i];
		} else if (arg == "--record") {
			if (!g_app)
				g_app = new App();
			g_app->record_seconds = (i + 1 < argc) ? std::atoi(argv[++i]) : 5;
			g_app->headless = true;
			use_gui = false;
		} else if (arg == "--once") {
			if (!g_app)
				g_app = new App();
			g_app->once = true;
			g_app->headless = true;
			use_gui = false;
		} else if (arg == "--ae-probe") {
			/* headless AE soak: log every controller update */
			if (!g_app)
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
		} else if (arg == "--res" && i + 1 < argc) {
			/* Force the stream/recording size for the camera in use,
			 * e.g. --res 1920x1080 (scripts and headless runs). */
			res_override = argv[++i];
		} else if (arg == "--audio" && i + 1 < argc) {
			/* video soundtrack: "none" (silent, the GUI default) or
			 * "mic" (the PipeWire default source) */
			if (!g_app)
				g_app = new App();
			g_app->audio_mode = std::string(argv[++i]) == "mic" ?
				AudioMode::Microphone : AudioMode::None;
		} else if (arg == "-h" || arg == "--help") {
			std::cout << "Usage: erhai-cam [--shot [PATH]] [--record SECONDS] "
				     "[--audio none|mic] "
				     "[--res WxH] "
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

	/* The two cameras' remembered resolutions (owner requirement: both must
	 * survive a restart).  Loaded for BOTH cameras whatever we start on. */
	app->rear_res = load_res_choice(kRearResolutions, "rear");
	app->front_res = load_res_choice(kFrontResolutions, "front");

	/* the stored warm/cool white-balance value (the WB fader) */
	load_wb_config(app);

	/* --res / ERHAI_CAM_RES: force the size for the camera in use, without
	 * storing it (a scripted/headless override, e.g. --res 1280x720). */
	if (res_override.empty()) {
		if (const char *env = std::getenv("ERHAI_CAM_RES"))
			res_override = env;
	}
	if (!res_override.empty()) {
		const auto &opts = app->rear ? kRearResolutions
					     : kFrontResolutions;
		const auto parsed = parse_res(res_override);

		if (!parsed) {
			std::cerr << "erhai-cam: cannot parse --res "
				  << res_override << "\n";
		} else if (const int i = res_index_of(opts, parsed->first,
						      parsed->second);
			   i < 0) {
			std::cerr << "erhai-cam: " << res_override
				  << " is not offered for the "
				  << (app->rear ? "rear" : "front")
				  << " camera\n";
		} else {
			(app->rear ? app->rear_res : app->front_res) = i;
			std::cout << "Resolution forced: " << res_override
				  << "\n";
		}
	}

	/* Video soundtrack default from the environment (ERHAI_CAM_AUDIO=mic);
	 * --audio wins over it, and the popover wins over both. */
	if (const char *env = std::getenv("ERHAI_CAM_AUDIO")) {
		if (std::string(env) == "mic" || std::string(env) == "microphone")
			app->audio_mode = AudioMode::Microphone;
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

	/* Sensor: the exposure register of the camera we START on (2.8 owns the
	 * front sc820cs as well; a switch re-attaches, see app_switch_camera). */
	sensor_attach(app);

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
	/* A recording still in flight (the window was closed / --record without a
	 * loop) is finalized before the process goes away: EOS both branches and
	 * let the muxer write its index, or the MP4 is left without a moov. */
	record_finish(app, 5000);
	preview_pipeline_stop(app);
	return 0;
}

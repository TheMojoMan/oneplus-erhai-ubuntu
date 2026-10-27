// SPDX-License-Identifier: GPL-2.0-only

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <gtk/gtk.h>

#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr char kRearCamera[] =
	"/base/soc@0/cci@ac7b000/i2c-bus@1/camera-sensor@36";
constexpr int kFocusMin = 0;
constexpr int kFocusMax = 1023;
constexpr int kSettleFrames = 3;
constexpr int kScoreFrames = 4;
constexpr auto kTapFocusHold = std::chrono::seconds(5);

volatile std::sig_atomic_t stop_requested;

void signal_handler(int)
{
	stop_requested = true;
}

std::string read_text_file(const std::filesystem::path &path)
{
	std::ifstream input(path);
	std::string value;

	std::getline(input, value);
	return value;
}

std::optional<std::string> find_lens_device()
{
	const std::filesystem::path video_class("/sys/class/video4linux");

	for (const auto &entry : std::filesystem::directory_iterator(video_class)) {
		const std::string node = entry.path().filename();

		if (!node.starts_with("v4l-subdev"))
			continue;
		if (read_text_file(entry.path() / "name") == "cn3927 focus")
			return "/dev/" + node;
	}

	return std::nullopt;
}

class Lens {
public:
	explicit Lens(const std::string &path) : path_(path)
	{
		fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd_ < 0)
			throw std::runtime_error("cannot open " + path + ": " +
						 std::strerror(errno));

		v4l2_control control = {
			.id = V4L2_CID_FOCUS_ABSOLUTE,
			.value = 0,
		};
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

	void set_focus(int position)
	{
		v4l2_control control = {
			.id = V4L2_CID_FOCUS_ABSOLUTE,
			.value = std::clamp(position, kFocusMin, kFocusMax),
		};

		if (ioctl(fd_, VIDIOC_S_CTRL, &control) < 0)
			throw std::runtime_error("cannot set focus on " + path_ + ": " +
						 std::strerror(errno));
		position_ = control.value;
	}

	int position() const { return position_; }

private:
	std::string path_;
	int fd_ = -1;
	int position_ = 0;
};

struct FocusPoint {
	double x;
	double y;
};

class CameraFrames {
public:
	CameraFrames(const std::string &camera, bool preview)
	{
		std::string pipeline_description =
			"libcamerasrc camera-name=\"" + camera +
			"\" ! video/x-raw,width=640,height=480 ! ";

		if (preview) {
			pipeline_description +=
				"tee name=t "
				"t. ! queue leaky=downstream max-size-buffers=1 ! "
				"videoconvert ! video/x-raw,format=GRAY8 ! "
				"appsink name=focus_frames max-buffers=1 drop=true sync=false "
				"t. ! queue leaky=downstream max-size-buffers=1 ! "
				"videoconvert ! gtk4paintablesink name=preview_sink sync=false";
		} else {
			pipeline_description +=
				"videoconvert ! video/x-raw,format=GRAY8 ! "
				"appsink name=focus_frames max-buffers=1 drop=true sync=false";
		}

		GError *error = nullptr;
		pipeline_ = gst_parse_launch(pipeline_description.c_str(), &error);
		if (error || !pipeline_) {
			std::string message = error ? error->message : "unknown error";

			if (pipeline_)
				gst_object_unref(pipeline_);
			pipeline_ = nullptr;
			g_clear_error(&error);
			throw std::runtime_error("cannot create camera pipeline: " + message);
		}

		GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline_), "focus_frames");
		if (!sink)
			throw std::runtime_error("camera pipeline has no focus appsink");
		appsink_ = GST_APP_SINK(sink);

		if (preview) {
			GstElement *preview_sink =
				gst_bin_get_by_name(GST_BIN(pipeline_), "preview_sink");
			GdkPaintable *paintable = nullptr;

			if (!preview_sink)
				throw std::runtime_error("camera pipeline has no preview sink");
			g_object_get(preview_sink, "paintable", &paintable, nullptr);
			gst_object_unref(preview_sink);
			if (!paintable)
				throw std::runtime_error("GTK preview sink has no paintable");

			window_ = GTK_WINDOW(gtk_window_new());
			g_object_add_weak_pointer(
				G_OBJECT(window_), reinterpret_cast<gpointer *>(&window_));
			gtk_window_set_title(window_, "Erhai tap autofocus");
			gtk_window_set_default_size(window_, 800, 600);
			picture_ = gtk_picture_new_for_paintable(paintable);
			g_object_unref(paintable);
			gtk_picture_set_content_fit(GTK_PICTURE(picture_),
						    GTK_CONTENT_FIT_CONTAIN);
			gtk_window_set_child(window_, picture_);

			GtkGesture *gesture = gtk_gesture_click_new();
			gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
			g_signal_connect(gesture, "pressed", G_CALLBACK(click_pressed), this);
			gtk_widget_add_controller(picture_, GTK_EVENT_CONTROLLER(gesture));
			g_signal_connect(window_, "close-request",
					 G_CALLBACK(window_close_requested), this);
			gtk_window_present(window_);
		}

		if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
		    GST_STATE_CHANGE_FAILURE)
			throw std::runtime_error("cannot start the rear camera");
	}

	~CameraFrames()
	{
		if (pipeline_)
			gst_element_set_state(pipeline_, GST_STATE_NULL);
		if (window_)
			gtk_window_destroy(window_);
		if (appsink_)
			gst_object_unref(appsink_);
		if (pipeline_)
			gst_object_unref(pipeline_);
	}

	CameraFrames(const CameraFrames &) = delete;
	CameraFrames &operator=(const CameraFrames &) = delete;

	void set_focus_point(FocusPoint point)
	{
		focus_point_ = point;
	}

	std::optional<FocusPoint> take_focus_request()
	{
		std::lock_guard lock(pending_focus_mutex_);
		std::optional<FocusPoint> point = pending_focus_point_;

		pending_focus_point_.reset();
		return point;
	}

	double next_score()
	{
		while (g_main_context_iteration(nullptr, false))
			;
		GstSample *sample = gst_app_sink_try_pull_sample(appsink_, 3 * GST_SECOND);
		if (!sample)
			throw_pipeline_error();
		while (g_main_context_iteration(nullptr, false))
			;

		GstCaps *caps = gst_sample_get_caps(sample);
		GstBuffer *buffer = gst_sample_get_buffer(sample);
		GstVideoInfo info;
		GstVideoFrame frame;

		if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
		    !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
			gst_sample_unref(sample);
			throw std::runtime_error("cannot map camera frame");
		}

		const auto *data = static_cast<const uint8_t *>(
			GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
		const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
		const int width = GST_VIDEO_FRAME_WIDTH(&frame);
		const int height = GST_VIDEO_FRAME_HEIGHT(&frame);
		int x_start;
		int x_end;
		int y_start;
		int y_end;

		if (focus_point_) {
			const int region_width = width * 3 / 10;
			const int region_height = height * 3 / 10;
			const int center_x = std::clamp(int(std::lround(focus_point_->x)),
						      0, width - 1);
			const int center_y = std::clamp(int(std::lround(focus_point_->y)),
						      0, height - 1);

			x_start = std::clamp(center_x - region_width / 2,
					     1, width - region_width - 1);
			y_start = std::clamp(center_y - region_height / 2,
					     1, height - region_height - 1);
			x_end = x_start + region_width;
			y_end = y_start + region_height;
		} else {
			x_start = width / 5;
			x_end = width - x_start;
			y_start = height / 5;
			y_end = height - y_start;
		}
		double sum = 0.0;
		std::uint64_t samples = 0;

		/* Tenengrad focus measure over the central 60% of the image. */
		for (int y = y_start + 1; y < y_end - 1; y += 2) {
			const uint8_t *previous = data + (y - 1) * stride;
			const uint8_t *current = data + y * stride;
			const uint8_t *next = data + (y + 1) * stride;

			for (int x = x_start + 1; x < x_end - 1; x += 2) {
				const int gx = int(current[x + 1]) - int(current[x - 1]);
				const int gy = int(next[x]) - int(previous[x]);

				sum += double(gx * gx + gy * gy);
				samples++;
			}
		}

		gst_video_frame_unmap(&frame);
		gst_sample_unref(sample);
		return samples ? sum / samples : 0.0;
	}

	void discard(int count)
	{
		for (int i = 0; i < count && !stop_requested; i++)
			(void)next_score();
	}

private:
	static void click_pressed(GtkGestureClick *, gint, gdouble x, gdouble y,
				  gpointer user_data)
	{
		auto *camera = static_cast<CameraFrames *>(user_data);
		const int widget_width = gtk_widget_get_width(camera->picture_);
		const int widget_height = gtk_widget_get_height(camera->picture_);
		const double scale = std::min(double(widget_width) / 640.0,
					      double(widget_height) / 480.0);
		const double image_width = 640.0 * scale;
		const double image_height = 480.0 * scale;
		const double offset_x = (widget_width - image_width) / 2.0;
		const double offset_y = (widget_height - image_height) / 2.0;

		if (scale <= 0.0 || x < offset_x || y < offset_y ||
		    x >= offset_x + image_width || y >= offset_y + image_height)
			return;

		std::lock_guard lock(camera->pending_focus_mutex_);
		camera->pending_focus_point_ = {
			(x - offset_x) / scale,
			(y - offset_y) / scale,
		};
	}

	static gboolean window_close_requested(GtkWindow *, gpointer)
	{
		stop_requested = true;
		return false;
	}

	[[noreturn]] void throw_pipeline_error()
	{
		GstBus *bus = gst_element_get_bus(pipeline_);
		GstMessage *message = gst_bus_pop_filtered(
			bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR |
							 GST_MESSAGE_EOS));
		std::string detail = "timed out waiting for a camera frame";

		if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
			GError *error = nullptr;
			gchar *debug = nullptr;

			gst_message_parse_error(message, &error, &debug);
			if (error)
				detail = error->message;
			g_clear_error(&error);
			g_free(debug);
		}

		if (message)
			gst_message_unref(message);
		gst_object_unref(bus);
		throw std::runtime_error("camera pipeline failed: " + detail);
	}

	GstElement *pipeline_ = nullptr;
	GstAppSink *appsink_ = nullptr;
	GtkWindow *window_ = nullptr;
	GtkWidget *picture_ = nullptr;
	std::optional<FocusPoint> focus_point_;
	std::mutex pending_focus_mutex_;
	std::optional<FocusPoint> pending_focus_point_;
};

double median_score(CameraFrames &camera, int count)
{
	std::vector<double> scores;

	for (int i = 0; i < count && !stop_requested; i++)
		scores.push_back(camera.next_score());
	if (scores.empty())
		return 0.0;

	std::sort(scores.begin(), scores.end());
	return scores[scores.size() / 2];
}

struct FocusResult {
	int position;
	double score;
	double minimum;
};

FocusResult scan_positions(Lens &lens, CameraFrames &camera,
			   const std::vector<int> &positions, const char *label)
{
	FocusResult best = { positions.front(), -1.0,
			     std::numeric_limits<double>::infinity() };

	std::cout << label << " scan:\n";
	for (int position : positions) {
		if (stop_requested)
			break;

		lens.set_focus(position);
		camera.discard(kSettleFrames);
		const double score = median_score(camera, kScoreFrames);
		std::cout << "  focus=" << position << " score=" << std::llround(score)
			  << '\n';
		best.minimum = std::min(best.minimum, score);
		if (score > best.score)
			best = { position, score, best.minimum };
	}

	return best;
}

FocusResult autofocus(Lens &lens, CameraFrames &camera)
{
	const std::vector<int> coarse = { 0, 128, 256, 384, 512,
					 640, 768, 896, 1023 };
	const int previous_position = lens.position();
	FocusResult best = scan_positions(lens, camera, coarse, "Coarse");
	std::set<int> fine_set;
	if (best.score - best.minimum < 8.0 || best.score < best.minimum * 1.15) {
		lens.set_focus(previous_position);
		camera.discard(kSettleFrames + 3);
		best.position = previous_position;
		best.score = median_score(camera, 9);
		std::cout << "No reliable detail in focus region; keeping focus="
			  << best.position << " score=" << std::llround(best.score)
			  << "\n";
		return best;
	}

	for (int offset = -128; offset <= 128; offset += 32)
		fine_set.insert(std::clamp(best.position + offset,
					   kFocusMin, kFocusMax));
	const std::vector<int> fine(fine_set.begin(), fine_set.end());
	FocusResult refined = scan_positions(lens, camera, fine, "Fine");

	if (refined.score > best.score)
		best = refined;
	lens.set_focus(best.position);
	camera.discard(kSettleFrames + 3);
	best.score = median_score(camera, 9);
	std::cout << "Locked focus=" << best.position
		  << " score=" << std::llround(best.score) << "\n";
	return best;
}

std::optional<FocusPoint> parse_focus_point(const char *value)
{
	FocusPoint point;
	char trailing;

	if (std::sscanf(value, "%lf,%lf%c", &point.x, &point.y, &trailing) != 2 ||
	    point.x < 0.0 || point.y < 0.0)
		return std::nullopt;
	return point;
}

void usage(const char *program)
{
	std::cout << "Usage: " << program << " [--once] [--no-preview] "
		     "[--point X,Y] [--camera NAME] [--lens DEVICE]\n"
		     "\n"
		     "Default: continuous autofocus with a preview window.\n"
		     "  --once       Focus once and exit.\n"
		     "  --no-preview Do not create a preview window.\n"
		     "  --point X,Y  Focus a 30% region around an image coordinate.\n";
}

} // namespace

int main(int argc, char **argv)
{
	bool continuous = true;
	bool preview = true;
	std::string camera_name = kRearCamera;
	std::optional<std::string> lens_path;
	std::optional<FocusPoint> initial_focus_point;

	for (int i = 1; i < argc; i++) {
		const std::string argument(argv[i]);

		if (argument == "--once") {
			continuous = false;
			preview = false;
		} else if (argument == "--no-preview") {
			preview = false;
		} else if (argument == "--point" && i + 1 < argc) {
			initial_focus_point = parse_focus_point(argv[++i]);
			if (!initial_focus_point) {
				std::cerr << "Invalid focus point; use X,Y image coordinates.\n";
				return 2;
			}
		} else if (argument == "--camera" && i + 1 < argc) {
			camera_name = argv[++i];
		} else if (argument == "--lens" && i + 1 < argc) {
			lens_path = argv[++i];
		} else if (argument == "-h" || argument == "--help") {
			usage(argv[0]);
			return 0;
		} else {
			std::cerr << "Unknown or incomplete option: " << argument << '\n';
			usage(argv[0]);
			return 2;
		}
	}

	try {
		gst_init(&argc, &argv);
		if (preview)
			gtk_init();
		std::signal(SIGINT, signal_handler);
		std::signal(SIGTERM, signal_handler);

		if (!lens_path)
			lens_path = find_lens_device();
		if (!lens_path)
			throw std::runtime_error("CN3927 lens subdevice was not found");

		std::cout << "Rear camera: " << camera_name << '\n'
			  << "Lens device: " << *lens_path << '\n';
		Lens lens(*lens_path);
		CameraFrames frames(camera_name, preview);
		if (initial_focus_point) {
			frames.set_focus_point(*initial_focus_point);
			std::cout << "Focus region centered at "
				  << std::lround(initial_focus_point->x) << ','
				  << std::lround(initial_focus_point->y) << "\n";
		}

		/* Let automatic exposure settle before comparing focus positions. */
		frames.discard(15);
		FocusResult locked = autofocus(lens, frames);
		if (!continuous || stop_requested)
			return 0;

		std::cout << "Continuous autofocus active; press Ctrl+C to stop.\n";
		int low_score_checks = 0;
		auto tap_focus_hold_until = initial_focus_point
			? std::chrono::steady_clock::now() + kTapFocusHold
			: std::chrono::steady_clock::time_point::min();
		while (!stop_requested) {
			if (std::optional<FocusPoint> point = frames.take_focus_request()) {
				frames.set_focus_point(*point);
				std::cout << "Tap focus at " << std::lround(point->x)
					  << ',' << std::lround(point->y) << "\n";
				locked = autofocus(lens, frames);
				tap_focus_hold_until =
					std::chrono::steady_clock::now() + kTapFocusHold;
				std::cout << "Tap focus held for "
					  << kTapFocusHold.count() << " seconds.\n";
				low_score_checks = 0;
				continue;
			}

			frames.discard(12);
			if (std::chrono::steady_clock::now() < tap_focus_hold_until)
				continue;
			const double score = median_score(frames, 5);

			if (score < locked.score * 0.55)
				low_score_checks++;
			else {
				low_score_checks = 0;
				locked.score = locked.score * 0.95 + score * 0.05;
			}

			if (low_score_checks >= 4) {
				std::cout << "Sharpness changed; refocusing.\n";
				locked = autofocus(lens, frames);
				low_score_checks = 0;
			}
		}
	} catch (const std::exception &error) {
		std::cerr << "Error: " << error.what() << '\n';
		return 1;
	}

	return 0;
}

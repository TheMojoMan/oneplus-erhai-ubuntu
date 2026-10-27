CC ?= gcc
CFLAGS ?= -O2
# VA driver callbacks intentionally leave some ABI parameters unused.
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += $(shell pkg-config --cflags libva 2>/dev/null)
LDLIBS += -lva -ldl -pthread

ifneq ($(shell pkg-config --exists vulkan 2>/dev/null && echo yes),)
CPPFLAGS += -DIRIS_HAVE_VULKAN $(shell pkg-config --cflags vulkan)
LDLIBS += $(shell pkg-config --libs vulkan)
endif

BUILD = build
DRIVER = $(BUILD)/iris_drv_video.so
V4L2_OBJ = $(BUILD)/v4l2_dec.o
H264_OBJ = $(BUILD)/h264_params.o
HEVC_OBJ = $(BUILD)/hevc_params.o
HEVC_REWRITE_OBJ = $(BUILD)/hevc_slice_rewrite.o
VK_COPY_OBJ = $(BUILD)/vk_copy.o
TEST_VA = $(BUILD)/test_va
TEST_V4L2 = $(BUILD)/test_v4l2_dec
TEST_H264 = $(BUILD)/test_h264_params
TEST_VADEC = $(BUILD)/test_va_decode
TEST_VP9 = $(BUILD)/test_va_vp9
TEST_V4L2_VP9 = $(BUILD)/test_v4l2_vp9
TEST_HEVC = $(BUILD)/test_hevc_au
TEST_HEVC_PARAMS = $(BUILD)/test_hevc_params
TEST_HEVC_REWRITE = $(BUILD)/test_hevc_slice_rewrite
TEST_VA_STRESS = $(BUILD)/test_va_stress
TEST_SURFACE_FENCE = $(BUILD)/test_surface_fence

.PHONY: all check clean install uninstall

DRIVERDIR ?= $(shell pkg-config --variable=driverdir libva 2>/dev/null)
DESTDIR ?=

all: $(DRIVER) $(TEST_VA) $(TEST_V4L2) $(TEST_H264) $(TEST_VADEC) \
	$(TEST_VP9) $(TEST_V4L2_VP9) $(TEST_HEVC) $(TEST_HEVC_PARAMS) $(TEST_HEVC_REWRITE) \
	$(TEST_VA_STRESS) $(TEST_SURFACE_FENCE)

DECODE_OBJ = $(BUILD)/decode.o

$(DRIVER): src/iris_vaapi.c $(BUILD)/decode.o $(BUILD)/v4l2_dec.o $(BUILD)/h264_params.o $(BUILD)/hevc_params.o $(HEVC_REWRITE_OBJ) $(VK_COPY_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC -shared $(CPPFLAGS) -Isrc -o $@ $< $(DECODE_OBJ) $(V4L2_OBJ) $(H264_OBJ) $(HEVC_OBJ) $(HEVC_REWRITE_OBJ) $(VK_COPY_OBJ) $(LDFLAGS) $(LDLIBS)

$(DECODE_OBJ): src/decode.c src/decode.h src/v4l2_dec.h src/h264_params.h src/hevc_params.h src/hevc_slice_rewrite.h src/vk_copy.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g $(CPPFLAGS) -Isrc -c -o $@ src/decode.c

$(VK_COPY_OBJ): src/vk_copy.c src/vk_copy.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/vk_copy.c

$(HEVC_OBJ): src/hevc_params.c src/hevc_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(HEVC_REWRITE_OBJ): src/hevc_slice_rewrite.c src/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(V4L2_OBJ): src/v4l2_dec.c src/v4l2_dec.h src/iris_surface_fence.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_VA): test/test_va.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_V4L2): test/test_v4l2_dec.c $(V4L2_OBJ) src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(V4L2_OBJ)

$(H264_OBJ): src/h264_params.c src/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_H264): test/test_h264_params.c $(H264_OBJ) src/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(H264_OBJ)

$(TEST_VADEC): test/test_va_decode.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_VP9): test/test_va_vp9.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_V4L2_VP9): test/test_v4l2_vp9.c $(V4L2_OBJ) src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(V4L2_OBJ)

$(TEST_HEVC): test/test_hevc_au.c $(V4L2_OBJ) src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(V4L2_OBJ)

$(TEST_HEVC_PARAMS): test/test_hevc_params.c $(HEVC_OBJ) src/hevc_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(HEVC_OBJ)

$(TEST_HEVC_REWRITE): test/test_hevc_slice_rewrite.c $(HEVC_REWRITE_OBJ) src/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(HEVC_REWRITE_OBJ)

$(TEST_VA_STRESS): test/test_va_stress.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_SURFACE_FENCE): test/test_surface_fence.c src/iris_surface_fence.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $<

clean:
	rm -rf $(BUILD)

check: $(TEST_H264) $(TEST_HEVC_PARAMS) $(TEST_HEVC_REWRITE)
	./$(TEST_H264)
	./$(TEST_HEVC_PARAMS)
	./$(TEST_HEVC_REWRITE)

install: $(DRIVER)
	test -n "$(DRIVERDIR)"
	install -d "$(DESTDIR)$(DRIVERDIR)"
	install -m 0755 $(DRIVER) "$(DESTDIR)$(DRIVERDIR)/iris_drv_video.so"

uninstall:
	test -n "$(DRIVERDIR)"
	rm -f "$(DESTDIR)$(DRIVERDIR)/iris_drv_video.so"

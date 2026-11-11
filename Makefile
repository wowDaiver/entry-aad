# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# entry-aad: wireless Android Auto for EntryNav2 (EntryEVO, Linux ARMv7 hard-float, glibc 2.15).
#
# ARM targets build in the docker image entrynav2-tc:4.7 (GCC 4.7.2 armhf) through toolchain/tc.sh, which mounts the
# project on /work and a copy of the car's system on /sysroot (read-only). From the repository root:
#   bash toolchain/tc.sh make <target>
#
#   all            what the car runs: entry-aad, aa-supervisor and hmiapp/libEntryAad.so (ARM)
#   dev-tools      evo-check and evo-capture, read-only diagnostics run by hand on the car (ARM, not installed)
#   test-arm-all   every ARM program tests/run_qemu.py runs (ARM)
#   test-host      the tests that run on the PC itself (host compiler, libdbus-1-dev, dbus, protoc, python3-protobuf)
#   check          what CI runs without a copy of the car's system: style, verify_core, verify_repo, test-host
#   clean          every build output

CXX       = arm-linux-gnueabihf-g++
SYSROOT  ?= /sysroot
ARCH      = -march=armv7-a -mfpu=neon -mfloat-abi=hard
CXXFLAGS ?= -std=gnu++0x -O2 -Wall -Wextra $(ARCH)
CPPFLAGS += -Iplatform -Icore -Icore/include
HDR_ROOT ?= /opt/hdr-armhf/usr
CPPFLAGS += -I$(HDR_ROOT)/include -I$(HDR_ROOT)/include/arm-linux-gnueabihf
CPPFLAGS += -I$(HDR_ROOT)/include/dbus-1.0 -I$(HDR_ROOT)/lib/arm-linux-gnueabihf/dbus-1.0/include

# HU libraries: sysroot paths, plus resolution of transitive dependencies at link time.
LIBDIRS   = -L$(SYSROOT)/usr/lib -L$(SYSROOT)/lib -L$(SYSROOT)/usr/local/lib
RPATHLINK = -Wl,-rpath-link,$(SYSROOT)/usr/lib:$(SYSROOT)/lib:$(SYSROOT)/usr/local/lib
LDSHLIB   = -Wl,--allow-shlib-undefined $(LIBDIRS) $(RPATHLINK)

PLATFORM_SRC = platform/omap_dmabuf.cpp platform/evo_ilm.cpp platform/evo_video.cpp platform/evo_process.cpp
PLATFORM_OBJ = $(PLATFORM_SRC:.cpp=.o)
CORE_SRC = core/aa_session.cpp core/aa_channels.cpp core/aa_messages.cpp core/car_inbox.cpp core/aa_frame.cpp \
           core/aa_tls.cpp core/tcp_server.cpp core/sdr_builder.cpp core/wireless_setup.cpp core/crash_handler.cpp
CORE_OBJ = $(CORE_SRC:.cpp=.o)
# The cluster guidance (platform/kombi_*), in the service and in its tests.
KOMBI_SRC = platform/kombi_phone.cpp platform/kombi_display.cpp platform/kombi_someip.cpp platform/kombi_graph.cpp \
            platform/kombi_client.cpp platform/kombi_guide.cpp platform/kombi_nav.cpp
KOMBI_OBJ = $(KOMBI_SRC:.cpp=.o)
# Android Auto over a USB cable: the kernel's usbfs, the accessory switch, the bridge to the session, the loop.
USB_SRC = platform/usb_devfs.cpp platform/usb_accessory.cpp platform/usb_bridge.cpp platform/usb_loop.cpp
USB_OBJ = $(USB_SRC:.cpp=.o)
DAEMON_SRC = platform/alps_spp.cpp platform/alps_spp_codec.cpp platform/evo_ap.cpp \
             platform/evo_ap_config.cpp platform/evo_spp_snapshot.cpp platform/evo_touch.cpp platform/evo_knob.cpp \
             platform/vmost_client.cpp platform/evo_control.cpp platform/evo_sdr_sensors.cpp platform/evo_mic.cpp \
             $(KOMBI_SRC) platform/evo_bmw_guidance.cpp platform/evo_pill.cpp \
             platform/peek_timing.cpp platform/wireless_loop.cpp platform/service_app.cpp platform/main.cpp \
             platform/wifi_link.cpp $(USB_SRC)
DAEMON_OBJ = $(CORE_OBJ) $(DAEMON_SRC:.cpp=.o)
SUPERVISOR_OBJ = tools/aa_supervisor.o platform/evo_spp_snapshot.o platform/alps_spp.o platform/alps_spp_codec.o \
            platform/evo_ilm.o platform/evo_process.o platform/evo_ap.o platform/evo_ap_config.o platform/evo_wlan.o \
            platform/vmost_client.o platform/usb_accessory.o
CHECK_OBJ = tools/evo_check.o platform/alps_spp.o platform/alps_spp_codec.o platform/evo_process.o
CAPTURE_OBJ = tools/evo_capture.o
ARM_OBJ = $(sort $(DAEMON_OBJ) $(PLATFORM_OBJ) $(SUPERVISOR_OBJ) $(CHECK_OBJ) $(CAPTURE_OBJ))
# The service's modules tests/test_wireless_lifecycle.cpp links around its fake Alps link and handshake.
LIFECYCLE_OBJ = $(PLATFORM_OBJ) platform/evo_touch.o platform/evo_knob.o platform/vmost_client.o platform/evo_control.o \
                platform/evo_mic.o platform/evo_pill.o $(KOMBI_OBJ) platform/evo_bmw_guidance.o platform/peek_timing.o \
                platform/wifi_link.o $(USB_OBJ)

VIDEO_LIBS = -l:libxsvideo.so.4 -l:libdrm_omap.so.1 -l:libdrm.so.2 -lpthread -lrt

.PHONY: all daemon hmiapp dev-tools check style clean verify_core verify_binary verify_wireless verify_repo \
        test-host test-parsers test-spp-host test-kombi-host test-gps-host test-capture-host test-touch-live \
        tests/evo-check-host test-arm-all test-pb-arm-build test-sdr-arm-build test-kombi-arm-build \
        test-session-silence-arm-build test-presentation-arm-build test-wireless-lifecycle-arm-build \
        test-gps-arm-build test-supervisor-arm-build test-hmiapp-arm-build test-spawn-arm-build test-lm-cli-arm-build \
        test-phone-arm-build test-usb-host test-usb-arm-build test-wifi-arm-build
all: daemon aa-supervisor hmiapp

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

# The service announces the project's version to the phone as the head unit's software version.
platform/main.o: CPPFLAGS += -DENTRY_AAD_VERSION='"$(strip $(shell cat VERSION))"'
platform/main.o: VERSION

daemon: entry-aad
entry-aad: $(DAEMON_OBJ) $(PLATFORM_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDSHLIB) $(VIDEO_LIBS) \
	    -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libdbus-1.so.3 -ldl

aa-supervisor: $(SUPERVISOR_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDSHLIB) -l:libdbus-1.so.3 -lpthread -lrt -ldl

# iDrive PluginApp (installed in /opt/hmi/data/ro/Plugins/EntryAad, loaded by HmiMain3D at boot). Its objects are
# position independent and hidden: HmiMain3D only looks up pluginAppInfo, the one symbol the library exports.
HMI_SRC = hmiapp/entry_aa_app.cpp hmiapp/bmw_framework.cpp hmiapp/app_log.cpp hmiapp/app_guards.cpp \
          hmiapp/service_link.cpp hmiapp/car_signals.cpp hmiapp/hold_machine.cpp
HMI_OBJ = $(HMI_SRC:.cpp=.o)
HMI_LIBDIRS = -L$(SYSROOT)/opt/hmi/lib -Wl,-rpath-link,$(SYSROOT)/opt/hmi/lib
hmiapp: hmiapp/libEntryAad.so
$(HMI_OBJ): hmiapp/%.o: hmiapp/%.cpp
	$(CXX) $(CXXFLAGS) -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -MMD -MP -c $< -o $@
hmiapp/libEntryAad.so: $(HMI_OBJ)
	$(CXX) $(CXXFLAGS) -fPIC -shared -Wl,-soname,libEntryAad.so $^ -o $@ \
	    $(LDSHLIB) $(HMI_LIBDIRS) -l:libAppFramework.A.so -l:libCoreFramework.A.so -lpthread -ldl

# Read-only diagnostics, run by hand on the car; neither is part of a release.
dev-tools: evo-check evo-capture
evo-check: $(CHECK_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDSHLIB) -l:libdbus-1.so.3 -lpthread -lrt
# Passive SOME/IP recorder (tools/evo_capture.cpp: receive only, nothing sent).
evo-capture: $(CAPTURE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDSHLIB) -lrt

# Checks run on the PC. verify_binary and verify_wireless need the ARM programs and the copy of the car's system.
verify_repo:
	python3 tools/verify_repo.py
verify_core:
	python3 tools/verify_core.py
verify_binary:
	python3 tools/verify_binary.py
verify_wireless:
	python3 tools/verify_wireless.py

# Style tools: on the PATH, or in STYLE_TOOLS (a folder holding clang-format and ruff, e.g. a virtual environment's bin).
STYLE_TOOLS ?=
RUFF ?= $(if $(STYLE_TOOLS),$(STYLE_TOOLS)/ruff,ruff)
SHELLCHECK ?= shellcheck
style:
	python3 tools/format_sources.py --check $(if $(STYLE_TOOLS),--tools $(STYLE_TOOLS))
	$(RUFF) check .
	$(SHELLCHECK) -s sh -S style .githooks/pre-push
	$(SHELLCHECK) -S style toolchain/tc.sh
check: style verify_core verify_repo test-host

# ---- tests on the PC ------------------------------------------------------------------------------------------------
HOST_CXX ?= g++
HOST_FLAGS = -std=c++11 -O1 -g -Wall -Wextra -Werror -Iplatform -Icore -pthread
HOST_DBUS = $(shell pkg-config --cflags --libs dbus-1)
# The session as the fake-phone test links it; the car's TLS 1.2 client method is deprecated in the PC's OpenSSL 3.
SESSION_SRC = $(filter-out core/wireless_setup.cpp core/tcp_server.cpp,$(CORE_SRC))
HOST_SSL_FLAGS = -Wno-deprecated-declarations
# A BMW library read by one test: only on a PC holding a copy of the car system, never in git.
SPP_FIXTURE_LIB = $(HOME)/entrynav2/rootfs/opt/app/usr/BTMedia/evolution/stack/lib/libevo_btappl.so

# evo-check built for the PC: two tests drive it. Phony, so built once per make run, always from the current sources.
tests/evo-check-host:
	$(HOST_CXX) $(HOST_FLAGS) tools/evo_check.cpp platform/alps_spp.cpp platform/alps_spp_codec.cpp \
	    platform/evo_process.cpp $(HOST_DBUS) -o $@

test-host: test-parsers test-spp-host test-kombi-host test-gps-host test-capture-host test-usb-host \
           tests/evo-check-host
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_elf.cpp -o tests/test-evo-elf
	if [ -f "$(SPP_FIXTURE_LIB)" ]; then ENTRY_AAD_FIXTURE_LIB="$(SPP_FIXTURE_LIB)" ./tests/test-evo-elf; else ./tests/test-evo-elf; fi
	$(HOST_CXX) $(HOST_FLAGS) tests/test_spp_snapshot.cpp platform/evo_spp_snapshot.cpp -o tests/test-spp-snapshot
	if [ -f "$(SPP_FIXTURE_LIB)" ]; then ENTRY_AAD_FIXTURE_LIB="$(SPP_FIXTURE_LIB)" ./tests/test-spp-snapshot; else echo "SKIP test-spp-snapshot: no copy of the car system"; fi
	$(HOST_CXX) $(HOST_FLAGS) tests/test_supervisor.cpp platform/alps_spp_codec.cpp platform/usb_accessory.cpp \
	    -o tests/test-supervisor
	./tests/test-supervisor
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_ilm.cpp platform/evo_ilm.cpp platform/evo_process.cpp -o tests/test-evo-ilm
	ENTRY_AAD_DIAGNOSTIC=./tests/evo-check-host ./tests/test-evo-ilm
	$(HOST_CXX) $(HOST_FLAGS) tests/test_video_presentation.cpp platform/evo_video.cpp -o tests/test-video-presentation
	./tests/test-video-presentation
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_input.cpp platform/evo_touch.cpp platform/evo_knob.cpp platform/vmost_client.cpp \
	    -ldl -o tests/test-evo-input
	./tests/test-evo-input
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_control.cpp platform/evo_control.cpp -o tests/test-evo-control
	./tests/test-evo-control
	$(HOST_CXX) $(HOST_FLAGS) tests/test_peek_timing.cpp platform/peek_timing.cpp -o tests/test-peek-timing
	./tests/test-peek-timing
	$(HOST_CXX) $(HOST_FLAGS) -Icore/include tests/test_sdr_g20.cpp core/sdr_builder.cpp platform/evo_sdr_sensors.cpp -o tests/test-sdr-g20
	./tests/test-sdr-g20
	$(HOST_CXX) $(HOST_FLAGS) -Icore/include tests/test_evo_sdr.cpp core/sdr_builder.cpp platform/evo_sdr_sensors.cpp -o tests/test-evo-sdr
	./tests/test-evo-sdr
	gcc -std=c99 -Wall -Wextra -Werror -shared -fPIC tests/fake_vmostclient.c -o tests/fake-vmostclient.so
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_wlan.cpp platform/evo_wlan.cpp platform/vmost_client.cpp -ldl -o tests/test-evo-wlan
	./tests/test-evo-wlan $(CURDIR)/tests/fake-vmostclient.so
	$(HOST_CXX) $(HOST_FLAGS) tests/test_spawn_attr.cpp -o tests/test-spawn-attr
	./tests/test-spawn-attr
	$(HOST_CXX) $(HOST_FLAGS) tests/test_popup_peek.cpp -o tests/test-popup-peek
	./tests/test-popup-peek
	$(HOST_CXX) $(HOST_FLAGS) tests/test_hold_machine.cpp hmiapp/hold_machine.cpp -o tests/test-hold-machine
	./tests/test-hold-machine
	$(HOST_CXX) $(HOST_FLAGS) tests/test_pending_restart.cpp -o tests/test-pending-restart
	./tests/test-pending-restart
	$(HOST_CXX) $(HOST_FLAGS) tests/test_log_profile.cpp -o tests/test-log-profile
	./tests/test-log-profile
	$(HOST_CXX) $(HOST_FLAGS) tests/test_pb.cpp -o tests/test-pb
	./tests/test-pb
	$(HOST_CXX) $(HOST_FLAGS) tests/test_volume_pill.cpp -o tests/test-volume-pill
	./tests/test-volume-pill
	$(HOST_CXX) $(HOST_FLAGS) tests/test_aa_frame.cpp core/aa_frame.cpp -o tests/test-aa-frame
	./tests/test-aa-frame
	$(HOST_CXX) $(HOST_FLAGS) tests/test_aa_messages.cpp core/aa_messages.cpp -o tests/test-aa-messages
	./tests/test-aa-messages
	$(HOST_CXX) $(HOST_FLAGS) tests/test_tcp_server.cpp core/tcp_server.cpp -o tests/test-tcp-server
	./tests/test-tcp-server
	$(HOST_CXX) $(HOST_FLAGS) tests/test_wifi_link.cpp platform/wifi_link.cpp -o tests/test-wifi-link
	./tests/test-wifi-link
	$(HOST_CXX) $(HOST_FLAGS) $(HOST_SSL_FLAGS) -Icore/include tests/test_session_phone.cpp $(SESSION_SRC) -lssl -lcrypto \
	    -o tests/test-session-phone
	./tests/test-session-phone tests/fixtures/session_phone.golden
	$(HOST_CXX) $(HOST_FLAGS) tests/test_wireless_phone.cpp core/wireless_setup.cpp -o tests/test-wireless-phone
	./tests/test-wireless-phone tests/fixtures/wireless_phone.golden

test-parsers:
	$(HOST_CXX) $(HOST_FLAGS) tests/test_parsers.cpp platform/alps_spp_codec.cpp platform/evo_ap_config.cpp -o tests/test-parsers
	./tests/test-parsers

test-spp-host: tests/evo-check-host
	$(HOST_CXX) $(HOST_FLAGS) tests/test_alps_spp.cpp platform/alps_spp.cpp platform/alps_spp_codec.cpp platform/evo_process.cpp $(HOST_DBUS) -o tests/test-alps-spp
	env -u XDG_RUNTIME_DIR dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS="$$DBUS_SESSION_BUS_ADDRESS" ENTRY_AAD_TEST_BUS=1 ENTRY_AAD_DIAGNOSTIC=./tests/evo-check-host ./tests/test-alps-spp'

test-kombi-host:
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_phone.cpp platform/kombi_phone.cpp -o tests/test-kombi-phone
	./tests/test-kombi-phone
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_display.cpp platform/kombi_display.cpp platform/kombi_phone.cpp \
	    -o tests/test-kombi-display
	./tests/test-kombi-display
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_graph.cpp platform/kombi_graph.cpp platform/kombi_someip.cpp \
	    -o tests/test-kombi-graph
	./tests/test-kombi-graph
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_guide.cpp $(KOMBI_SRC) -o tests/test-kombi-guide
	./tests/test-kombi-guide
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_client.cpp $(KOMBI_SRC) -o tests/test-kombi-client
	./tests/test-kombi-client
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_nav.cpp $(KOMBI_SRC) -o tests/test-kombi-nav
	./tests/test-kombi-nav
	$(HOST_CXX) $(HOST_FLAGS) tests/test_bmw_guidance.cpp platform/evo_bmw_guidance.cpp $(HOST_DBUS) -o tests/test-bmw-guidance
	env -u XDG_RUNTIME_DIR dbus-run-session -- ./tests/test-bmw-guidance
	$(HOST_CXX) $(HOST_FLAGS) tests/test_kombi_golden.cpp $(KOMBI_SRC) -o tests/test-kombi-golden
	./tests/test-kombi-golden

# The USB cable: a fake bus (a folder like /sys/bus/usb/devices) and fake phones; no real device is opened.
test-usb-host:
	$(HOST_CXX) $(HOST_FLAGS) tests/test_usb_accessory.cpp platform/usb_accessory.cpp -o tests/test-usb-accessory
	./tests/test-usb-accessory
	$(HOST_CXX) $(HOST_FLAGS) tests/test_usb_bridge.cpp platform/usb_bridge.cpp core/aa_frame.cpp -o tests/test-usb-bridge
	./tests/test-usb-bridge
	$(HOST_CXX) $(HOST_FLAGS) tests/test_usb_loop.cpp platform/usb_loop.cpp platform/usb_bridge.cpp \
	    platform/usb_accessory.cpp platform/usb_devfs.cpp core/aa_frame.cpp -o tests/test-usb-loop
	./tests/test-usb-loop

test-gps-host:
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_gps.cpp -o tests/test-evo-gps
	./tests/test-evo-gps
	python3 tests/test_gps_wire_oracle.py

test-capture-host:
	$(HOST_CXX) $(HOST_FLAGS) tools/evo_capture.cpp -o tests/test-evo-capture
	./tests/test-evo-capture --selftest

# Root only (AF_PACKET): sudo make test-touch-live. Loopback traffic, no other interface touched.
test-touch-live:
	$(HOST_CXX) $(HOST_FLAGS) tests/test_evo_touch_live.cpp platform/evo_touch.cpp -o tests/test-evo-touch-live
	./tests/test-evo-touch-live

# ---- ARM tests, run on the PC under QEMU with the car's libraries (tests/run_qemu.py) --------------------------------
# test-spawn-attr-arm checks how the app starts the service from HmiMain3D with the HU's own glibc 2.15.
test-arm-all: dev-tools test-pb-arm-build test-sdr-arm-build test-kombi-arm-build test-session-silence-arm-build \
              test-presentation-arm-build test-wireless-lifecycle-arm-build test-gps-arm-build test-supervisor-arm-build \
              test-hmiapp-arm-build test-spawn-arm-build test-lm-cli-arm-build test-phone-arm-build \
              test-usb-arm-build test-wifi-arm-build

test-pb-arm-build:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_pb.cpp -o tests/test-pb-arm $(LDSHLIB) -lrt

test-spawn-arm-build:
	$(CXX) $(CXXFLAGS) tests/test_spawn_attr.cpp -o tests/test-spawn-attr-arm $(LDSHLIB)

test-hmiapp-arm-build: hmiapp/libEntryAad.so
	$(CXX) $(CXXFLAGS) tests/test_hmiapp_load.cpp -o tests/test-hmiapp-load-arm $(LDSHLIB) $(HMI_LIBDIRS) -ldl
	$(CXX) $(CXXFLAGS) tests/test_hold_machine.cpp hmiapp/hold_machine.cpp -o tests/test-hold-machine-arm $(LDSHLIB)

# LD_PRELOAD stub for the car's own LayerManagerControl (tests/test_real_lm_cli.py).
test-lm-cli-arm-build:
	arm-linux-gnueabihf-gcc $(ARCH) -std=c99 -Wall -Wextra -shared -fPIC tests/lm_cli_stub.c -o tests/lm-cli-stub.so

test-gps-arm-build:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_evo_gps.cpp -o tests/test-evo-gps-arm $(LDSHLIB) -lrt

test-supervisor-arm-build:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_spp_snapshot.cpp platform/evo_spp_snapshot.cpp -o tests/test-spp-snapshot-arm $(LDSHLIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_evo_elf.cpp -o tests/test-evo-elf-arm $(LDSHLIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_supervisor.cpp platform/alps_spp_codec.cpp platform/usb_accessory.cpp \
	    -o tests/test-supervisor-arm $(LDSHLIB) -lrt

test-usb-arm-build: $(USB_OBJ) core/aa_frame.o
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_usb_accessory.cpp platform/usb_accessory.o -o tests/test-usb-accessory-arm \
	    $(LDSHLIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_usb_bridge.cpp platform/usb_bridge.o core/aa_frame.o \
	    -o tests/test-usb-bridge-arm $(LDSHLIB) -lpthread -lrt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_usb_loop.cpp platform/usb_loop.o platform/usb_bridge.o \
	    platform/usb_accessory.o platform/usb_devfs.o core/aa_frame.o -o tests/test-usb-loop-arm $(LDSHLIB) -lpthread -lrt

test-wifi-arm-build: platform/wifi_link.o
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_wifi_link.cpp platform/wifi_link.o -o tests/test-wifi-link-arm \
	    $(LDSHLIB) -lpthread -lrt

test-kombi-arm-build: $(KOMBI_OBJ)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_phone.cpp platform/kombi_phone.o -o tests/test-kombi-phone-arm $(LDSHLIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_display.cpp platform/kombi_display.o platform/kombi_phone.o \
	    -o tests/test-kombi-display-arm $(LDSHLIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_graph.cpp platform/kombi_graph.o platform/kombi_someip.o \
	    -o tests/test-kombi-graph-arm $(LDSHLIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_guide.cpp $(KOMBI_OBJ) -o tests/test-kombi-guide-arm $(LDSHLIB) -lpthread -lrt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_client.cpp $(KOMBI_OBJ) -o tests/test-kombi-client-arm $(LDSHLIB) -lpthread -lrt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_nav.cpp $(KOMBI_OBJ) -o tests/test-kombi-nav-arm $(LDSHLIB) -lpthread -lrt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_kombi_golden.cpp $(KOMBI_OBJ) -o tests/test-kombi-golden-arm $(LDSHLIB) -lpthread -lrt

test-sdr-arm-build:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_sdr_g20.cpp core/sdr_builder.cpp platform/evo_sdr_sensors.cpp -o tests/test-sdr-g20-arm $(LDSHLIB)

# A fake phone against the session (the car's libssl on both sides) and against the Bluetooth handshake.
test-phone-arm-build: $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_session_phone.cpp $(filter-out core/wireless_setup.o core/tcp_server.o,$(CORE_OBJ)) \
	    -o tests/test-session-phone-arm $(LDSHLIB) -lpthread -lrt -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -ldl
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_wireless_phone.cpp core/wireless_setup.o -o tests/test-wireless-phone-arm \
	    $(LDSHLIB) -lpthread -lrt

test-session-silence-arm-build: $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -ffunction-sections -fdata-sections tests/test_session_silence.cpp \
	    $(filter-out core/wireless_setup.o,$(CORE_OBJ)) \
	    -Wl,--gc-sections -o tests/test-session-silence-arm $(LDSHLIB) -lpthread -lrt -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -ldl

test-presentation-arm-build:
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_video_presentation.cpp platform/evo_video.cpp -o tests/test-video-presentation-arm $(LDSHLIB) -lpthread -lrt
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_evo_input.cpp platform/evo_touch.cpp platform/evo_knob.cpp platform/vmost_client.cpp \
	    -o tests/test-evo-input-arm $(LDSHLIB) -lpthread -ldl -lrt

# service_app and wireless_loop are compiled here in sections: what the test never calls (the SPP refresh) drops out.
test-wireless-lifecycle-arm-build: $(CORE_OBJ) $(LIFECYCLE_OBJ)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -ffunction-sections -fdata-sections tests/test_wireless_lifecycle.cpp \
	    platform/service_app.cpp platform/wireless_loop.cpp $(filter-out core/wireless_setup.o,$(CORE_OBJ)) $(LIFECYCLE_OBJ) \
	    -Wl,--gc-sections -o tests/test-wireless-lifecycle-arm $(LDSHLIB) $(VIDEO_LIBS) -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libdbus-1.so.3 -ldl

clean:
	rm -f $(ARM_OBJ) $(ARM_OBJ:.o=.d) $(HMI_OBJ) $(HMI_OBJ:.o=.d) entry-aad aa-supervisor evo-check evo-capture \
	    hmiapp/libEntryAad.so hmiapp/EntryAad_ALL_Texts.zip hmiapp/EntryAad_ALL_Images.zip \
	    tests/test-* tests/evo-check-host tests/*.so

-include $(ARM_OBJ:.o=.d) $(HMI_OBJ:.o=.d)

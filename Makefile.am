AUTOMAKE_OPTIONS=foreign
DISTCLEANFILES=*~
AM_CPPFLAGS=-I$(builddir)

TESTS=serial_test seqpacket_test
check_PROGRAMS=$(TESTS)
bin_PROGRAMS=listen serial seqpacket decode seqpacket_client seqpacket_server router_hub
lib_LTLIBRARIES=libpreload.la

libpreload_la_SOURCES=\
src/preload.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc
libpreload_la_CPPFLAGS=-fPIC
libpreload_la_CXXFLAGS=-fPIC
libpreload_la_LDFLAGS=-shared -fPIC -Wl,-z,defs -Wl,--as-needed -ldl -lax25 -lpthread

listen_SOURCES=\
src/main.cc \
src/listen.cc \
src/parse.cc \
src/mic-e.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

router_hub_SOURCES=\
src/main.cc \
src/router_hub.cc \
src/parse.cc \
src/mic-e.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

decode_SOURCES=\
src/main.cc \
src/decode.cc \
src/parse.cc \
src/mic-e.cc \
src/aprs.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

serial_SOURCES=\
src/main.cc \
src/serial.cc \
src/util.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

serial_test_SOURCES=\
src/serial_test.cc \
src/serial.cc \
src/util.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

seqpacket_SOURCES=\
src/main.cc \
src/seqpacket.cc \
src/serialize.cc \
src/parse.cc \
src/scheduler.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

seqpacket_test_SOURCES=\
src/seqpacket_test.cc \
src/seqpacket.cc \
src/serialize.cc \
src/parse.cc \
src/scheduler.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

seqpacket_client_SOURCES=\
src/main.cc \
src/seqpacket_client.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc

seqpacket_server_SOURCES=\
src/main.cc \
src/seqpacket_server.cc \
src/util.cc \
proto/gen/ax25.pb.cc \
proto/gen/aprs.pb.cc \
proto/gen/api.grpc.pb.cc \
proto/gen/api.pb.cc


lsp:
	~/.local/bin/intercept-build make -j$(shell nproc)

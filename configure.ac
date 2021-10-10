# -*- Autoconf -*-

AC_PREREQ(2.61)
AC_INIT(ax25ms, 0.01, thomas@habets.se)
AC_CANONICAL_SYSTEM
AC_CONFIG_SRCDIR([src/listen.cc])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_MAINTAINER_MODE
AC_CONFIG_HEADER(src/config.h)

# Check for programs.
AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MAKE_SET
PKG_PROG_PKG_CONFIG

# Check for libraries.
AC_LANG_CPLUSPLUS

AC_CHECK_PROG([PROTOC], [protoc], [protoc])
#LDFLAGS="$LDFLAGS -lgrpc++"
AS_IF([test "x${PROTOC}" == "x"],
    [AC_MSG_ERROR([ProtoBuf compiler "protoc" not found.])])

PKG_CHECK_MODULES(PROTOBUF, protobuf >= 2.4.0)
LIBS="$LIBS $PROTOBUF_LIBS"
CXXFLAGS="$CXXFLAGS $PROTOBUF_CXXFLAGS $PROTOBUF_CFLAGS"

PKG_CHECK_MODULES(GRPCPP, grpc++ >= 1.30.0)
LIBS="$LIBS $GRPCPP_LIBS"
CXXFLAGS="$CXXFLAGS $GRPCPP_CXXFLAGS $GRPCPP_CFLAGS"

# Check for header files.
AC_CHECK_HEADERS([\
signal.h \
sys/socket.h \
sys/types.h \
grp.h \
pwd.h \
unistd.h \
google/protobuf/stubs/logging.h \
google/protobuf/stubs/common.h \
])

AC_TYPE_SIGNAL

CXXFLAGS="$CXXFLAGS -std=c++20 -g -Wall -pedantic"

# Output
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

# Status
echo "
  $PACKAGE_NAME version $PACKAGE_VERSION
  Prefix.........: $prefix
  Debug Build....: $debug
  C++ Compiler...: $CXX $CXXFLAGS $CPPFLAGS
  Linker.........: $LD $LDFLAGS $LIBS
"

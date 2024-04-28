# AX.25 microservices

This project is a set of AX.25 microservices, to be pluggable for any
implementation.

This is not an official Google product.

## Background

AX.25 on Linux is centred around the `AF_AX25` socket types. That
requires a kernel implementation.

I've found the Linux kernel AX.25 socket implementation to have many flaws.
And other operating systems don't have support at all.

The logical conclusion, therefore is to rewrite it in userspace.

## Microservices

There are standards for sending AX.25 over IP. For example
[RFC1226][rfc1226]. But what I want to do here is to have a strict
interface between more structured data, and run it as microservices.

E.g. there's no reason an AX.25 router would need to parse MicE APRS
messages, so it can just treat it as payload.

And if one wants to write a something more examining in Python, then
if there's an RPC interface it doesn't matter in what language it's
written. No need for pybind11 or SWIG.

## State of the code

Missing features:

* REJ handling
* SREJ handling and sending
* Extended sequence numbers
* Setting window size, timer lengths, digipeater list, etc…
* Whole parts of the state machine are no-ops (though not all parts
  are needed)

Still, I can't say it's more buggy than the Linux kernel
implementation. E.g. you can DoS the kernel version by building up a
window and them spamming REJ. You can send 10 REJs and the kernel
implementation will re-send the whole window 10 times.

[axsh][axsh] works on the server and client side.

```
 tnc <KISS> serial <gRPC> seqpacket <gRPC> ax25ms_axsh
```

## Protobuf

Protobufs are a bit annoying latest thing that worked was:

```
cd proto
protoc \
    --cpp_out=gen \
    --grpc_out=gen \
    --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
    *.proto
```

## How to test it

If you have an existing program that uses `AX25_SEQPACKET`, and a TNC
on a serial port (e.g. one created by direwolf), then you should be
able to start the setup using something like:

### Start the serial port interface

For my Kenwood TH-D74 via bluetooth, this is:

```
$ ./serial -p /dev/rfcomm0 -l '[::]:12001'
```

### Start the seqpacket daemon, using the serial service as a "router"

```
$ ./seqpacket -r localhost:12001 -l '[::]:12002'
```

### Run your program using libpreload

```
$ export AX25_ADDR=M0XXX-1        # Address used by the interface.
$ export AX25_ROUTER=localhost:12002  # seqpacket service location
$ LD_PRELOAD=$(pwd)/libpreload.so axsh -r radio -s M0XXX-9 2E0XXX-3
```

If that works you can make the `LD_PRELOAD` permanent on a binary
with:

```
$ patchelf --add-needed $(pwd)/libpreload.so $(which axsh)
```

You'll still have to set `AX25_ADDR` and `AX25_ROUTER` of course.

[rfc1226]: https://datatracker.ietf.org/doc/html/rfc1226
[axsh]: https://github.com/ThomasHabets/radiostuff/tree/master/ax25/axsh

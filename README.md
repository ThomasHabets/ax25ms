# AX.25 microservices

This project is a set of AX.25 microservices, to be pluggable for any
implementation.

This is not an official Google product.

## Background

AX.25 on Linux is centred around the `AF_AX25` socket types. That requires a kernel implementation.

I've found the Linux kernel AX.25 socket implementation to have many flaws.
And other operating systems don't have support at all.

The logical conclusion, therefore is to rewrite it in userspace.

## Microservices

There are standards for sending AX.25 over IP. For example [RFC1226][rfc1226]. But
what I want to do here is to have a strict interface between more structured data, and
run it as microservices.

E.g. there's no reason an AX.25 router would need to parse MicE APRS messages, so
it can just treat it as payload.

And if one wants to write a something more examining in Python, then if there's
an RPC interface it doesn't matter in what language it's written. No need for
pybind11 or SWIG.

## State of the code

It's pretty messy. What I aim to get working is this:

![Alt text](https://g.gravizo.com/source/custom_mark10?https%3A%2F%2Fraw.githubusercontent.com%2FThomasHabets%2Fax25ms%2Fmain%2FREADME.md)
<details> 
<summary></summary>
  digraph G {
    tnc -> ax25ms_serial;
    ax25ms_serial -> ax25ms_seqpacket;
    ax25ms_seqpacket -> ax25ms_axsh;
  }
</details>


[rfc1226]: https://datatracker.ietf.org/doc/html/rfc1226

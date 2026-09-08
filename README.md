# RTT HTTP service

This package implements an independent HTTP/REST interface for Orocos RTT
components. OCL owns its component service plugin and deployment lifetime.
The HTTP package owns JSON codecs, explicit static publication, the listener,
and bounded operation execution. It does not link to the OPC UA transport.

Implementation is in progress; distribution integration and native platform
validation are required before treating this as a released deployment API.

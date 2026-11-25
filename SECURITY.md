# Security and Safety Policy

This project controls mains power to a pump on a pressurised espresso machine, so a software bug
can be a physical safety problem. Please report the following privately:

- anything that can energise the pump when it should be off: a stuck TRIAC gate, a failing
  interlock, or a power-stage state that is not cut on fault;
- a way to bypass the over-pressure, sensor-fault or maximum-time cut-offs;
- a way for another device or web page on the network to start the pump, change the configuration
  or install firmware without the owner's action;
- anything else that could cause electric shock, fire, or a scalding release of water or steam.

## How to report

Use GitHub's **private vulnerability reporting** for this repository: the **Security** tab, then
**Report a vulnerability**. Include the firmware version (`status` on the serial console), the
hardware involved, the steps to reproduce, and the relevant log lines.

Please do not open a public issue for these until a fix is available. Ordinary bugs that cannot
make the hardware unsafe can go to the public issue tracker.

## Supported versions

Only the latest release on the default branch receives fixes.

## Scope

The controller's local web API is designed for a trusted home network. It rejects cross-origin
requests, but it has no user authentication. Do not expose it to the internet.

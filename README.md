# CoFUSO
# CoFUSO's codes
Currently, CoFUSO is a kernel transport implementation which is built upon MPTCP’s Linux implementation (v0.90).
## Installation
At first, please install MPTCP Linux kernel version (v0.90).

Next, download the patch files (i.e. the modified kernel code files). Then copy them in the mptcp kernel folder and overwrite the original files.

Then, you need to compile and install the modified kernel.
## Enabling CoFUSO
CoFUSO is turned off by default. To enable it:
systcl net.mptcp.mptcp_rmt=1
systcl net.mptcp.mptcp_coding=1
systcl net.mptcp.mptcp_decoding=1

If you want to use FUSO's optimization for the original MPTCP receiving end, enable the receiver optimization:
sysctl net.mptcp.mptcp_receive_ofo_optimize=1

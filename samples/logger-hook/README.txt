Title: Logger application hook sample

Description:

A simple example on how to use logger hook in order to redirect the logger
output to various backends.
In this case, the logger is sending the output to ringbuf,
logger output can be sent to a driver such as UART / USIF etc. as well.

--------------------------------------------------------------------------------

Building and Running Project:

This unified project outputs to the console.
It can be built and executed on QEMU as follows:

    make qemu

--------------------------------------------------------------------------------

Troubleshooting:

Problems caused by out-dated project information can be addressed by
issuing one of the following commands then rebuilding the project:

    make clean          # discard results of previous builds
                        # but keep existing configuration info
or
    make pristine       # discard results of previous builds
                        # and restore pre-defined configuration info

--------------------------------------------------------------------------------

Sample Output:

[sample-unified] [ERR] main: SYS LOG ERR is ACTIVE
[sample-unified] [WRN] main: SYS LOG WRN is ACTIVE
[sample-unified] [INF] main: SYS LOG INF is ACTIVE
